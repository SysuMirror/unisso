#include "core.h"

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace {

std::string absolute(const std::string& path) {
    return AppConfig::get().issuer + path;
}

std::string appendQuery(const std::string& uri, const std::string& query) {
    if (query.empty()) return uri;
    return uri + (uri.find('?') == std::string::npos ? "?" : "&") + query;
}

bool redirectAllowed(const Application& app, const std::string& uri) {
    return std::find(app.redirectUris.begin(), app.redirectUris.end(), uri) != app.redirectUris.end();
}

bool validPkce(const std::string& challenge, const std::string& method) {
    static const std::regex shape(R"(^[A-Za-z0-9_\-]{43}$)");
    return method == "S256" && std::regex_match(challenge, shape);
}

bool verifyPkce(const std::string& verifier, const std::string& challenge) {
    if (verifier.empty()) return false;
    return util::constantTimeEquals(util::base64UrlEncode(util::sha256Raw(verifier)), challenge);
}

std::string issueCode(const std::string& clientId, const std::string& userId,
                      const std::string& redirectUri, const std::string& scope,
                      const std::string& challenge, const std::string& method,
                      const std::string& state, const std::string& nonce) {
    std::map<std::string, JsonValue> data;
    data["client_id"] = JsonValue(clientId);
    data["user_id"] = JsonValue(userId);
    data["redirect_uri"] = JsonValue(redirectUri);
    data["scope"] = JsonValue(scope);
    data["code_challenge"] = JsonValue(challenge);
    data["code_challenge_method"] = JsonValue(method);
    data["state"] = JsonValue(state);
    data["nonce"] = JsonValue(nonce);
    std::string code = util::randomToken(32);
    Cache::set("auth_code:" + code, jsonx::dump(data),
               AppConfig::get().authCodeSeconds);
    return code;
}

std::optional<std::map<std::string, JsonValue>> consumeCode(const std::string& code) {
    std::string raw = Cache::getdel("auth_code:" + code);
    if (raw.empty()) return std::nullopt;
    std::map<std::string, JsonValue> data;
    if (!jsonx::parseObject(raw, data)) return std::nullopt;
    return data;
}

std::string redirectUrl(const std::string& uri, const std::string& code,
                        const std::string& state) {
    std::string query = "code=" + util::urlEncode(code);
    if (!state.empty()) query += "&state=" + util::urlEncode(state);
    return appendQuery(uri, query);
}

void oauthError(Response& res, int status, const std::string& error,
                const std::string& description = "") {
    std::map<std::string, JsonValue> obj;
    obj["error"] = JsonValue(error);
    if (!description.empty()) obj["error_description"] = JsonValue(description);
    http::json(res, status, JsonValue(obj));
}

std::vector<std::string> scopeItems(const std::vector<std::string>& values) {
    return values;
}

JsonValue scopeListJson(const std::vector<std::string>& values) {
    std::vector<JsonValue> items;
    for (const auto& name : values) {
        std::map<std::string, JsonValue> item;
        item["name"] = JsonValue(name);
        item["description"] = JsonValue(scopes::description(name));
        item["required"] = JsonValue(name == "openid");
        items.push_back(JsonValue(item));
    }
    return JsonValue(items);
}

struct AuthorizeRequest {
    std::string clientId;
    std::string redirectUri;
    std::string responseType;
    std::string scope;
    std::string state;
    std::string challenge;
    std::string challengeMethod;
    std::string nonce;
};

AuthorizeRequest readAuthorize(const Request& req, bool fromQuery) {
    auto get = [&req, fromQuery](const std::string& key) {
        return fromQuery ? http::query(req, key) : http::input(req, key);
    };
    AuthorizeRequest value;
    value.clientId = util::trim(get("client_id"));
    value.redirectUri = util::trim(get("redirect_uri"));
    value.responseType = util::trim(get("response_type"));
    value.scope = util::trim(get("scope"));
    value.state = get("state");
    value.challenge = util::trim(get("code_challenge"));
    value.challengeMethod = util::trim(get("code_challenge_method"));
    value.nonce = get("nonce");
    return value;
}

bool validateAuthorize(const Request& req, bool fromQuery,
                       const User& user, AuthorizeRequest& ar,
                       Application& app, std::vector<std::string>& candidate,
                       std::string& errorCode, std::string& message) {
    ar = readAuthorize(req, fromQuery);
    if (ar.clientId.empty() || ar.redirectUri.empty()) {
        errorCode = "invalid_request";
        message = "缺少 client_id 或 redirect_uri";
        return false;
    }
    auto found = repo::findApplicationByClientId(ar.clientId);
    if (!found || !found->isActive) {
        errorCode = "invalid_client";
        message = "应用不存在或已停用";
        return false;
    }
    app = *found;
    if (!redirectAllowed(app, ar.redirectUri)) {
        errorCode = "invalid_redirect_uri";
        message = "回调地址与应用注册信息不匹配";
        return false;
    }
    if (ar.responseType != "code") {
        errorCode = "unsupported_response_type";
        message = "仅支持 code 授权模式";
        return false;
    }
    if (!validPkce(ar.challenge, ar.challengeMethod)) {
        errorCode = "invalid_request";
        message = "必须使用 S256 PKCE";
        return false;
    }
    auto requested = scopes::split(ar.scope);
    candidate = scopes::effective(requested, app.allowedScopes);
    if (candidate.empty()) {
        errorCode = "invalid_scope";
        message = "请求的权限范围无效";
        return false;
    }
    // A consent can only be auto-approved for scopes it already covers.
    return true;
}

bool verifyClient(const Application& app, const std::string& clientSecret,
                  std::string& errorCode) {
    if (!app.isActive) {
        errorCode = "invalid_client";
        return false;
    }
    if (app.isConfidential) {
        if (clientSecret.empty() || !password::verify(clientSecret, app.clientSecret)) {
            errorCode = "invalid_client";
            return false;
        }
    } else if (!clientSecret.empty()) {
        errorCode = "invalid_client";
        return false;
    }
    return true;
}

std::pair<std::string, std::string> basicCredentials(const Request& req) {
    std::string header = http::header(req, "Authorization");
    if (header.rfind("Basic ", 0) != 0) return {"", ""};
    std::string decoded = util::base64Decode(header.substr(6));
    size_t colon = decoded.find(':');
    if (colon == std::string::npos) return {"", ""};
    return {util::urlDecode(decoded.substr(0, colon)), util::urlDecode(decoded.substr(colon + 1))};
}

}  // namespace

namespace handlers {

void oauthContext(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    AuthorizeRequest ar;
    Application app;
    std::vector<std::string> candidate;
    std::string errorCode, message;
    if (!validateAuthorize(req, true, user, ar, app, candidate, errorCode, message)) {
        http::error(res, 400, errorCode, message);
        return;
    }

    std::vector<User> peers = repo::linkedPeers(user.id);
    auto accountJson = [&](const User& account) {
        std::map<std::string, JsonValue> item;
        item["id"] = JsonValue(account.id);
        item["email"] = JsonValue(account.email);
        item["username"] = JsonValue(account.username);
        item["full_name"] = JsonValue(account.fullName);
        item["avatar"] = JsonValue(account.avatar);
        item["is_current"] = JsonValue(account.id == user.id);
        // Accounts can bind several marketplace identities; the consent page
        // asks which identity to present (sse_sub claim) when more than one.
        std::vector<JsonValue> idents;
        for (const auto& identity : sse::listSseIdentities(account)) {
            std::map<std::string, JsonValue> entry;
            entry["id"] = JsonValue(identity.userId);
            entry["name"] = JsonValue(identity.name);
            entry["email"] = JsonValue(identity.email);
            entry["is_current"] = JsonValue(identity.current);
            idents.push_back(JsonValue(entry));
        }
        item["sse_identities"] = JsonValue(idents);
        return JsonValue(item);
    };
    std::vector<JsonValue> accounts;
    accounts.push_back(accountJson(user));
    for (const auto& peer : peers) accounts.push_back(accountJson(peer));

    // Always show the consent screen. A previous grant must not silently
    // re-issue a code: the user expects to review the app and click Allow.
    std::map<std::string, JsonValue> obj;
    obj["auto_approved"] = JsonValue(false);
    obj["application"] = repo::applicationJson(app, false);
    obj["scopes"] = scopeListJson(candidate);
    obj["accounts"] = JsonValue(accounts);
    obj["current_user_id"] = JsonValue(user.id);
    std::map<std::string, JsonValue> request;
    request["client_id"] = JsonValue(ar.clientId);
    request["redirect_uri"] = JsonValue(ar.redirectUri);
    request["scope"] = JsonValue(ar.scope);
    request["state"] = JsonValue(ar.state);
    request["code_challenge"] = JsonValue(ar.challenge);
    request["code_challenge_method"] = JsonValue(ar.challengeMethod);
    request["nonce"] = JsonValue(ar.nonce);
    obj["request"] = JsonValue(request);
    http::json(res, 200, JsonValue(obj));
}

void oauthAuthorize(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    AuthorizeRequest ar;
    Application app;
    std::vector<std::string> candidate;
    std::string errorCode, message;
    if (!validateAuthorize(req, false, user, ar, app, candidate, errorCode, message)) {
        http::error(res, 400, errorCode, message);
        return;
    }

    std::string approvedValue = http::input(req, "approved");
    bool approved = approvedValue.empty() || approvedValue == "true" || approvedValue == "1";
    if (!approved) {
        std::string query = "error=access_denied";
        if (!ar.state.empty()) query += "&state=" + util::urlEncode(ar.state);
        std::map<std::string, JsonValue> obj;
        obj["redirect_url"] = JsonValue(appendQuery(ar.redirectUri, query));
        http::json(res, 200, JsonValue(obj));
        return;
    }

    std::vector<std::string> requested = http::inputArray(req, "scopes");
    if (requested.empty()) requested = candidate;
    std::vector<std::string> effective = scopes::effective(requested, app.allowedScopes);
    // Only scopes that were shown to the user are allowed.
    for (auto it = effective.begin(); it != effective.end();) {
        if (std::find(candidate.begin(), candidate.end(), *it) == candidate.end()) {
            it = effective.erase(it);
        } else {
            ++it;
        }
    }
    if (effective.empty()) {
        http::error(res, 400, "invalid_scope", "至少需要保留一项权限");
        return;
    }

    std::string asUserId = util::trim(http::input(req, "as_user_id"));
    User selected = user;
    if (!asUserId.empty() && asUserId != user.id) {
        auto peers = repo::linkedPeers(user.id);
        bool found = false;
        for (const auto& peer : peers) {
            if (peer.id == asUserId) { selected = peer; found = true; break; }
        }
        if (!found) {
            http::error(res, 403, "forbidden", "不能以该账号授权");
            return;
        }
        if (!selected.isActive) {
            http::error(res, 400, "inactive_account", "该账号已停用");
            return;
        }
    }

    // Which marketplace identity (sse_sub) this grant should present. Only
    // ids already bound to the selected account may be chosen; empty keeps
    // the account's current identity.
    std::string asSse = util::trim(http::input(req, "as_sse_identity"));
    if (!asSse.empty()) {
        std::string identityError;
        if (!sse::selectSseIdentity(selected, asSse, identityError)) {
            http::error(res, 403, "invalid_sse_identity", "未绑定该集市身份，请重新选择");
            return;
        }
        auto refreshed = repo::findUserById(selected.id);
        if (refreshed) selected = *refreshed;
    }

    if (!repo::upsertConsent(selected.id, app.id, effective)) {
        http::error(res, 500, "consent_failed", "无法保存授权记录");
        return;
    }
    std::string code = issueCode(app.clientId, selected.id, ar.redirectUri,
                                 scopes::join(effective), ar.challenge,
                                 ar.challengeMethod, ar.state, ar.nonce);
    std::map<std::string, JsonValue> obj;
    obj["redirect_url"] = JsonValue(redirectUrl(ar.redirectUri, code, ar.state));
    http::json(res, 200, JsonValue(obj));
}

void oauthToken(const Request& req, Response& res) {
    std::string grantType = http::input(req, "grant_type");
    std::string clientId = http::input(req, "client_id");
    std::string clientSecret = http::input(req, "client_secret");
    auto basic = basicCredentials(req);
    if (clientId.empty()) clientId = basic.first;
    if (clientSecret.empty()) clientSecret = basic.second;

    if (grantType == "authorization_code") {
        std::string code = http::input(req, "code");
        std::string redirectUri = http::input(req, "redirect_uri");
        std::string verifier = http::input(req, "code_verifier");
        if (code.empty()) {
            oauthError(res, 400, "invalid_request", "missing code");
            return;
        }
        auto data = consumeCode(code);
        if (!data) {
            oauthError(res, 400, "invalid_grant", "authorization code invalid or expired");
            return;
        }
        std::string codeClient = jsonx::getString(*data, "client_id");
        std::string codeRedirect = jsonx::getString(*data, "redirect_uri");
        if (clientId.empty()) clientId = codeClient;
        if (clientId != codeClient) {
            oauthError(res, 400, "invalid_grant", "client mismatch");
            return;
        }
        auto app = repo::findApplicationByClientId(codeClient);
        if (!app) {
            oauthError(res, 400, "invalid_client");
            return;
        }
        std::string clientError;
        if (!verifyClient(*app, clientSecret, clientError)) {
            oauthError(res, 401, "invalid_client");
            return;
        }
        if (!redirectUri.empty() && redirectUri != codeRedirect) {
            oauthError(res, 400, "invalid_grant", "redirect_uri mismatch");
            return;
        }
        std::string challenge = jsonx::getString(*data, "code_challenge");
        std::string method = jsonx::getString(*data, "code_challenge_method");
        if (!validPkce(challenge, method) || !verifyPkce(verifier, challenge)) {
            oauthError(res, 400, "invalid_grant", "PKCE verification failed");
            return;
        }
        auto user = repo::findUserById(jsonx::getString(*data, "user_id"));
        if (!user || !user->isActive) {
            oauthError(res, 400, "invalid_grant", "user unavailable");
            return;
        }
        auto granted = scopes::split(jsonx::getString(*data, "scope"));
        JsonValue response;
        if (!tokens::issue(*user, *app, granted, true,
                           jsonx::getString(*data, "nonce"), response)) {
            oauthError(res, 500, "server_error", "token persistence failed");
            return;
        }
        http::json(res, 200, response);
        return;
    }

    if (grantType == "refresh_token") {
        std::string refresh = http::input(req, "refresh_token");
        if (refresh.empty()) {
            oauthError(res, 400, "invalid_request", "missing refresh_token");
            return;
        }
        auto payload = tokens::decode(refresh, "refresh");
        if (!payload) {
            oauthError(res, 400, "invalid_grant");
            return;
        }
        std::string jti = jsonx::getString(*payload, "jti");
        if (tokens::revoked(jti, "refresh")) {
            oauthError(res, 400, "invalid_grant", "refresh token revoked");
            return;
        }
        std::string codeClient = jsonx::getString(*payload, "client_id");
        if (clientId.empty()) clientId = codeClient;
        if (clientId != codeClient) {
            oauthError(res, 400, "invalid_grant", "client mismatch");
            return;
        }
        auto app = repo::findApplicationByClientId(codeClient);
        if (!app) {
            oauthError(res, 400, "invalid_client");
            return;
        }
        std::string clientError;
        if (!verifyClient(*app, clientSecret, clientError)) {
            oauthError(res, 401, "invalid_client");
            return;
        }
        auto user = repo::findUserById(jsonx::getString(*payload, "sub"));
        if (!user || !user->isActive) {
            oauthError(res, 400, "invalid_grant", "user unavailable");
            return;
        }
        Db::exec("UPDATE refresh_tokens SET revoked=1 WHERE token=" + Db::quote(jti));
        auto granted = scopes::split(jsonx::getString(*payload, "scope"));
        JsonValue response;
        if (!tokens::issue(*user, *app, granted, false, "", response)) {
            oauthError(res, 500, "server_error", "token persistence failed");
            return;
        }
        http::json(res, 200, response);
        return;
    }

    oauthError(res, 400, "unsupported_grant_type");
}

void oauthIntrospect(const Request& req, Response& res) {
    std::string clientId = http::input(req, "client_id");
    std::string clientSecret = http::input(req, "client_secret");
    auto basic = basicCredentials(req);
    if (clientId.empty()) clientId = basic.first;
    if (clientSecret.empty()) clientSecret = basic.second;
    auto app = repo::findApplicationByClientId(clientId);
    std::string clientError;
    if (!app || !verifyClient(*app, clientSecret, clientError)) {
        oauthError(res, 401, "invalid_client");
        return;
    }
    std::string token = http::input(req, "token");
    std::string hint = http::input(req, "token_type_hint");
    std::map<std::string, JsonValue> obj;
    obj["active"] = JsonValue(false);
    if (token.empty()) {
        http::json(res, 200, JsonValue(obj));
        return;
    }
    auto payload = tokens::decode(token, hint == "refresh_token" ? "refresh" : "access");
    if (!payload) payload = tokens::decode(token, "refresh");
    if (!payload) {
        http::json(res, 200, JsonValue(obj));
        return;
    }
    if (jsonx::getString(*payload, "client_id") != app->clientId) {
        http::json(res, 200, JsonValue(obj));
        return;
    }
    std::string type = jsonx::getString(*payload, "type");
    std::string jti = jsonx::getString(*payload, "jti");
    if (tokens::revoked(jti, type)) {
        http::json(res, 200, JsonValue(obj));
        return;
    }
    obj["active"] = JsonValue(true);
    obj["scope"] = JsonValue(jsonx::getString(*payload, "scope"));
    obj["client_id"] = JsonValue(jsonx::getString(*payload, "client_id"));
    obj["sub"] = JsonValue(jsonx::getString(*payload, "sub"));
    obj["token_type"] = JsonValue(type);
    obj["iss"] = JsonValue(jsonx::getString(*payload, "iss"));
    obj["aud"] = JsonValue(jsonx::getString(*payload, "aud"));
    obj["jti"] = JsonValue(jti);
    auto exp = payload->find("exp");
    if (exp != payload->end() && exp->second.getType() == JsonValue::NUMBER) {
        obj["exp"] = jsonx::num(static_cast<long long>(exp->second.asNumber()));
    }
    auto user = repo::findUserById(jsonx::getString(*payload, "sub"));
    if (user) obj["username"] = JsonValue(user->username);
    http::json(res, 200, JsonValue(obj));
}

void oauthUserinfo(const Request& req, Response& res) {
    std::string header = http::header(req, "Authorization");
    std::string token;
    if (header.rfind("Bearer ", 0) == 0) token = util::trim(header.substr(7));
    if (token.empty()) {
        res.headers["WWW-Authenticate"] = "Bearer error=\"invalid_token\"";
        oauthError(res, 401, "invalid_token");
        return;
    }
    auto payload = tokens::decode(token, "access");
    if (!payload) {
        res.headers["WWW-Authenticate"] = "Bearer error=\"invalid_token\"";
        oauthError(res, 401, "invalid_token");
        return;
    }
    if (tokens::revoked(jsonx::getString(*payload, "jti"), "access")) {
        res.headers["WWW-Authenticate"] = "Bearer error=\"invalid_token\"";
        oauthError(res, 401, "invalid_token");
        return;
    }
    auto user = repo::findUserById(jsonx::getString(*payload, "sub"));
    if (!user || !user->isActive) {
        oauthError(res, 401, "invalid_token");
        return;
    }
    auto granted = scopes::split(jsonx::getString(*payload, "scope"));
    http::json(res, 200, scopes::userClaims(*user, granted));
}

void oidcDiscovery(const Request&, Response& res) {
    auto& cfg = AppConfig::get();
    std::map<std::string, JsonValue> obj;
    obj["issuer"] = JsonValue(cfg.issuer);
    obj["authorization_endpoint"] = JsonValue(absolute("/authorize"));
    obj["token_endpoint"] = JsonValue(absolute("/api/oauth/token"));
    obj["userinfo_endpoint"] = JsonValue(absolute("/api/oauth/userinfo"));
    obj["introspection_endpoint"] = JsonValue(absolute("/api/oauth/introspect"));
    obj["jwks_uri"] = JsonValue(absolute("/.well-known/jwks.json"));
    obj["response_types_supported"] = JsonValue(std::vector<std::string>{"code"});
    obj["grant_types_supported"] = JsonValue(std::vector<std::string>{"authorization_code", "refresh_token"});
    obj["subject_types_supported"] = JsonValue(std::vector<std::string>{"public"});
    obj["id_token_signing_alg_values_supported"] = JsonValue(std::vector<std::string>{"RS256"});
    obj["scopes_supported"] = JsonValue(std::vector<std::string>{
        "openid", "profile", "profile:username", "profile:name", "profile:avatar",
        "profile:student_id", "email", "roles"});
    obj["token_endpoint_auth_methods_supported"] = JsonValue(std::vector<std::string>{
        "client_secret_post", "client_secret_basic"});
    obj["code_challenge_methods_supported"] = JsonValue(std::vector<std::string>{"S256"});
    obj["claims_supported"] = JsonValue(std::vector<std::string>{
        "sub", "preferred_username", "name", "picture", "student_id", "email",
        "email_verified", "roles", "internal", "sso_sub", "sse_sub"});
    http::json(res, 200, JsonValue(obj));
}

void oidcJwks(const Request&, Response& res) {
    res.statusCode = 200;
    res.body = RsJwt::jwks();
    res.headers["Content-Type"] = "application/json; charset=utf-8";
    res.headers["Cache-Control"] = "public, max-age=300";
}

namespace {

bool m2mAuthorized(const Request& req, Response& res, std::string& clientId) {
    std::string clientSecret = http::input(req, "client_secret");
    auto basic = basicCredentials(req);
    if (clientId.empty()) clientId = basic.first;
    if (clientSecret.empty()) clientSecret = basic.second;
    auto app = repo::findApplicationByClientId(clientId);
    std::string clientError;
    if (!app || !verifyClient(*app, clientSecret, clientError)) {
        oauthError(res, 401, "invalid_client");
        return false;
    }
    // Silent SSO is enabled per application from the admin console. The
    // environment allow-list is still honoured so existing deployments keep
    // working until every client has been switched over in the database.
    bool allowed = app->m2mEnabled;
    std::string allowRaw = util::env("UNISSO_PASSWORD_VERIFY_CLIENTS", "");
    if (!allowed && !allowRaw.empty()) {
        std::stringstream stream(allowRaw);
        std::string item;
        while (std::getline(stream, item, ',')) {
            if (util::trim(item) == clientId) { allowed = true; break; }
        }
    }
    if (!allowed) {
        oauthError(res, 403, "password_verify_forbidden");
        return false;
    }
    return true;
}

int ssoSessionHours() {
    std::string raw = util::env("UNISSO_SSO_SESSION_HOURS", "168");
    int hours = std::atoi(raw.c_str());
    return hours > 0 ? hours : 168;
}

std::string ssoCookieDomain() {
    return util::env("UNISSO_SSO_COOKIE_DOMAIN", ".ssemarket.cn");
}

std::string ssoCookieName() {
    return util::env("UNISSO_SSO_COOKIE_NAME", "sse_sso");
}

JsonValue userClaimsJson(const User& user) {
    std::map<std::string, JsonValue> u;
    u["sub"] = JsonValue(user.id);
    u["sso_sub"] = JsonValue(user.id);
    u["sse_sub"] = user.sseUserId.empty() ? JsonValue() : JsonValue(user.sseUserId);
    u["internal"] = JsonValue(user.isInternal);
    u["preferred_username"] = JsonValue(user.username);
    u["name"] = JsonValue(user.fullName);
    u["email"] = JsonValue(user.email);
    u["picture"] = JsonValue(user.avatar);
    u["student_id"] = JsonValue(user.studentId);
    return JsonValue(u);
}

void addSsoCookieMeta(std::map<std::string, JsonValue>& obj, const std::string& sid,
                      long long expiresAt) {
    obj["sso_session"] = JsonValue(sid);
    obj["sso_cookie_name"] = JsonValue(ssoCookieName());
    obj["sso_cookie_domain"] = JsonValue(ssoCookieDomain());
    obj["sso_max_age"] = JsonValue(ssoSessionHours() * 3600);
    obj["sso_expires_at"] = jsonx::num(expiresAt);
}

}  // namespace

void oauthPasswordVerify(const Request& req, Response& res) {
    std::string clientId = http::input(req, "client_id");
    if (!m2mAuthorized(req, res, clientId)) return;

    std::string email = util::lower(util::trim(http::input(req, "email")));
    std::string password = http::input(req, "password");
    if (email.empty() || password.empty()) {
        oauthError(res, 400, "invalid_request", "missing email or password");
        return;
    }

    std::string ip = http::clientIp(req);
    std::string key = "pwverify_attempts:" + ip + ":" + email;
    long long attempts = Cache::incr(key, 900);
    if (attempts > 5) {
        oauthError(res, 429, "too_many_attempts", "尝试次数过多，请稍后再试");
        return;
    }

    auto user = repo::findUserByEmail(email);
    if (!user || user->passwordHash.empty() ||
        !password::verify(password, user->passwordHash) || !user->isActive) {
        oauthError(res, 401, "invalid_credentials", "邮箱或密码错误");
        return;
    }
    Cache::del(key);

    int hours = ssoSessionHours();
    std::string sid = authx::createSession(user->id, user->username, req, "", hours);

    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["user"] = userClaimsJson(*user);
    addSsoCookieMeta(obj, sid, util::nowEpoch() + static_cast<long long>(hours) * 3600);
    http::ok(res, JsonValue(obj));
}

void oauthSessionVerify(const Request& req, Response& res) {
    std::string clientId = http::input(req, "client_id");
    if (!m2mAuthorized(req, res, clientId)) return;

    std::string sid = util::trim(http::input(req, "session"));
    if (sid.empty()) {
        oauthError(res, 400, "invalid_request", "missing session");
        return;
    }
    std::string raw = Cache::get("session:" + sid);
    if (raw.empty()) {
        oauthError(res, 401, "invalid_session");
        return;
    }
    std::map<std::string, JsonValue> data;
    if (!jsonx::parseObject(raw, data)) {
        oauthError(res, 401, "invalid_session");
        return;
    }
    std::string userId = jsonx::getString(data, "user_id");
    long long expiresAt = 0;
    auto expIt = data.find("expires_at");
    if (expIt != data.end() && expIt->second.getType() == JsonValue::NUMBER) {
        expiresAt = static_cast<long long>(expIt->second.asNumber());
    }
    if (userId.empty() || (expiresAt > 0 && expiresAt <= util::nowEpoch())) {
        Cache::del("session:" + sid);
        oauthError(res, 401, "invalid_session");
        return;
    }
    auto user = repo::findUserById(userId);
    if (!user || !user->isActive) {
        oauthError(res, 401, "invalid_session");
        return;
    }

    int hours = ssoSessionHours();
    long long now = util::nowEpoch();
    long long nextExpiry = now + static_cast<long long>(hours) * 3600;
    data["last_seen_at"] = jsonx::num(now);
    data["expires_at"] = jsonx::num(nextExpiry);
    Cache::setJson("session:" + sid, jsonx::dump(data), hours * 3600);

    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["user"] = userClaimsJson(*user);
    addSsoCookieMeta(obj, sid, nextExpiry);
    http::ok(res, JsonValue(obj));
}

void oauthSessionRevoke(const Request& req, Response& res) {
    std::string clientId = http::input(req, "client_id");
    if (!m2mAuthorized(req, res, clientId)) return;

    std::string sid = util::trim(http::input(req, "session"));
    if (sid.empty()) {
        oauthError(res, 400, "invalid_request", "missing session");
        return;
    }
    Cache::del("session:" + sid);
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    http::ok(res, JsonValue(obj));
}

}  // namespace handlers

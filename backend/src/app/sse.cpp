#include "core.h"
#include "app/logger.h"

#include <cstdlib>

#include <curl/curl.h>
#include <openssl/evp.h>

#include <iostream>
#include <mutex>

namespace {

struct HttpResult {
    long status = 0;
    std::string body;
};

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

void ensureCurl() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpResult httpRequest(const std::string& url, const std::string& method,
                       const std::string& body, const std::string& contentType,
                       const std::vector<std::string>& headers) {
    ensureCurl();
    HttpResult result;
    CURL* curl = curl_easy_init();
    if (!curl) return result;

    struct curl_slist* list = nullptr;
    if (!contentType.empty()) {
        list = curl_slist_append(list, ("Content-Type: " + contentType).c_str());
    }
    for (const auto& header : headers) list = curl_slist_append(list, header.c_str());

    char errorBuffer[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    if (code != CURLE_OK && result.body.empty()) {
        result.body = errorBuffer[0] ? errorBuffer : curl_easy_strerror(code);
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return result;
}

std::string aesEncrypt(const std::string& plain) {
    auto& cfg = AppConfig::get();
    std::string key = cfg.sseAesKey;
    if (key.size() != 16) key = "16bit secret key";
    std::string ivHex = util::hex(util::sha256Raw(key));
    std::string iv = ivHex.substr(0, 16);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";
    std::string out(plain.size() + 32, '\0');
    int len = 0;
    int finalLen = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(iv.data())) != 1 ||
        EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &len,
                          reinterpret_cast<const unsigned char*>(plain.data()),
                          static_cast<int>(plain.size())) != 1 ||
        EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + len,
                            &finalLen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return "";
    }
    out.resize(static_cast<size_t>(len + finalLen));
    EVP_CIPHER_CTX_free(ctx);
    return util::base64Encode(out);
}

int responseCode(const std::map<std::string, JsonValue>& obj) {
    auto it = obj.find("code");
    if (it == obj.end()) return 0;
    if (it->second.getType() == JsonValue::NUMBER) {
        return static_cast<int>(it->second.asNumber());
    }
    if (it->second.getType() == JsonValue::STRING) {
        try { return std::stoi(it->second.asString()); } catch (...) { return 0; }
    }
    return 0;
}

std::string userIdString(const JsonValue& value) {
    if (value.getType() == JsonValue::NUMBER) {
        return std::to_string(static_cast<long long>(value.asNumber()));
    }
    if (value.getType() == JsonValue::STRING) return value.asString();
    return "";
}

}  // namespace

namespace sse {

bool login(const std::string& email, const std::string& password,
           Profile& profile, std::string& error) {
    auto& cfg = AppConfig::get();
    std::string cipher = aesEncrypt(password);
    if (cipher.empty()) {
        error = "sse_encrypt_failed";
        return false;
    }

    std::map<std::string, JsonValue> body;
    body["email"] = JsonValue(util::lower(util::trim(email)));
    body["password"] = JsonValue(cipher);
    HttpResult loginResult = httpRequest(cfg.sseApiBase + "/auth/login", "POST",
                                         JsonValue(body).toJson(), "application/json", {});
    std::map<std::string, JsonValue> loginObj;
    if (!jsonx::parseObject(loginResult.body, loginObj)) {
        error = "sse_invalid_response";
        return false;
    }
    if (responseCode(loginObj) != 200) {
        error = jsonx::getString(loginObj, "msg", "sse_login_failed");
        return false;
    }
    auto data = loginObj.count("data") ? loginObj["data"].asObject()
                                        : std::map<std::string, JsonValue>{};
    std::string token = jsonx::getString(data, "token");
    if (token.empty()) {
        error = "sse_login_failed";
        return false;
    }

    HttpResult infoResult = httpRequest(cfg.sseApiBase + "/auth/info", "GET", "", "",
                                        {"Authorization: Bearer " + token});
    std::map<std::string, JsonValue> infoObj;
    if (!jsonx::parseObject(infoResult.body, infoObj)) {
        error = "sse_invalid_response";
        return false;
    }
    if (responseCode(infoObj) != 200) {
        error = jsonx::getString(infoObj, "msg", "sse_userinfo_failed");
        return false;
    }
    auto infoData = infoObj.count("data") ? infoObj["data"].asObject()
                                          : std::map<std::string, JsonValue>{};
    auto user = infoData.count("user") ? infoData["user"].asObject()
                                       : std::map<std::string, JsonValue>{};
    profile.userId = user.count("userID") ? userIdString(user["userID"]) : "";
    profile.email = jsonx::getString(user, "email", email);
    profile.name = jsonx::getString(user, "name");
    profile.identity = jsonx::getString(user, "identity");
    profile.avatar = jsonx::getString(user, "avatarURL",
                                     jsonx::getString(user, "avatar_url"));
    if (!profile.avatar.empty() && profile.avatar.front() == '/') {
        profile.avatar = "https://ssemarket.cn" + profile.avatar;
    }
    if (profile.userId.empty()) {
        error = "sse_userid_missing";
        return false;
    }
    return true;
}

bool exchangeOauthCode(const std::string& code, Profile& profile, std::string& error) {
    auto& cfg = AppConfig::get();
    std::string form = "code=" + util::urlEncode(code) +
                       "&app_id=" + util::urlEncode(cfg.sseOauthAppId) +
                       "&app_secret=" + util::urlEncode(cfg.sseOauthAppSecret);
    HttpResult tokenResult = httpRequest(cfg.sseOauthTokenUrl, "POST", form,
                                         "application/x-www-form-urlencoded", {});
    std::map<std::string, JsonValue> tokenObj;
    if (!jsonx::parseObject(tokenResult.body, tokenObj)) {
        error = "sse_invalid_response";
        return false;
    }
    if (responseCode(tokenObj) != 200) {
        error = jsonx::getString(tokenObj, "msg", "sse_token_failed");
        return false;
    }
    auto tokenData = tokenObj.count("data") ? tokenObj["data"].asObject()
                                              : std::map<std::string, JsonValue>{};
    std::string accessToken = jsonx::getString(tokenData, "access_token");
    if (accessToken.empty()) {
        error = "sse_token_missing";
        return false;
    }

    HttpResult infoResult = httpRequest(cfg.sseOauthUserinfoUrl, "POST", "", "",
                                        {"Authorization: Bearer " + accessToken});
    std::map<std::string, JsonValue> infoObj;
    if (!jsonx::parseObject(infoResult.body, infoObj)) {
        error = "sse_invalid_response";
        return false;
    }
    if (responseCode(infoObj) != 200) {
        error = jsonx::getString(infoObj, "msg", "sse_userinfo_failed");
        return false;
    }
    auto infoData = infoObj.count("data") ? infoObj["data"].asObject()
                                          : std::map<std::string, JsonValue>{};
    profile.userId = infoData.count("user_id") ? userIdString(infoData["user_id"]) : "";
    profile.email = jsonx::getString(infoData, "email");
    profile.name = jsonx::getString(infoData, "name");
    profile.identity = jsonx::getString(infoData, "identity");
    profile.avatar = jsonx::getString(infoData, "avatarURL",
                                     jsonx::getString(infoData, "avatar_url"));
    if (!profile.avatar.empty() && profile.avatar.front() == '/') {
        profile.avatar = "https://ssemarket.cn" + profile.avatar;
    }
    if (profile.userId.empty()) {
        error = "sse_userid_missing";
        return false;
    }
    return true;
}


bool validateEmail(const std::string& email, std::string& error) {
    auto& cfg = AppConfig::get();
    std::map<std::string, JsonValue> body;
    body["email"] = JsonValue(util::lower(util::trim(email)));
    body["mode"] = JsonValue(0);
    HttpResult result = httpRequest(cfg.sseApiBase + "/auth/validateEmail", "POST",
                                    JsonValue(body).toJson(), "application/json", {});
    std::map<std::string, JsonValue> obj;
    if (!jsonx::parseObject(result.body, obj)) {
        error = "sse_invalid_response";
        return false;
    }
    if (responseCode(obj) != 200) {
        error = jsonx::getString(obj, "msg", "sse_email_failed");
        return false;
    }
    return true;
}

bool registerAccount(const std::string& email, const std::string& password,
                     const std::string& name, const std::string& valiCode,
                     const std::string& cdkey, Profile& profile, std::string& error) {
    auto& cfg = AppConfig::get();
    std::string cipher = aesEncrypt(password);
    if (cipher.empty()) { error = "sse_encrypt_failed"; return false; }
    std::string displayName = util::trim(name);
    if (displayName.empty()) {
        size_t at = email.find('@');
        displayName = at == std::string::npos ? email : email.substr(0, at);
    }
    std::map<std::string, JsonValue> body;
    body["name"] = JsonValue(displayName);
    body["email"] = JsonValue(util::lower(util::trim(email)));
    body["password"] = JsonValue(cipher);
    body["password2"] = JsonValue(cipher);
    body["phone"] = JsonValue("");
    body["valiCode"] = JsonValue(valiCode);
    body["ValiCode"] = JsonValue(valiCode);
    body["CDKey"] = JsonValue(cdkey);
    HttpResult result = httpRequest(cfg.sseApiBase + "/auth/register", "POST",
                                    JsonValue(body).toJson(), "application/json", {});
    std::map<std::string, JsonValue> obj;
    if (!jsonx::parseObject(result.body, obj)) {
        error = "sse_invalid_response";
        return false;
    }
    if (responseCode(obj) != 200) {
        error = jsonx::getString(obj, "msg", "sse_register_failed");
        return false;
    }
    auto data = obj.count("data") ? obj["data"].asObject() : std::map<std::string, JsonValue>{};
    std::string token = jsonx::getString(data, "token");
    if (token.empty()) { error = "sse_register_failed"; return false; }

    HttpResult infoResult = httpRequest(cfg.sseApiBase + "/auth/info", "GET", "", "",
                                        {"Authorization: Bearer " + token});
    std::map<std::string, JsonValue> infoObj;
    if (!jsonx::parseObject(infoResult.body, infoObj) || responseCode(infoObj) != 200) {
        error = jsonx::getString(infoObj, "msg", "sse_userinfo_failed");
        return false;
    }
    auto infoData = infoObj.count("data") ? infoObj["data"].asObject()
                                          : std::map<std::string, JsonValue>{};
    auto user = infoData.count("user") ? infoData["user"].asObject()
                                       : std::map<std::string, JsonValue>{};
    profile.userId = user.count("userID") ? userIdString(user["userID"]) : "";
    profile.email = jsonx::getString(user, "email", email);
    profile.name = jsonx::getString(user, "name", displayName);
    profile.identity = jsonx::getString(user, "identity");
    profile.avatar = jsonx::getString(user, "avatarURL",
                                     jsonx::getString(user, "avatar_url"));
    if (!profile.avatar.empty() && profile.avatar.front() == '/') {
        profile.avatar = "https://ssemarket.cn" + profile.avatar;
    }
    if (profile.userId.empty()) { error = "sse_userid_missing"; return false; }
    return true;
}

std::string authorizeUrl(const std::string& state) {
    auto& cfg = AppConfig::get();
    return cfg.sseOauthAuthorizeUrl + "?appid=" + util::urlEncode(cfg.sseOauthAppId) +
           "&redirect_uri=" + util::urlEncode(cfg.sseOauthRedirectUri) +
           "&response_type=code&scope=read_basic&state=" + util::urlEncode(state);
}

std::optional<User> upsertUser(const Profile& profile, const std::string& password,
                               std::string* rejectReason) {
    auto& cfg = AppConfig::get();
    auto existing = repo::findUserBySseId(profile.userId);
    if (!existing) {
        // The SSE id may be bound as a secondary identity (an account can
        // bind several marketplace accounts): ownership still identifies it.
        auto rows = Db::query(
            "SELECT user_id FROM user_identities WHERE provider='sse' AND "
            "provider_user_id=" + Db::quote(profile.userId) + " AND is_active=1 LIMIT 1");
        if (!rows.empty()) existing = repo::findUserById(rows.front().at("user_id"));
    }
    if (!existing && !profile.email.empty()) {
        auto byEmail = repo::findUserByEmail(profile.email);
        if (byEmail) {
            // Email match: bind this SSE identity to the account. An external
            // (email-registered) account is promoted; an account already
            // bound to a *different* SSE id simply gains another bound
            // marketplace identity. UniSSO registration, email-code login and
            // password reset all prove mailbox control, and this email was
            // just verified by SSE server-side, so binding is safe. The
            // identity used for this login becomes the active one (primary);
            // the consent page lets the user pick among bound identities.
            std::string bindError;
            if (!bindIdentityToUser(byEmail->id, profile, bindError)) {
                std::cerr << "[sse] email match but bind refused (" << bindError
                          << ") sse_user_id=" << profile.userId << std::endl;
                if (rejectReason) *rejectReason = bindError;
                return std::nullopt;
            }
            applog::access("[sse] bound sse_user_id=" + profile.userId + " to account " +
                           byEmail->id + (byEmail->isInternal ? "" : " (promoted external)"));
            existing = repo::findUserById(byEmail->id);
            if (!existing) return std::nullopt;
        }
    }

    std::string superIds = "561";
    if (const char* env = std::getenv("UNISSO_SSE_SUPER_ADMIN_IDS"); env && *env) superIds = env;
    bool superAdmin = ("," + superIds + ",").find("," + profile.userId + ",") != std::string::npos;
    if (!existing) {
        User user;
        user.id = util::uuid4();
        user.email = util::lower(util::trim(profile.email));
        if (user.email.empty()) user.email = profile.userId + "@sse.local";
        user.passwordHash = password::hash(password.empty() ? util::randomToken(32) : password);
        std::string baseName = util::trim(profile.name);
        if (baseName.empty()) baseName = "sse_" + profile.userId;
        if (baseName.size() > 64) baseName.resize(64);
        auto clash = Db::query("SELECT id FROM users WHERE username=" + Db::quote(baseName) + " LIMIT 1");
        if (!clash.empty() || baseName.empty()) baseName = "sse_" + profile.userId;
        user.username = baseName;
        user.fullName = profile.name;
        user.avatar = profile.avatar;
        user.emailVerified = true;
        user.isActive = true;
        user.isInternal = true;
        user.isSuperAdmin = superAdmin;
        user.isAdmin = superAdmin;
        user.sseUserId = profile.userId;
        if (!repo::createUser(user)) return std::nullopt;
        existing = repo::findUserById(user.id);
    } else {
        std::string updates =
            "is_internal=1, sse_user_id=" + Db::quote(profile.userId) +
            ", email_verified=1, updated_at=NOW()";
        if (superAdmin) updates += ", is_super_admin=1, is_admin=1";
        if (!profile.avatar.empty()) {
            updates += ", avatar=COALESCE(NULLIF(avatar,''), " +
                       Db::quote(profile.avatar) + ")";
        }
        // An account that was registered locally with the same email before
        // the marketplace identity was bound keeps a placeholder display name
        // (empty full_name, "user_xxxxxxxx" username). Apps that asked for
        // profile:name then see a fresh anonymous account instead of the
        // marketplace identity they just logged in with. Adopt the SSE
        // display name once, when the account has none of its own.
        if (existing && existing->fullName.empty() && !profile.name.empty()) {
            std::string nm;
            size_t chars = 0;
            for (size_t i = 0; i < profile.name.size() && chars < 120; chars++) {
                unsigned char lead = (unsigned char)profile.name[i];
                size_t step = lead >= 240 ? 4 : lead >= 224 ? 3 : lead >= 192 ? 2 : 1;
                if (i + step > profile.name.size()) break;
                nm.append(profile.name, i, step);
                i += step;
            }
            updates += ", full_name=" + Db::quote(util::trim(nm));
        }
        Db::exec("UPDATE users SET " + updates + " WHERE id=" + Db::quote(existing->id));
        existing = repo::findUserById(existing->id);
    }
    if (!existing) return std::nullopt;

    Db::exec(
        "INSERT INTO sse_identities (id,sse_user_id,email,name,identity,is_internal,"
        "last_verified_at,created_at,updated_at) VALUES (" +
        Db::quote(util::uuid4()) + "," + Db::quote(profile.userId) + "," +
        Db::nullable(profile.email) + "," + Db::nullable(profile.name) + "," +
        Db::nullable(profile.identity) + ",1,NOW(),NOW(),NOW()) "
        "ON DUPLICATE KEY UPDATE email=VALUES(email), name=VALUES(name), "
        "identity=VALUES(identity), is_internal=1, last_verified_at=NOW(), updated_at=NOW()");

    if (!profile.email.empty()) {
        std::map<std::string, JsonValue> cache;
        cache["sse_user_id"] = JsonValue(profile.userId);
        cache["email"] = JsonValue(profile.email);
        cache["name"] = JsonValue(profile.name);
        cache["identity"] = JsonValue(profile.identity);
        Cache::setJson("sse_login:" + util::lower(profile.email),
                       jsonx::dump(cache), cfg.sseCacheSeconds);
    }

    return repo::findUserById(existing->id);
}


bool bindIdentityToUser(const std::string& userId, const Profile& profile,
                        std::string& error) {
    if (profile.userId.empty()) { error = "sse_userid_missing"; return false; }
    // One account may bind several marketplace identities. If a different SSE
    // id is currently primary, keep it as a bound identity row before the new
    // one takes over (the consent page lets the user pick which to present).
    auto primary = Db::query("SELECT sse_user_id FROM users WHERE id=" + Db::quote(userId) + " LIMIT 1");
    if (!primary.empty()) {
        std::string current = primary.front().at("sse_user_id");
        if (!current.empty() && current != profile.userId) {
            auto prev = Db::query(
                "SELECT id FROM user_identities WHERE user_id=" + Db::quote(userId) +
                " AND provider='sse' AND provider_user_id=" + Db::quote(current) + " LIMIT 1");
            if (prev.empty()) {
                Db::exec(
                    "INSERT INTO user_identities (id,user_id,provider,provider_user_id,"
                    "is_active,created_at,updated_at) VALUES (" + Db::quote(util::uuid4()) + "," +
                    Db::quote(userId) + ",'sse'," + Db::quote(current) + ",1,NOW(),NOW())");
            }
        }
    }

    auto bySse = Db::query("SELECT id FROM users WHERE sse_user_id=" +
                           Db::quote(profile.userId) + " AND id<>" +
                           Db::quote(userId) + " LIMIT 1");
    if (!bySse.empty()) { error = "sse_already_bound"; return false; }
    auto existing = Db::query(
        "SELECT user_id FROM user_identities WHERE provider='sse' AND "
        "provider_user_id=" + Db::quote(profile.userId) +
        " AND is_active=1 LIMIT 1");
    if (!existing.empty() && existing.front().at("user_id") != userId) {
        error = "sse_already_bound";
        return false;
    }

    auto own = Db::query(
        "SELECT id FROM user_identities WHERE user_id=" + Db::quote(userId) +
        " AND provider='sse' AND provider_user_id=" +
        Db::quote(profile.userId) + " LIMIT 1");
    if (own.empty()) {
        Db::exec(
            "INSERT INTO user_identities (id,user_id,provider,provider_user_id,"
            "provider_username,provider_email,provider_avatar,is_active,created_at,updated_at) "
            "VALUES (" + Db::quote(util::uuid4()) + "," + Db::quote(userId) +
            ",'sse'," + Db::quote(profile.userId) + "," +
            Db::nullable(profile.name) + "," + Db::nullable(profile.email) + "," +
            Db::nullable(profile.avatar) + ",1,NOW(),NOW())");
    } else {
        Db::exec(
            "UPDATE user_identities SET provider_username=" + Db::nullable(profile.name) +
            ", provider_email=" + Db::nullable(profile.email) +
            ", provider_avatar=" + Db::nullable(profile.avatar) +
            ", is_active=1, updated_at=NOW() WHERE id=" +
            Db::quote(own.front().at("id")));
    }

    Db::exec(
        "UPDATE users SET is_internal=1, "
        "sse_user_id=" + Db::quote(profile.userId) + ", "
        "avatar=COALESCE(NULLIF(avatar,'')," + Db::nullable(profile.avatar) + "), "
        "updated_at=NOW() WHERE id=" + Db::quote(userId));
    return true;
}

// All SSE ids usable by this account: the active primary plus every active
// identity row (deduplicated, primary first).
std::vector<SseIdentityOption> listSseIdentities(const User& user) {
    std::vector<SseIdentityOption> out;
    auto push = [&](const std::string& id, const std::string& name,
                    const std::string& email, bool current) {
        if (id.empty()) return;
        for (const auto& item : out) {
            if (item.userId == id) return;
        }
        out.push_back({id, name, email, current || id == user.sseUserId});
    };
    push(user.sseUserId, user.fullName, user.email, true);
    auto rows = Db::query(
        "SELECT provider_user_id, provider_username, provider_email FROM user_identities "
        "WHERE user_id=" + Db::quote(user.id) + " AND provider='sse' AND is_active=1 "
        "ORDER BY updated_at DESC");
    for (const auto& row : rows) {
        auto get = [&](const char* key) -> std::string {
            auto it = row.find(key);
            return it == row.end() ? "" : it->second;
        };
        push(get("provider_user_id"), get("provider_username"), get("provider_email"), false);
    }
    return out;
}

bool selectSseIdentity(const User& user, const std::string& sseUserId, std::string& error) {
    for (const auto& option : listSseIdentities(user)) {
        if (option.userId == sseUserId) {
            if (sseUserId == user.sseUserId) return true;
            if (Db::exec("UPDATE users SET sse_user_id=" + Db::quote(sseUserId) +
                         ", updated_at=NOW() WHERE id=" + Db::quote(user.id)) < 0) {
                error = "switch_failed";
                return false;
            }
            return true;
        }
    }
    error = "identity_not_bound";
    return false;
}

void selfTest() {
    std::string cipher = aesEncrypt("secret123");
    std::cout << "[sse] AES self-test: "
              << (cipher == "ZQwiKcmlFoNOeBdvHoUOGQ==" ? "PASS" : "FAIL")
              << std::endl;
}

}  // namespace sse

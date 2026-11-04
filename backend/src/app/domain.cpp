#include "core.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>

namespace {

std::vector<std::string> parseJsonArray(const std::string& raw) {
    std::vector<std::string> out;
    if (raw.empty()) return out;
    JsonValue value;
    try { value.fromJson(raw); } catch (...) { return out; }
    if (value.getType() != JsonValue::ARRAY) return out;
    for (const auto& item : value.asArray()) {
        if (item.getType() == JsonValue::STRING) out.push_back(item.asString());
    }
    return out;
}

std::string jsonArraySql(const std::vector<std::string>& values) {
    return Db::quote(JsonValue(values).toJson());
}

bool isTrue(const std::string& value) { return value == "1" || util::lower(value) == "true"; }

User mapUser(const std::map<std::string, std::string>& row) {
    User user;
    auto get = [&row](const std::string& key) {
        auto it = row.find(key);
        return it == row.end() ? std::string() : it->second;
    };
    user.id = get("id");
    user.email = get("email");
    user.emailVerified = isTrue(get("email_verified"));
    user.passwordHash = get("password_hash");
    user.username = get("username");
    user.fullName = get("full_name");
    user.studentId = get("student_id");
    user.avatar = get("avatar");
    user.isActive = isTrue(get("is_active"));
    user.isAdmin = isTrue(get("is_admin"));
    user.isInternal = isTrue(get("is_internal"));
    user.isSuperAdmin = isTrue(get("is_super_admin"));
    user.sseUserId = get("sse_user_id");
    user.createdAt = get("created_at");
    user.updatedAt = get("updated_at");
    user.lastLoginAt = get("last_login_at");
    return user;
}

Application mapApplication(const std::map<std::string, std::string>& row) {
    Application app;
    auto get = [&row](const std::string& key) {
        auto it = row.find(key);
        return it == row.end() ? std::string() : it->second;
    };
    app.id = get("id");
    app.clientId = get("client_id");
    app.clientSecret = get("client_secret");
    app.name = get("name");
    app.description = get("description");
    app.icon = get("icon");
    app.redirectUris = parseJsonArray(get("redirect_uris"));
    app.allowedScopes = parseJsonArray(get("allowed_scopes"));
    app.homepageUrl = get("homepage_url");
    app.callbackUrl = get("callback_url");
    app.isActive = isTrue(get("is_active"));
    app.isConfidential = isTrue(get("is_confidential"));
    app.m2mEnabled = isTrue(get("m2m_enabled"));
    app.kind = get("kind").empty() ? "internal" : get("kind");
    app.status = get("status").empty() ? "approved" : get("status");
    app.reviewedBy = get("reviewed_by");
    app.reviewedAt = get("reviewed_at");
    app.reviewNote = get("review_note");
    app.ownerId = get("owner_id");
    app.createdAt = get("created_at");
    app.updatedAt = get("updated_at");
    return app;
}

Identity mapIdentity(const std::map<std::string, std::string>& row) {
    Identity identity;
    auto get = [&row](const std::string& key) {
        auto it = row.find(key);
        return it == row.end() ? std::string() : it->second;
    };
    identity.id = get("id");
    identity.userId = get("user_id");
    identity.provider = get("provider");
    identity.providerUserId = get("provider_user_id");
    identity.providerUsername = get("provider_username");
    identity.providerEmail = get("provider_email");
    identity.providerAvatar = get("provider_avatar");
    identity.isActive = isTrue(get("is_active"));
    identity.createdAt = get("created_at");
    return identity;
}

}  // namespace

// ===========================================================================
// Repository
// ===========================================================================

namespace repo {

std::optional<User> findUserByEmail(const std::string& email) {
    auto rows = Db::query("SELECT * FROM users WHERE email=" + Db::quote(util::lower(util::trim(email))) +
                          " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    User user = mapUser(rows.front());
    loadUserRelations(user);
    return user;
}

std::optional<User> findUserByUsername(const std::string& username) {
    std::string value = util::trim(username);
    if (value.empty()) return std::nullopt;
    // username carries a unique index with the table's case-insensitive
    // collation, so casing does not matter here.
    auto rows = Db::query("SELECT * FROM users WHERE username=" + Db::quote(value) + " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    User user = mapUser(rows.front());
    loadUserRelations(user);
    return user;
}

std::optional<User> findUserById(const std::string& id) {
    auto rows = Db::query("SELECT * FROM users WHERE id=" + Db::quote(id) + " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    User user = mapUser(rows.front());
    loadUserRelations(user);
    return user;
}

std::optional<User> findUserBySseId(const std::string& sseUserId) {
    if (sseUserId.empty()) return std::nullopt;
    auto rows = Db::query("SELECT * FROM users WHERE sse_user_id=" + Db::quote(sseUserId) + " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    User user = mapUser(rows.front());
    loadUserRelations(user);
    return user;
}


std::vector<User> listUsers() {
    std::vector<User> users;
    auto rows = Db::query("SELECT * FROM users ORDER BY created_at DESC");
    for (const auto& row : rows) {
        User user = mapUser(row);
        loadUserRelations(user);
        users.push_back(std::move(user));
    }
    return users;
}

bool loadUserRelations(User& user) {
    user.identities = listIdentities(user.id);
    return true;
}

bool createUser(const User& user) {
    std::string username = user.username.empty()
        ? "user_" + util::randomHex(4) : user.username;
    std::string sql =
        "INSERT INTO users (id,email,email_verified,password_hash,username,full_name,student_id,avatar,is_active,is_admin,is_internal,sse_user_id,is_super_admin,created_at,updated_at) VALUES (" +
        Db::quote(user.id) + "," +
        Db::quote(util::lower(util::trim(user.email))) + "," +
        (user.emailVerified ? "1" : "0") + "," +
        Db::quote(user.passwordHash) + "," +
        Db::quote(username) + "," +
        Db::nullable(user.fullName) + "," +
        Db::nullable(user.studentId) + "," +
        Db::nullable(user.avatar) + "," +
        (user.isActive ? "1" : "0") + "," +
        (user.isAdmin ? "1" : "0") + "," +
        (user.isInternal ? "1" : "0") + "," +
        Db::nullable(user.sseUserId) + "," +
        (user.isSuperAdmin ? "1" : "0") + ",NOW(),NOW())";
    return Db::exec(sql) > 0;
}

bool updateUserPassword(const std::string& userId, const std::string& passwordHash) {
    return Db::exec("UPDATE users SET password_hash=" + Db::quote(passwordHash) +
                    ", updated_at=NOW() WHERE id=" + Db::quote(userId)) >= 0;
}

bool updateUserLastLogin(const std::string& userId) {
    return Db::exec("UPDATE users SET last_login_at=NOW() WHERE id=" + Db::quote(userId)) >= 0;
}

std::vector<Application> listApplications() {
    std::vector<Application> out;
    auto rows = Db::query("SELECT * FROM applications ORDER BY created_at DESC");
    for (const auto& row : rows) out.push_back(mapApplication(row));
    return out;
}

std::vector<Application> listApplicationsByStatus(const std::string& status) {
    std::vector<Application> out;
    auto rows = Db::query("SELECT * FROM applications WHERE status=" + Db::quote(status) +
                          " ORDER BY created_at ASC LIMIT 100");
    for (const auto& row : rows) out.push_back(mapApplication(row));
    return out;
}

std::optional<Application> findApplicationByClientId(const std::string& clientId) {
    auto rows = Db::query("SELECT * FROM applications WHERE client_id=" + Db::quote(clientId) + " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    return mapApplication(rows.front());
}

std::optional<Application> findApplicationById(const std::string& id) {
    auto rows = Db::query("SELECT * FROM applications WHERE id=" + Db::quote(id) + " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    return mapApplication(rows.front());
}

std::vector<Application> listApplicationsForUser(const std::string& userId) {
    std::vector<Application> out;
    auto rows = Db::query("SELECT * FROM applications WHERE owner_id=" + Db::quote(userId) +
                          " ORDER BY created_at DESC");
    for (const auto& row : rows) out.push_back(mapApplication(row));
    return out;
}

std::optional<Consent> findConsent(const std::string& userId, const std::string& applicationId) {
    auto rows = Db::query(
        "SELECT * FROM user_consents WHERE user_id=" + Db::quote(userId) +
        " AND application_id=" + Db::quote(applicationId) +
        " AND is_active=1 LIMIT 1");
    if (rows.empty()) return std::nullopt;
    const auto& row = rows.front();
    Consent consent;
    auto get = [&row](const std::string& key) {
        auto it = row.find(key);
        return it == row.end() ? std::string() : it->second;
    };
    consent.id = get("id");
    consent.userId = get("user_id");
    consent.applicationId = get("application_id");
    consent.scopes = parseJsonArray(get("scopes"));
    consent.grantedAt = get("granted_at");
    consent.isActive = isTrue(get("is_active"));
    auto app = findApplicationById(consent.applicationId);
    if (app) consent.application = *app;
    return consent;
}

std::vector<Consent> listConsents(const std::string& userId, bool activeOnly) {
    std::vector<Consent> out;
    std::string sql = "SELECT * FROM user_consents WHERE user_id=" + Db::quote(userId);
    if (activeOnly) sql += " AND is_active=1";
    sql += " ORDER BY granted_at DESC";
    auto rows = Db::query(sql);
    for (const auto& row : rows) {
        Consent consent;
        auto get = [&row](const std::string& key) {
            auto it = row.find(key);
            return it == row.end() ? std::string() : it->second;
        };
        consent.id = get("id");
        consent.userId = get("user_id");
        consent.applicationId = get("application_id");
        consent.scopes = parseJsonArray(get("scopes"));
        consent.grantedAt = get("granted_at");
        consent.isActive = isTrue(get("is_active"));
        auto app = findApplicationById(consent.applicationId);
        if (app) consent.application = *app;
        out.push_back(std::move(consent));
    }
    return out;
}

bool upsertConsent(const std::string& userId, const std::string& applicationId,
                   const std::vector<std::string>& scopes) {
    auto existing = findConsent(userId, applicationId);
    if (existing) {
        std::vector<std::string> merged = existing->scopes;
        for (const auto& scope : scopes) {
            if (std::find(merged.begin(), merged.end(), scope) == merged.end()) merged.push_back(scope);
        }
        return Db::exec("UPDATE user_consents SET scopes=" + jsonArraySql(merged) +
                        ", granted_at=NOW(), revoked_at=NULL, is_active=1 WHERE id=" +
                        Db::quote(existing->id)) >= 0;
    }
    return Db::exec(
               "INSERT INTO user_consents (id,user_id,application_id,scopes,granted_at,is_active) VALUES (" +
               Db::quote(util::uuid4()) + "," + Db::quote(userId) + "," +
               Db::quote(applicationId) + "," + jsonArraySql(scopes) + ",NOW(),1)") > 0;
}

bool revokeConsent(const std::string& userId, const std::string& applicationId) {
    auto existing = findConsent(userId, applicationId);
    if (!existing) return false;
    Db::exec("UPDATE user_consents SET is_active=0, revoked_at=NOW() WHERE id=" +
             Db::quote(existing->id));
    return true;
}

std::vector<Identity> listIdentities(const std::string& userId) {
    auto rows = Db::query("SELECT * FROM user_identities WHERE user_id=" + Db::quote(userId) +
                          " AND is_active=1 ORDER BY created_at DESC");
    std::vector<Identity> out;
    for (const auto& row : rows) out.push_back(mapIdentity(row));
    return out;
}

bool bindIdentity(const Identity& identity) {
    std::string sql =
        "INSERT INTO user_identities (id,user_id,provider,provider_user_id,provider_username,"
        "provider_email,provider_avatar,is_active,created_at,updated_at) VALUES (" +
        Db::quote(identity.id.empty() ? util::uuid4() : identity.id) + "," +
        Db::quote(identity.userId) + "," + Db::quote(identity.provider) + "," +
        Db::quote(identity.providerUserId) + "," + Db::nullable(identity.providerUsername) + "," +
        Db::nullable(identity.providerEmail) + "," + Db::nullable(identity.providerAvatar) +
        ",1,NOW(),NOW())";
    return Db::exec(sql) > 0;
}

bool unbindIdentity(const std::string& userId, const std::string& identityId) {
    return Db::exec("UPDATE user_identities SET is_active=0, updated_at=NOW() WHERE id=" +
                    Db::quote(identityId) + " AND user_id=" + Db::quote(userId)) >= 0;
}

JsonValue userJson(const User& user, bool withRelations) {
    std::map<std::string, JsonValue> obj;
    obj["id"] = JsonValue(user.id);
    obj["email"] = JsonValue(user.email);
    obj["email_verified"] = JsonValue(user.emailVerified);
    obj["username"] = JsonValue(user.username);
    obj["full_name"] = JsonValue(user.fullName);
    obj["student_id"] = JsonValue(user.studentId);
    obj["avatar"] = JsonValue(user.avatar);
    obj["is_active"] = JsonValue(user.isActive);
    obj["is_admin"] = JsonValue(user.isAdmin);
    obj["is_internal"] = JsonValue(user.isInternal);
    obj["is_super_admin"] = JsonValue(user.isSuperAdmin);
    obj["created_at"] = JsonValue(user.createdAt);
    obj["last_login_at"] = JsonValue(user.lastLoginAt);
    if (withRelations) {
        std::vector<JsonValue> identities;
        for (const auto& identity : user.identities) identities.push_back(identityJson(identity));
        obj["identities"] = JsonValue(identities);
    }
    return JsonValue(obj);
}

JsonValue applicationJson(const Application& app, bool withSecret) {
    std::map<std::string, JsonValue> obj;
    obj["id"] = JsonValue(app.id);
    obj["client_id"] = JsonValue(app.clientId);
    if (withSecret) obj["client_secret"] = JsonValue(app.clientSecret);
    obj["name"] = JsonValue(app.name);
    obj["description"] = JsonValue(app.description);
    obj["icon"] = JsonValue(app.icon);
    obj["redirect_uris"] = JsonValue(app.redirectUris);
    obj["allowed_scopes"] = JsonValue(app.allowedScopes);
    obj["homepage_url"] = JsonValue(app.homepageUrl);
    obj["callback_url"] = JsonValue(app.callbackUrl);
    obj["is_active"] = JsonValue(app.isActive);
    obj["is_confidential"] = JsonValue(app.isConfidential);
    obj["m2m_enabled"] = JsonValue(app.m2mEnabled);
    obj["kind"] = JsonValue(app.kind);
    obj["status"] = JsonValue(app.status);
    obj["review_note"] = JsonValue(app.reviewNote);
    obj["owner_id"] = JsonValue(app.ownerId);
    obj["created_at"] = JsonValue(app.createdAt);
    return JsonValue(obj);
}

JsonValue consentJson(const Consent& consent) {
    std::map<std::string, JsonValue> obj;
    obj["id"] = JsonValue(consent.id);
    obj["application"] = applicationJson(consent.application, false);
    obj["scopes"] = JsonValue(consent.scopes);
    obj["granted_at"] = JsonValue(consent.grantedAt);
    obj["is_active"] = JsonValue(consent.isActive);
    return JsonValue(obj);
}

JsonValue identityJson(const Identity& identity) {
    std::map<std::string, JsonValue> obj;
    obj["id"] = JsonValue(identity.id);
    obj["provider"] = JsonValue(identity.provider);
    obj["provider_user_id"] = JsonValue(identity.providerUserId);
    obj["provider_username"] = JsonValue(identity.providerUsername);
    obj["provider_email"] = JsonValue(identity.providerEmail);
    obj["provider_avatar"] = JsonValue(identity.providerAvatar);
    obj["is_active"] = JsonValue(identity.isActive);
    obj["created_at"] = JsonValue(identity.createdAt);
    return JsonValue(obj);
}

}  // namespace repo

// ===========================================================================
// RS256 JWT
// ===========================================================================

namespace {
EVP_PKEY* g_privateKey = nullptr;
EVP_PKEY* g_publicKey = nullptr;
std::mutex g_jwtMutex;

EVP_PKEY* loadPrivateKey(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return nullptr;
    EVP_PKEY* key = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
    std::fclose(file);
    return key;
}

EVP_PKEY* loadPublicKey(const std::string& path, EVP_PKEY* fallback) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file) {
        EVP_PKEY* key = PEM_read_PUBKEY(file, nullptr, nullptr, nullptr);
        std::fclose(file);
        if (key) return key;
    }
    if (fallback) {
        EVP_PKEY* key = nullptr;
        if (EVP_PKEY_up_ref(fallback) == 1) key = fallback;
        return key;
    }
    return nullptr;
}

std::string rsaSign(const std::string& data) {
    std::lock_guard<std::mutex> lock(g_jwtMutex);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    size_t signatureLen = 0;
    std::string signature;
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, g_privateKey) == 1 &&
        EVP_DigestSignUpdate(ctx, data.data(), data.size()) == 1 &&
        EVP_DigestSignFinal(ctx, nullptr, &signatureLen) == 1) {
        signature.resize(signatureLen);
        if (EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(signature.data()),
                                &signatureLen) == 1) {
            signature.resize(signatureLen);
        } else {
            signature.clear();
        }
    }
    EVP_MD_CTX_free(ctx);
    return signature;
}

bool rsaVerify(const std::string& data, const std::string& signature) {
    std::lock_guard<std::mutex> lock(g_jwtMutex);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    bool ok = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, g_publicKey) == 1 &&
              EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) == 1 &&
              EVP_DigestVerifyFinal(ctx,
                                    reinterpret_cast<const unsigned char*>(signature.data()),
                                    signature.size()) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}
}  // namespace

bool RsJwt::init() {
    auto& cfg = AppConfig::get();
    g_privateKey = loadPrivateKey(cfg.signingPrivateKeyFile);
    if (!g_privateKey) {
        std::cerr << "[jwt] cannot load private key: " << cfg.signingPrivateKeyFile << std::endl;
        return false;
    }
    g_publicKey = loadPublicKey(cfg.signingPublicKeyFile, g_privateKey);
    if (!g_publicKey) {
        std::cerr << "[jwt] cannot load public key: " << cfg.signingPublicKeyFile << std::endl;
        return false;
    }
    std::cout << "[jwt] RS256 key loaded, kid=" << cfg.signingKeyId << std::endl;
    return true;
}

std::string RsJwt::kid() { return AppConfig::get().signingKeyId; }

std::string RsJwt::sign(const std::map<std::string, JsonValue>& payload) {
    std::map<std::string, JsonValue> header;
    header["alg"] = JsonValue("RS256");
    header["typ"] = JsonValue("JWT");
    header["kid"] = JsonValue(kid());
    std::string signingInput = util::base64UrlEncode(JsonValue(header).toJson()) + "." +
                               util::base64UrlEncode(JsonValue(payload).toJson());
    std::string signature = rsaSign(signingInput);
    if (signature.empty()) return "";
    return signingInput + "." + util::base64UrlEncode(signature);
}

std::optional<std::map<std::string, JsonValue>> RsJwt::verify(const std::string& token) {
    auto parts = util::split(token, '.');
    if (parts.size() != 3) return std::nullopt;
    std::string signingInput = parts[0] + "." + parts[1];
    std::string signature = util::base64UrlDecode(parts[2]);
    if (!rsaVerify(signingInput, signature)) return std::nullopt;
    std::map<std::string, JsonValue> header;
    std::map<std::string, JsonValue> payload;
    if (!jsonx::parseObject(util::base64UrlDecode(parts[0]), header)) return std::nullopt;
    if (jsonx::getString(header, "kid") != kid()) return std::nullopt;
    if (!jsonx::parseObject(util::base64UrlDecode(parts[1]), payload)) return std::nullopt;
    return payload;
}

std::string RsJwt::jwks() {
    if (!g_publicKey) return "{\"keys\":[]}";
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    EVP_PKEY_get_bn_param(g_publicKey, "n", &n);
    EVP_PKEY_get_bn_param(g_publicKey, "e", &e);
    auto bnToString = [](BIGNUM* bn) -> std::string {
        if (!bn) return "";
        int len = BN_num_bytes(bn);
        std::string raw(static_cast<size_t>(len), '\0');
        BN_bn2bin(bn, reinterpret_cast<unsigned char*>(raw.data()));
        return util::base64UrlEncode(raw);
    };
    std::string n64 = bnToString(n);
    std::string e64 = bnToString(e);
    BN_free(n);
    BN_free(e);
    std::map<std::string, JsonValue> key;
    key["kty"] = JsonValue("RSA");
    key["use"] = JsonValue("sig");
    key["alg"] = JsonValue("RS256");
    key["kid"] = JsonValue(kid());
    key["n"] = JsonValue(n64);
    key["e"] = JsonValue(e64);
    std::vector<JsonValue> keys{JsonValue(key)};
    return JsonValue(std::map<std::string, JsonValue>{{"keys", JsonValue(keys)}}).toJson();
}

std::string RsJwt::publicPem() {
    auto& cfg = AppConfig::get();
    std::string pem;
    util::readFile(cfg.signingPublicKeyFile, pem);
    return pem;
}

// ===========================================================================
// OAuth token service
// ===========================================================================

namespace tokens {

std::string createAccess(const std::string& userId, const std::string& clientId,
                         const std::string& scope, const std::string& jti) {
    auto& cfg = AppConfig::get();
    long long now = util::nowEpoch();
    std::map<std::string, JsonValue> payload;
    payload["sub"] = JsonValue(userId);
    payload["client_id"] = JsonValue(clientId);
    payload["scope"] = JsonValue(scope);
    payload["type"] = JsonValue("access");
    payload["iat"] = jsonx::num(now);
    payload["exp"] = jsonx::num(now + static_cast<long long>(cfg.accessTokenMinutes) * 60);
    payload["jti"] = JsonValue(jti);
    payload["iss"] = JsonValue(cfg.issuer);
    payload["aud"] = JsonValue(cfg.issuer + "/api/oauth/userinfo");
    return RsJwt::sign(payload);
}

std::string createRefresh(const std::string& userId, const std::string& clientId,
                          const std::string& scope, const std::string& jti) {
    auto& cfg = AppConfig::get();
    long long now = util::nowEpoch();
    std::map<std::string, JsonValue> payload;
    payload["sub"] = JsonValue(userId);
    payload["client_id"] = JsonValue(clientId);
    payload["scope"] = JsonValue(scope);
    payload["type"] = JsonValue("refresh");
    payload["iat"] = jsonx::num(now);
    payload["exp"] = jsonx::num(now + static_cast<long long>(cfg.refreshTokenDays) * 86400);
    payload["jti"] = JsonValue(jti);
    payload["iss"] = JsonValue(cfg.issuer);
    payload["aud"] = JsonValue(cfg.issuer + "/api/oauth/token");
    return RsJwt::sign(payload);
}

std::string createId(const std::string& userId, const std::string& clientId,
                     const std::map<std::string, JsonValue>& claims) {
    auto& cfg = AppConfig::get();
    long long now = util::nowEpoch();
    std::map<std::string, JsonValue> payload;
    payload["sub"] = JsonValue(userId);
    payload["type"] = JsonValue("id");
    payload["iat"] = jsonx::num(now);
    payload["exp"] = jsonx::num(now + static_cast<long long>(cfg.idTokenMinutes) * 60);
    payload["jti"] = JsonValue(util::randomToken(16));
    payload["iss"] = JsonValue(cfg.issuer);
    payload["aud"] = JsonValue(clientId);
    for (const auto& [key, value] : claims) {
        if (payload.find(key) == payload.end()) payload[key] = value;
    }
    return RsJwt::sign(payload);
}

std::optional<std::map<std::string, JsonValue>> decode(const std::string& token,
                                                      const std::string& type,
                                                      const std::string& audience) {
    auto payload = RsJwt::verify(token);
    if (!payload) return std::nullopt;
    auto& cfg = AppConfig::get();
    std::string iss = jsonx::getString(*payload, "iss");
    if (iss != cfg.issuer) return std::nullopt;
    std::string expectedAud = audience;
    if (expectedAud.empty()) {
        if (type == "refresh") expectedAud = cfg.issuer + "/api/oauth/token";
        else if (type == "access") expectedAud = cfg.issuer + "/api/oauth/userinfo";
    }
    if (!expectedAud.empty()) {
        std::string aud = jsonx::getString(*payload, "aud");
        if (aud != expectedAud) return std::nullopt;
    }
    long long exp = 0, iat = 0;
    auto expIt = payload->find("exp");
    auto iatIt = payload->find("iat");
    if (expIt != payload->end() && expIt->second.getType() == JsonValue::NUMBER)
        exp = static_cast<long long>(expIt->second.asNumber());
    if (iatIt != payload->end() && iatIt->second.getType() == JsonValue::NUMBER)
        iat = static_cast<long long>(iatIt->second.asNumber());
    if (exp <= util::nowEpoch() || iat <= 0) return std::nullopt;
    if (jsonx::getString(*payload, "type") != type) return std::nullopt;
    if (jsonx::getString(*payload, "jti").empty()) return std::nullopt;
    if (jsonx::getString(*payload, "sub").empty()) return std::nullopt;
    if (type != "id" && jsonx::getString(*payload, "client_id").empty()) return std::nullopt;
    return payload;
}

bool revoked(const std::string& jti, const std::string& type) {
    std::string table = (type == "refresh") ? "refresh_tokens" : "access_tokens";
    auto rows = Db::query("SELECT revoked FROM " + table + " WHERE token=" + Db::quote(jti) + " LIMIT 1");
    if (rows.empty()) return true;
    auto it = rows.front().find("revoked");
    if (it == rows.front().end()) return true;
    return it->second == "1";
}

bool issue(const User& user, const Application& app,
           const std::vector<std::string>& grantedScopes,
           bool includeIdToken, const std::string& nonce,
           JsonValue& response) {
    auto& cfg = AppConfig::get();
    std::string scope = scopes::join(grantedScopes);
    std::string accessJti = util::randomToken(16);
    std::string refreshJti = util::randomToken(16);
    std::string accessToken = createAccess(user.id, app.clientId, scope, accessJti);
    std::string refreshToken = createRefresh(user.id, app.clientId, scope, refreshJti);
    if (accessToken.empty() || refreshToken.empty()) return false;

    long long now = util::nowEpoch();
    long long accessExp = now + static_cast<long long>(cfg.accessTokenMinutes) * 60;
    long long refreshExp = now + static_cast<long long>(cfg.refreshTokenDays) * 86400;
    std::string insertAccess =
        "INSERT INTO access_tokens (id,token,client_id,user_id,scope,expires_at,revoked,created_at) VALUES (" +
        Db::quote(util::uuid4()) + "," + Db::quote(accessJti) + "," + Db::quote(app.clientId) + "," +
        Db::quote(user.id) + "," + Db::quote(scope) + ",FROM_UNIXTIME(" + std::to_string(accessExp) +
        "),0,NOW())";
    std::string insertRefresh =
        "INSERT INTO refresh_tokens (id,token,client_id,user_id,scope,expires_at,revoked,created_at) VALUES (" +
        Db::quote(util::uuid4()) + "," + Db::quote(refreshJti) + "," + Db::quote(app.clientId) + "," +
        Db::quote(user.id) + "," + Db::quote(scope) + ",FROM_UNIXTIME(" + std::to_string(refreshExp) +
        "),0,NOW())";
    if (Db::exec(insertAccess) < 0 || Db::exec(insertRefresh) < 0) return false;

    std::map<std::string, JsonValue> obj;
    obj["access_token"] = JsonValue(accessToken);
    obj["token_type"] = JsonValue("Bearer");
    obj["expires_in"] = jsonx::num(static_cast<long long>(cfg.accessTokenMinutes) * 60);
    obj["refresh_token"] = JsonValue(refreshToken);
    obj["scope"] = JsonValue(scope);
    obj["sso_sub"] = JsonValue(user.id);
    if (!user.sseUserId.empty()) obj["sse_sub"] = JsonValue(user.sseUserId);
    if (includeIdToken && std::find(grantedScopes.begin(), grantedScopes.end(), "openid") != grantedScopes.end()) {
        auto claims = scopes::userClaims(user, grantedScopes).asObject();
        if (!nonce.empty()) claims["nonce"] = JsonValue(nonce);
        obj["id_token"] = JsonValue(createId(user.id, app.clientId, claims));
    }
    response = JsonValue(obj);
    return true;
}

int revokeForConsent(const std::string& userId, const std::string& clientId) {
    int changed = 0;
    changed += Db::exec("UPDATE access_tokens SET revoked=1 WHERE user_id=" + Db::quote(userId) +
                        " AND client_id=" + Db::quote(clientId) + " AND revoked=0");
    changed += Db::exec("UPDATE refresh_tokens SET revoked=1 WHERE user_id=" + Db::quote(userId) +
                        " AND client_id=" + Db::quote(clientId) + " AND revoked=0");
    return changed;
}

}  // namespace tokens

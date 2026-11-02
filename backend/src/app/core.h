#pragma once

// UniSSO v2 application core: config, crypto, DB/Redis access, HTTP helpers,
// domain models and shared services built on the vendored CBSF HTTP framework.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Server.h"
#include "JsonValue.h"
#include "DBConnector.h"
#include "RDConnector.h"

// ===========================================================================
// Configuration (environment first, sensible defaults)
// ===========================================================================

namespace constants {
inline constexpr const char* kDefaultOrganizationId =
    "00000000-0000-0000-0000-000000000001";
}

struct AppConfig {
    int port = 8080;
    std::string issuer = "https://sso.ssemarket.cn";
    std::string publicUrl;
    std::string groupName;

    std::string mysqlHost = "localhost";
    int mysqlPort = 3306;
    std::string mysqlUser;
    std::string mysqlPassword;
    std::string mysqlDb = "unisso";

    std::string redisHost = "localhost";
    int redisPort = 6379;
    std::string redisUser;
    std::string redisPassword;
    std::string redisPrefix = "unisso";

    std::string signingPrivateKeyFile = "secrets/signing-private.pem";
    std::string signingPublicKeyFile = "secrets/signing-public.pem";
    std::string signingKeyId = "unisso-2026-09-08";
    std::string secretKey;

    int accessTokenMinutes = 60;
    int refreshTokenDays = 7;
    int idTokenMinutes = 60;
    int authCodeSeconds = 600;
    int sessionHours = 24;
    int stepUpDays = 7;
    int trustDeviceDays = 180;
    // Hops allowed to supply X-Forwarded-For / X-Real-IP. Anything connecting
    // from outside these ranges cannot choose its own client identity.
    std::vector<std::string> trustedProxyCidrs;

    std::string faceBaseUrl = "https://ai.ssemarket.cn/face";
    std::string faceApiKeyFile = "secrets/face-api.key";
    std::string faceApiKey;
    bool faceEnabled = false;
    double faceLoginThreshold = 0.6;
    double faceBindThreshold = 0.5;
    int faceTimeoutSeconds = 30;

    bool emailRegistrationEnabled = false;
    std::string smtpHost;
    int smtpPort = 465;
    std::string smtpUser;
    std::string smtpPassword;
    std::string smtpFrom;
    std::string smtpFromName = "UniSSO";
    bool smtpUseTls = true;
    bool smtpStartTls = false;

    bool sseEnabled = true;
    std::string sseApiBase = "https://ssemarket.cn/api";
    std::string sseWebBase = "https://ssemarket.cn";
    std::string sseAesKey = "16bit secret key";
    int sseCacheSeconds = 604800;

    bool sseOauthEnabled = false;
    std::string sseOauthAppId;
    std::string sseOauthAppSecret;
    std::string sseOauthRedirectUri = "https://sso.ssemarket.cn/api/auth/sse/oauth/callback";
    std::string sseOauthAuthorizeUrl = "https://ssemarket.cn/new/connect";
    std::string sseOauthTokenUrl = "https://ssemarket.cn/api/auth/connect/token";
    std::string sseOauthUserinfoUrl = "https://ssemarket.cn/api/auth/connect/userinfo";

    std::string webRoot = "web";
    std::string dataRoot = "data";
    std::string allowedHostsJson;
    std::string csrfOriginsJson;

    std::string reportUrl;
    std::string reportToken;
    std::string deployId;

    bool debug = false;

    static AppConfig& get();
    void load();
    bool emailAvailable() const { return emailRegistrationEnabled && !smtpHost.empty(); }
};

// ===========================================================================
// Small utilities
// ===========================================================================

namespace util {
std::string env(const std::string& name, const std::string& fallback = "");
int envInt(const std::string& name, int fallback);
bool envBool(const std::string& name, bool fallback);

std::string randomHex(size_t bytes);
std::string randomToken(size_t bytes);   // base64url, no padding
std::string uuid4();

std::string base64Encode(const std::string& raw);
std::string base64Decode(const std::string& text);
std::string base64UrlEncode(const std::string& raw);
std::string base64UrlDecode(const std::string& text);
std::string sha256Raw(const std::string& data);
std::string hmacSha256Raw(const std::string& key, const std::string& data);
std::string hex(const std::string& raw);

std::string lower(std::string value);
std::string trim(std::string value);
std::vector<std::string> split(const std::string& value, char sep);
std::string join(const std::vector<std::string>& values, const std::string& sep);
std::string urlEncode(const std::string& value);
std::string urlDecode(const std::string& value);

bool constantTimeEquals(const std::string& a, const std::string& b);
long long nowEpoch();
std::string nowIso();
std::string formatTs(long long epoch);

std::string mimeType(const std::string& filename);
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);
bool ensureDir(const std::string& path);
bool fileExists(const std::string& path);
std::string pathJoin(const std::string& a, const std::string& b);
}  // namespace util

// ===========================================================================
// JSON helpers
// ===========================================================================

namespace jsonx {
JsonValue parse(const std::string& text);
bool parseObject(const std::string& text, std::map<std::string, JsonValue>& out);
std::string dump(const std::map<std::string, JsonValue>& obj);
std::string dump(const JsonValue& value);
JsonValue str(const std::string& value);
JsonValue num(long long value);
JsonValue boolean(bool value);
std::string getString(const std::map<std::string, JsonValue>& obj,
                      const std::string& key, const std::string& fallback = "");
bool getBool(const std::map<std::string, JsonValue>& obj,
             const std::string& key, bool fallback = false);
std::vector<std::string> getStringArray(const std::map<std::string, JsonValue>& obj,
                                        const std::string& key);
JsonValue stringArray(const std::vector<std::string>& values);
}  // namespace jsonx

// ===========================================================================
// Database / cache
// ===========================================================================

class Db {
public:
    static bool init();
    static std::vector<std::map<std::string, std::string>> query(const std::string& sql);
    static int exec(const std::string& sql);
    static long long lastInsertId();
    static std::string esc(const std::string& value);
    static std::string quote(const std::string& value);
    static std::string nullable(const std::string& value);
    static bool healthy();
    static std::string lastError();
};

class Cache {
public:
    static bool init();
    static std::string key(const std::string& name);
    static std::string get(const std::string& name);
    static bool set(const std::string& name, const std::string& value, int ttl = 0);
    static bool setnx(const std::string& name, const std::string& value, int ttl);
    static bool del(const std::string& name);
    static std::string getdel(const std::string& name);
    static long long incr(const std::string& name, int ttl);
    static bool exists(const std::string& name);
    static std::string getJson(const std::string& name);
    static bool setJson(const std::string& name, const std::string& json, int ttl = 0);
    static long long ttl(const std::string& name);
    // Returns unprefixed key names matching the pattern (e.g. "session:*").
    static std::vector<std::string> scan(const std::string& pattern, size_t maxKeys = 500);
    static bool healthy();
};

// ===========================================================================
// Domain models
// ===========================================================================

struct OrgRef {
    std::string id;
    std::string name;
    std::string slug;
    std::string role;
};

struct Identity {
    std::string id;
    std::string userId;
    std::string provider;
    std::string providerUserId;
    std::string providerUsername;
    std::string providerEmail;
    std::string providerAvatar;
    bool isActive = true;
    std::string createdAt;
};

struct User {
    std::string id;
    std::string email;
    bool emailVerified = false;
    std::string passwordHash;
    std::string username;
    std::string fullName;
    std::string studentId;
    std::string avatar;
    bool isActive = true;
    bool isAdmin = false;
    bool isInternal = false;
    bool isSuperAdmin = false;
    bool stepUpEnabled = false;
    std::string sseUserId;
    std::string createdAt;
    std::string updatedAt;
    std::string lastLoginAt;
    std::vector<std::string> roles;
    std::vector<Identity> identities;
    std::vector<OrgRef> organizations;
};

struct Application {
    std::string id;
    std::string clientId;
    std::string clientSecret;
    std::string name;
    std::string description;
    std::string icon;
    std::vector<std::string> redirectUris;
    std::vector<std::string> allowedScopes;
    std::string homepageUrl;
    std::string callbackUrl;
    bool isActive = true;
    bool isConfidential = true;
    std::string ownerId;
    std::string organizationId;
    std::string createdAt;
    std::string updatedAt;
};

struct Consent {
    std::string id;
    std::string userId;
    std::string applicationId;
    std::vector<std::string> scopes;
    std::string grantedAt;
    bool isActive = true;
    Application application;
};

namespace repo {
std::optional<User> findUserByEmail(const std::string& email);
std::optional<User> findUserById(const std::string& id);
std::optional<User> findUserBySseId(const std::string& sseUserId);
std::vector<User> listUsers();
bool loadUserRelations(User& user);
bool createUser(const User& user);
bool updateUserPassword(const std::string& userId, const std::string& passwordHash);
bool updateUserLastLogin(const std::string& userId);

std::vector<Application> listApplications();
std::optional<Application> findApplicationByClientId(const std::string& clientId);
std::optional<Application> findApplicationById(const std::string& id);
std::vector<Application> listApplicationsOwnedBy(const std::string& userId);
std::vector<Application> listApplicationsForUser(const std::string& userId);

std::optional<Consent> findConsent(const std::string& userId, const std::string& applicationId);
std::vector<Consent> listConsents(const std::string& userId, bool activeOnly = true);
bool upsertConsent(const std::string& userId, const std::string& applicationId,
                   const std::vector<std::string>& scopes);
bool revokeConsent(const std::string& userId, const std::string& applicationId);

std::vector<Identity> listIdentities(const std::string& userId);
int faceCount(const std::string& userId);
std::vector<User> linkedPeers(const std::string& userId);
std::vector<JsonValue> linkedAccountsJson(const std::string& userId);
bool bindIdentity(const Identity& identity);
bool unbindIdentity(const std::string& userId, const std::string& identityId);

JsonValue userJson(const User& user, bool withRelations = true);
JsonValue applicationJson(const Application& app, bool withSecret = false);
JsonValue consentJson(const Consent& consent);
JsonValue identityJson(const Identity& identity);
}  // namespace repo

// Password hashing/verification (bcrypt through libxcrypt, compatible with
// the existing passlib $2b$ hashes).
namespace password {
std::string hash(const std::string& plain);
bool verify(const std::string& plain, const std::string& stored);
void validateStrength(const std::string& plain);  // throws std::invalid_argument
}  // namespace password

// ===========================================================================
// Scopes
// ===========================================================================

namespace scopes {
std::vector<std::string> split(const std::string& value);
std::string join(const std::vector<std::string>& values);
std::vector<std::string> expandLegacy(const std::vector<std::string>& values);
std::vector<std::string> effective(const std::vector<std::string>& requested,
                                   const std::vector<std::string>& appAllowed,
                                   const std::optional<std::vector<std::string>>& consented = std::nullopt);
std::string description(const std::string& name);
JsonValue userClaims(const User& user, const std::vector<std::string>& granted);
JsonValue catalog();
}  // namespace scopes

// ===========================================================================
// RS256 JWT + OAuth token service
// ===========================================================================

class RsJwt {
public:
    static bool init();
    static std::string sign(const std::map<std::string, JsonValue>& payload);
    static std::optional<std::map<std::string, JsonValue>> verify(const std::string& token);
    static std::string jwks();
    static std::string kid();
    static std::string publicPem();
};

namespace tokens {
std::string createAccess(const std::string& userId, const std::string& clientId,
                         const std::string& scope, const std::string& jti);
std::string createRefresh(const std::string& userId, const std::string& clientId,
                          const std::string& scope, const std::string& jti);
std::string createId(const std::string& userId, const std::string& clientId,
                     const std::map<std::string, JsonValue>& claims);
std::optional<std::map<std::string, JsonValue>> decode(const std::string& token,
                                                      const std::string& type,
                                                      const std::string& audience = "");
bool revoked(const std::string& jti, const std::string& type);
bool issue(const User& user, const Application& app,
           const std::vector<std::string>& grantedScopes,
           bool includeIdToken, const std::string& nonce,
           JsonValue& response);
int revokeForConsent(const std::string& userId, const std::string& clientId);
}  // namespace tokens

// ===========================================================================
// HTTP helpers
// ===========================================================================

namespace http {
std::string header(const Request& req, const std::string& name);
std::string cookie(const Request& req, const std::string& name);
std::string query(const Request& req, const std::string& name);
std::string param(const Request& req, const std::string& name);
std::string input(const Request& req, const std::string& name);
std::vector<std::string> inputArray(const Request& req, const std::string& name);
std::string clientIp(const Request& req);
// Semantic IPv4/IPv6 comparison ("1.2.3.4" == "::ffff:1.2.3.4").
bool sameIp(const std::string& a, const std::string& b);
std::string origin(const Request& req);
bool secure(const Request& req);
bool wantsJson(const Request& req);

void json(Response& res, int status, const JsonValue& value);
void ok(Response& res, const JsonValue& value);
void ok(Response& res);
void error(Response& res, int status, const std::string& code,
           const std::string& message = "");
void redirect(Response& res, const std::string& location, int status = 302);
void setCookie(Response& res, const Request& req, const std::string& name,
               const std::string& value, int maxAge, bool httpOnly = true,
               const std::string& sameSite = "Lax");
void clearCookie(Response& res, const Request& req, const std::string& name);
void securityHeaders(Response& res, const Request& req);
std::string originOf(const std::string& url);
}  // namespace http

// ===========================================================================
// Sessions / CSRF / auth gate
// ===========================================================================

namespace authx {
std::string createSession(const std::string& userId, const std::string& username,
                          const Request& req, const std::string& deviceHash = "");
std::optional<std::string> sessionId(const Request& req);
std::optional<User> currentUser(const Request& req);
bool requireUser(const Request& req, Response& res, User& out);
bool requireAdmin(const Request& req, Response& res, User& out);
bool requireSuperAdmin(const Request& req, Response& res, User& out);
void setSessionCookie(Response& res, const Request& req, const std::string& session);
std::string deviceHash(const std::string& token);
bool hasActiveSession(const std::string& userId);
void cancelDeviceRequests(const std::string& stepUpToken);
bool deviceTrusted(const std::string& userId, const Request& req);
std::string trustDevice(const std::string& userId, const Request& req, Response& res);
void untrustDevice(const std::string& userId, const Request& req);
// Refresh the recorded IPs of an already-trusted device without creating
// new trust (used by logins that are second-factor proofs in their own right).
void noteDeviceIp(const std::string& userId, const Request& req);
void destroySession(const Request& req, Response& res);
std::string issueCsrf(const Request& req, Response& res);
bool validateCsrf(const Request& req, Response& res);
bool allowHost(const Request& req);
}  // namespace authx

// ===========================================================================
// Email + registration
// ===========================================================================

namespace mail {
bool send(const std::string& to, const std::string& subject,
          const std::string& html, std::string& error);
}

namespace registration {
bool start(const std::string& email, const std::string& password,
           const std::string& username, const std::string& fullName,
           const std::string& clientIp, const std::string& inviteCode,
           bool& sseCodeRequired, std::string& errorCode, std::string& errorMessage);
bool resend(const std::string& email, const std::string& clientIp,
            std::string& errorCode);
bool verify(const std::string& email, const std::string& code,
            const std::string& sseCode, const std::string& password,
            std::optional<User>& created, std::string& errorCode,
            std::string& errorMessage);
}

// ===========================================================================
// Handlers / routing
// ===========================================================================

namespace handlers {
void registerAll(Server& app);

void health(const Request& req, Response& res);
void csrfToken(const Request& req, Response& res);
void authLogin(const Request& req, Response& res);
void authLogout(const Request& req, Response& res);
void authMe(const Request& req, Response& res);
void authSession(const Request& req, Response& res);
void authRegister(const Request& req, Response& res);
void authVerify(const Request& req, Response& res);
void authResend(const Request& req, Response& res);

void oauthContext(const Request& req, Response& res);
void oauthAuthorize(const Request& req, Response& res);
void oauthToken(const Request& req, Response& res);
void oauthIntrospect(const Request& req, Response& res);
void oauthUserinfo(const Request& req, Response& res);
void oidcDiscovery(const Request& req, Response& res);
void oidcJwks(const Request& req, Response& res);

void organizationsList(const Request& req, Response& res);
void organizationsCreate(const Request& req, Response& res);
void organizationsGet(const Request& req, Response& res);
void organizationsAddMember(const Request& req, Response& res);
void organizationsSetMember(const Request& req, Response& res);
void organizationsRemoveMember(const Request& req, Response& res);
void organizationsJoinApply(const Request& req, Response& res);
void orgJoinRequestsMine(const Request& req, Response& res);
void orgJoinRequestsPending(const Request& req, Response& res);
void orgJoinRequestReview(const Request& req, Response& res);

void appIntegrationSkill(const Request& req, Response& res);
void appsList(const Request& req, Response& res);
void appsCreate(const Request& req, Response& res);
void appsGet(const Request& req, Response& res);
void appsUpdate(const Request& req, Response& res);
void appsDelete(const Request& req, Response& res);
void appsResetSecret(const Request& req, Response& res);
void appsIcon(const Request& req, Response& res);

void consentsList(const Request& req, Response& res);
void consentsUpdate(const Request& req, Response& res);
void consentsRevoke(const Request& req, Response& res);

void identitiesList(const Request& req, Response& res);
void identitiesBind(const Request& req, Response& res);
void identitiesUnbind(const Request& req, Response& res);

void adminUsersList(const Request& req, Response& res);
void adminUsersCreate(const Request& req, Response& res);
void adminUsersUpdate(const Request& req, Response& res);
void adminUsersDelete(const Request& req, Response& res);
void beginStepUp(const Request& req, Response& res, const User& user,
                 const std::string& next, bool allowDeviceMethod = true,
                 std::string* outToken = nullptr);
// Shared gate for every login path (password / QR claim): untrusted device or
// dormant account on an account that has a second factor available.
// allowDeviceMethod=false refuses "confirm on another logged-in device" as
// the factor (a phone that just approved a scan must not approve itself).
bool loginNeedsStepUp(const User& user, const Request& req,
                      bool allowDeviceMethod = true);
void emailCodeStart(const Request& req, Response& res);
void emailCodeVerify(const Request& req, Response& res);
void passwordResetStart(const Request& req, Response& res);
void passwordResetConfirm(const Request& req, Response& res);
void stepUpMethods(const Request& req, Response& res);
void stepUpDeviceStart(const Request& req, Response& res);
void stepUpDeviceStatus(const Request& req, Response& res);
void stepUpDevicePending(const Request& req, Response& res);
void stepUpDeviceDecide(const Request& req, Response& res);
void stepUpSendEmail(const Request& req, Response& res);
void sessionsList(const Request& req, Response& res);
void sessionRevoke(const Request& req, Response& res);
void sessionRevokeOthers(const Request& req, Response& res);
void deviceTrustStatus(const Request& req, Response& res);
void deviceTrustSet(const Request& req, Response& res);
void stepUpVerify(const Request& req, Response& res);
void stepUpCancel(const Request& req, Response& res);
void faceStatus(const Request& req, Response& res);
void facesList(const Request& req, Response& res);
void faceSession(const Request& req, Response& res);
void faceProbe(const Request& req, Response& res);
void faceSendCode(const Request& req, Response& res);
void faceConfirm(const Request& req, Response& res);
void faceEnroll(const Request& req, Response& res);
void faceUnbind(const Request& req, Response& res);
void stepUpFaceSession(const Request& req, Response& res);
void stepUpFaceVerify(const Request& req, Response& res);
void clientLog(const Request& req, Response& res);
void adminLogsList(const Request& req, Response& res);
void adminLogsTail(const Request& req, Response& res);

void avatarGet(const Request& req, Response& res);
void avatarUpload(const Request& req, Response& res);
}  // namespace handlers

// SPA/static fallback installed as the server's not-found handler.
bool spaFallback(const Request& req, Response& res);

// Idempotent schema upgrades (organizations, groups, SSE, roles).
namespace migrations {
void run();
}

// Face service (liveness + 1:N + enrollment). Only used for the second factor
// and explicit enrollment; there is no face-only login.
namespace face {
struct LivenessSession {
    std::string sessionId;
    std::string challenge;
    int expiresIn = 120;
    int minFrames = 3;
    int maxFrames = 8;
};
struct Match {
    std::string providerPersonId;
    std::string externalId;
    double score = 0.0;
};
struct LiveResult {
    bool passed = false;
    int bestFrame = -1;
    std::string challenge;
    std::vector<Match> candidates;
    Match match;
};
bool createLivenessSession(LivenessSession& out, std::string& error);
bool recognizeLive(const std::string& sessionId,
                   const std::vector<std::string>& frames, double minScore,
                   LiveResult& out, std::string& error);
bool enrollLive(const std::string& sessionId, const std::string& externalId,
                const std::string& name, const std::vector<std::string>& frames,
                std::string& providerPersonId, std::string& error);
bool deletePerson(const std::string& providerPersonId, std::string& error);
bool deletePersonByExternalId(const std::string& externalId, std::string& error);
}

// SSE Market (院内侧) account-password login adapter.
namespace sse {
struct Profile {
    std::string userId;
    std::string email;
    std::string name;
    std::string identity;
    std::string avatar;
};
bool login(const std::string& email, const std::string& password,
           Profile& profile, std::string& error);
bool exchangeOauthCode(const std::string& code, Profile& profile, std::string& error);
bool validateEmail(const std::string& email, std::string& error);
bool registerAccount(const std::string& email, const std::string& password,
                     const std::string& name, const std::string& valiCode,
                     const std::string& cdkey, Profile& profile, std::string& error);
std::string authorizeUrl(const std::string& state);
// rejectReason (optional) explains a null return (rare: the SSE id is owned
// by a different account, or the bind failed): "sse_already_bound" etc.
std::optional<User> upsertUser(const Profile& profile, const std::string& password,
                               std::string* rejectReason = nullptr);
bool bindIdentityToUser(const std::string& userId, const Profile& profile, std::string& error);
// One UniSSO account can bind several marketplace identities. The active one
// (users.sse_user_id, surfaced as the sse_sub claim) is always listed first.
struct SseIdentityOption {
    std::string userId;
    std::string name;
    std::string email;
    bool current = false;
};
std::vector<SseIdentityOption> listSseIdentities(const User& user);
bool selectSseIdentity(const User& user, const std::string& sseUserId, std::string& error);
void selfTest();
}

namespace handlers {
void sseQrStatus(const Request& req, Response& res);
void sseQrStart(const Request& req, Response& res);
void sseQrPoll(const Request& req, Response& res);
void sseOauthCallback(const Request& req, Response& res);
void sseBindStart(const Request& req, Response& res);
void qrLoginStart(const Request& req, Response& res);
void qrLoginPoll(const Request& req, Response& res);
void qrLoginScan(const Request& req, Response& res);
void qrLoginDetail(const Request& req, Response& res);
void qrLoginConfirm(const Request& req, Response& res);
void qrLoginDeny(const Request& req, Response& res);
void accountUpdateProfile(const Request& req, Response& res);
void accountLinksList(const Request& req, Response& res);
void accountLinkSendCode(const Request& req, Response& res);
void accountLinkBind(const Request& req, Response& res);
void accountLinkUnbind(const Request& req, Response& res);
}

// Infrastructure services started once at boot.
namespace services {
void startHeartbeat();
void stopHeartbeat();
}

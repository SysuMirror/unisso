#include "core.h"
#include "YamlParser.h"
#include "app/logger.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#include <crypt.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

// ===========================================================================
// Config
// ===========================================================================

AppConfig& AppConfig::get() {
    static AppConfig config;
    static bool loaded = false;
    if (!loaded) {
        config.load();
        loaded = true;
    }
    return config;
}

void AppConfig::load() {
    // Precedence: environment > config.yml > built-in defaults.
    // The platform injects MYSQL_* / REDIS_* / SMTP credentials at runtime.
    std::string configPath = util::env("UNISSO_CONFIG_FILE", "config.yml");
    std::string localPath = util::env("UNISSO_LOCAL_CONFIG_FILE", "config.local.yml");
    YamlParser yaml(configPath);
    bool yamlLoaded = yaml.loadYaml();
    YamlParser local(localPath);
    bool localLoaded = local.loadYaml();
    if (!yamlLoaded) {
        std::cerr << "[config] " << configPath
                  << " not readable; using environment/defaults" << std::endl;
    }
    if (localLoaded) {
        std::cout << "[config] local overrides: " << localPath << std::endl;
    }
    auto yStr = [&](const std::string& key, const std::string& fallback) {
        if (localLoaded) {
            auto value = local.getString(key, "");
            if (!value.empty()) return value;
        }
        return yamlLoaded ? yaml.getString(key, fallback) : fallback;
    };
    auto yInt = [&](const std::string& key, int fallback) {
        if (localLoaded) {
            auto value = local.getString(key, "");
            if (!value.empty()) return local.getInt(key, fallback);
        }
        return yamlLoaded ? yaml.getInt(key, fallback) : fallback;
    };
    auto yBool = [&](const std::string& key, bool fallback) {
        if (localLoaded) {
            auto value = local.getString(key, "");
            if (!value.empty()) return local.getBool(key, fallback);
        }
        return yamlLoaded ? yaml.getBool(key, fallback) : fallback;
    };

    port = util::envInt("PORT", yInt("server.port", 8080));

    issuer = util::env("UNISSO_ISSUER",
                       yStr("jwt.issuer",
                            util::env("PUBLIC_URL", "https://sso.ssemarket.cn")));
    while (!issuer.empty() && issuer.back() == '/') issuer.pop_back();
    publicUrl = util::env("PUBLIC_URL", issuer);
    groupName = util::env("GROUP_NAME", "unisso");

    mysqlHost = util::env("MYSQL_HOST", yStr("mysql.host", "localhost"));
    mysqlPort = util::envInt("MYSQL_PORT", yInt("mysql.port", 3306));
    mysqlUser = util::env("MYSQL_USER", yStr("mysql.user", ""));
    mysqlPassword = util::env("MYSQL_PASSWORD", yStr("mysql.password", ""));
    mysqlDb = util::env("MYSQL_DB", yStr("mysql.database", "unisso"));

    redisHost = util::env("REDIS_HOST", yStr("redis.host", "localhost"));
    redisPort = util::envInt("REDIS_PORT", yInt("redis.port", 6379));
    redisUser = util::env("REDIS_USER", yStr("redis.username", ""));
    redisPassword = util::env("REDIS_PASSWORD", yStr("redis.password", ""));
    redisPrefix = util::env("REDIS_PREFIX", yStr("redis.prefix", "unisso"));

    signingPrivateKeyFile = util::env(
        "UNISSO_SIGNING_PRIVATE_KEY_FILE",
        yStr("jwt.private_key_file", "secrets/signing-private.pem"));
    signingPublicKeyFile = util::env(
        "UNISSO_SIGNING_PUBLIC_KEY_FILE",
        yStr("jwt.public_key_file", "secrets/signing-public.pem"));
    signingKeyId = util::env("UNISSO_SIGNING_KEY_ID",
                             yStr("jwt.signing_key_id", "unisso-2026-09-08"));
    secretKey = util::env("UNISSO_SECRET_KEY", yStr("jwt.secret", ""));

    accessTokenMinutes = util::envInt(
        "UNISSO_ACCESS_TOKEN_MINUTES", yInt("jwt.access_token_minutes", 60));
    stepUpDays = util::envInt("UNISSO_STEP_UP_DAYS",
                              yInt("security.step_up_days", 7));
    trustDeviceDays = util::envInt("UNISSO_TRUST_DEVICE_DAYS",
                                   yInt("security.trust_device_days", 180));
    if (trustDeviceDays < 1) trustDeviceDays = 1;
    if (trustDeviceDays > 3650) trustDeviceDays = 3650;

    faceBaseUrl = util::env("UNISSO_FACE_BASE_URL",
                            yStr("face.base_url", "https://ai.ssemarket.cn/face"));
    faceApiKeyFile = util::env("UNISSO_FACE_API_KEY_FILE",
                               yStr("face.api_key_file", "secrets/face-api.key"));
    faceLoginThreshold = 0.6;
    faceBindThreshold = 0.5;
    if (localLoaded) {
        faceLoginThreshold = local.getDouble("face.login_threshold", faceLoginThreshold);
        faceBindThreshold = local.getDouble("face.bind_threshold", faceBindThreshold);
    }
    if (yamlLoaded) {
        faceLoginThreshold = yaml.getDouble("face.login_threshold", faceLoginThreshold);
        faceBindThreshold = yaml.getDouble("face.bind_threshold", faceBindThreshold);
    }
    auto envDouble = [](const char* name, double fallback) {
        std::string value = util::env(name, "");
        if (value.empty()) return fallback;
        try { return std::stod(value); } catch (...) { return fallback; }
    };
    faceLoginThreshold = envDouble("UNISSO_FACE_LOGIN_THRESHOLD", faceLoginThreshold);
    faceBindThreshold = envDouble("UNISSO_FACE_BIND_THRESHOLD", faceBindThreshold);
    faceTimeoutSeconds = util::envInt("UNISSO_FACE_TIMEOUT_SECONDS",
                                      yInt("face.timeout_seconds", 30));
    faceApiKey = util::env("UNISSO_FACE_API_KEY", "");
    if (faceApiKey.empty()) {
        std::string keyFile;
        if (util::readFile(faceApiKeyFile, keyFile)) faceApiKey = util::trim(keyFile);
    }
    faceEnabled = !faceApiKey.empty();
    refreshTokenDays = util::envInt(
        "UNISSO_REFRESH_TOKEN_DAYS", yInt("jwt.refresh_token_days", 7));
    idTokenMinutes = util::envInt(
        "UNISSO_ID_TOKEN_MINUTES", yInt("jwt.id_token_minutes", 60));
    authCodeSeconds = util::envInt(
        "UNISSO_AUTH_CODE_SECONDS", yInt("oauth.authorization_code_seconds", 600));
    sessionHours = util::envInt("UNISSO_SESSION_HOURS", yInt("session.hours", 24));

    emailRegistrationEnabled = util::envBool(
        "UNISSO_EMAIL_REGISTRATION_ENABLED", yBool("smtp.enabled", false));
    // Accept the platform's UNISSO_SMTP_* names as well as the older
    // SMTP_EMAIL_* / SMTP_ADDR aliases used by the market deployment.
    std::string aliasHost = util::env("SMTP_EMAIL_HOST", util::env("SMTP_HOST", ""));
    std::string aliasUser = util::env("SMTP_EMAIL_USERNAME", util::env("SMTP_USERNAME", ""));
    std::string aliasPass = util::env("SMTP_EMAIL_PASSWORD", util::env("SMTP_PASSWORD", ""));
    int aliasPort = util::envInt("SMTP_EMAIL_PORT", util::envInt("SMTP_PORT", 465));
    std::string aliasAddr = util::env("SMTP_ADDR", "");
    if (!aliasAddr.empty()) {
        auto colon = aliasAddr.find(':');
        if (colon != std::string::npos) {
            aliasHost = aliasAddr.substr(0, colon);
            try { aliasPort = std::stoi(aliasAddr.substr(colon + 1)); } catch (...) {}
        } else {
            aliasHost = aliasAddr;
        }
    }
    smtpHost = util::env("UNISSO_SMTP_HOST", yStr("smtp.host", aliasHost));
    smtpPort = util::envInt("UNISSO_SMTP_PORT", yInt("smtp.port", aliasPort));
    smtpUser = util::env("UNISSO_SMTP_USERNAME", yStr("smtp.username", aliasUser));
    smtpPassword = util::env("UNISSO_SMTP_PASSWORD", yStr("smtp.password", aliasPass));
    smtpFrom = util::env("UNISSO_SMTP_FROM", yStr("smtp.from", smtpUser));
    smtpFromName = util::env("UNISSO_SMTP_FROM_NAME", yStr("smtp.from_name", "UniSSO"));
    smtpUseTls = util::envBool("UNISSO_SMTP_USE_TLS", yBool("smtp.use_tls", true));
    smtpStartTls = util::envBool("UNISSO_SMTP_STARTTLS", yBool("smtp.starttls", false));

    sseEnabled = util::envBool("SSE_ENABLED", yBool("sse.enabled", true));
    sseApiBase = util::env("SSE_API_BASE", yStr("sse.api_base", "https://ssemarket.cn/api"));
    sseWebBase = util::env("SSE_WEB_BASE", yStr("sse.web_base", "https://ssemarket.cn"));
    sseAesKey = util::env("SSE_AES_KEY", yStr("sse.aes_key", "16bit secret key"));
    sseCacheSeconds = util::envInt("SSE_CACHE_SECONDS", yInt("sse.cache_seconds", 604800));
    sseOauthAppId = util::env("SSE_OAUTH_APP_ID", yStr("sse_oauth.app_id", ""));
    sseOauthAppSecret = util::env("SSE_OAUTH_APP_SECRET", yStr("sse_oauth.app_secret", ""));
    sseOauthRedirectUri = util::env("SSE_OAUTH_REDIRECT_URI",
        yStr("sse_oauth.redirect_uri", "https://sso.ssemarket.cn/api/auth/sse/oauth/callback"));
    sseOauthAuthorizeUrl = util::env("SSE_OAUTH_AUTHORIZE_URL",
        yStr("sse_oauth.authorize_url", "https://ssemarket.cn/new/connect"));
    sseOauthTokenUrl = util::env("SSE_OAUTH_TOKEN_URL",
        yStr("sse_oauth.token_url", "https://ssemarket.cn/api/auth/connect/token"));
    sseOauthUserinfoUrl = util::env("SSE_OAUTH_USERINFO_URL",
        yStr("sse_oauth.userinfo_url", "https://ssemarket.cn/api/auth/connect/userinfo"));
    sseOauthEnabled = util::envBool("SSE_OAUTH_ENABLED",
        yBool("sse_oauth.enabled", !sseOauthAppId.empty() && !sseOauthAppSecret.empty()));

    webRoot = util::env("UNISSO_WEB_ROOT", yStr("web.root", "web"));
    dataRoot = util::env("UNISSO_DATA_ROOT", yStr("web.data_root", "data"));

    allowedHostsJson = util::env("UNISSO_ALLOWED_HOSTS", "");
    if (allowedHostsJson.empty() && yamlLoaded) {
        auto hosts = yaml.getStringArray("security.allowed_hosts");
        if (!hosts.empty()) allowedHostsJson = JsonValue(hosts).toJson();
    }
    csrfOriginsJson = util::env("UNISSO_CSRF_TRUSTED_ORIGINS", "");
    if (csrfOriginsJson.empty() && yamlLoaded) {
        auto origins = yaml.getStringArray("security.csrf_trusted_origins");
        if (!origins.empty()) csrfOriginsJson = JsonValue(origins).toJson();
    }
    trustedProxyCidrs.clear();
    if (yamlLoaded) trustedProxyCidrs = yaml.getStringArray("server.trusted_proxy_cidrs");
    std::string envCidrs = util::env("UNISSO_TRUSTED_PROXY_CIDRS", "");
    if (!envCidrs.empty()) {
        trustedProxyCidrs.clear();
        for (auto& item : util::split(envCidrs, ',')) {
            item = util::trim(item);
            if (!item.empty()) trustedProxyCidrs.push_back(item);
        }
    }
    if (trustedProxyCidrs.empty()) {
        // Loopback, the k3s pod CIDR, and the relay's docker bridge — the hop
        // our public entry forwards through. Overridable via env / config.yml.
        trustedProxyCidrs = {"127.0.0.0/8", "::1", "10.42.0.0/16", "172.18.0.0/16"};
    }

    reportUrl = util::env("REPORT_URL", "");
    reportToken = util::env("REPORT_TOKEN", "");
    deployId = util::env("DEPLOY_ID", "");

    debug = util::envBool("UNISSO_DEBUG", false);
    if (secretKey.empty()) {
        // Registration code hashing needs a stable per-deployment secret.
        secretKey = util::env("UNISSO_ADMIN_PASSWORD", "");
    }
}

// ===========================================================================
// Utilities
// ===========================================================================

namespace util {

std::string env(const std::string& name, const std::string& fallback) {
    const char* value = std::getenv(name.c_str());
    return (value && *value) ? std::string(value) : fallback;
}

int envInt(const std::string& name, int fallback) {
    const char* value = std::getenv(name.c_str());
    if (!value || !*value) return fallback;
    try { return std::stoi(value); } catch (...) { return fallback; }
}

bool envBool(const std::string& name, bool fallback) {
    const char* value = std::getenv(name.c_str());
    if (!value || !*value) return fallback;
    std::string v = lower(value);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::string randomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), static_cast<int>(bytes)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return hex(raw);
}

std::string randomToken(size_t bytes) {
    std::string raw(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), static_cast<int>(bytes)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return base64UrlEncode(raw);
}

std::string uuid4() {
    unsigned char b[16];
    if (RAND_bytes(b, sizeof(b)) != 1) throw std::runtime_error("RAND_bytes failed");
    b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80);
    char out[37];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out);
}

std::string base64Encode(const std::string& raw) {
    if (raw.empty()) return "";
    std::string out(4 * ((raw.size() + 2) / 3), '\0');
    int len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                              reinterpret_cast<const unsigned char*>(raw.data()),
                              static_cast<int>(raw.size()));
    if (len < 0) return "";
    out.resize(static_cast<size_t>(len));
    return out;
}

std::string base64Decode(const std::string& text) {
    if (text.empty()) return "";
    std::string in = text;
    while (in.size() % 4 != 0) in.push_back('=');
    std::string out((in.size() / 4) * 3, '\0');
    int len = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                              reinterpret_cast<const unsigned char*>(in.data()),
                              static_cast<int>(in.size()));
    if (len < 0) return "";
    size_t padding = 0;
    if (!in.empty() && in[in.size() - 1] == '=') ++padding;
    if (in.size() > 1 && in[in.size() - 2] == '=') ++padding;
    if (static_cast<size_t>(len) >= padding) len -= static_cast<int>(padding);
    out.resize(static_cast<size_t>(len));
    return out;
}

std::string base64UrlEncode(const std::string& raw) {
    std::string out = base64Encode(raw);
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

std::string base64UrlDecode(const std::string& text) {
    std::string in = text;
    for (char& c : in) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    return base64Decode(in);
}

std::string sha256Raw(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), digest, &len, EVP_sha256(), nullptr);
    return std::string(reinterpret_cast<char*>(digest), len);
}

std::string hmacSha256Raw(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, &len);
    return std::string(reinterpret_cast<char*>(digest), len);
}

std::string hex(const std::string& raw) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0f]);
    }
    return out;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim(std::string value) {
    size_t start = value.find_first_not_of(" \t\r\n");
    size_t end = value.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return value.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string& value, char sep) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream iss(value);
    while (std::getline(iss, item, sep)) out.push_back(item);
    return out;
}

std::string join(const std::vector<std::string>& values, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += sep;
        out += values[i];
    }
    return out;
}

std::string urlEncode(const std::string& value) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('%'); out.push_back('2'); out.push_back('0');
        } else {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0x0f]);
        }
    }
    return out;
}

std::string urlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            auto h = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = h(value[i + 1]), lo = h(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

long long nowEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string formatTs(long long epoch) {
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

std::string nowIso() { return formatTs(nowEpoch()); }

std::string mimeType(const std::string& filename) {
    std::string ext = fs::path(filename).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".webp", "image/webp"},
        {".ico", "image/x-icon"},
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".map", "application/json"},
        {".txt", "text/plain; charset=utf-8"},
    };
    auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

bool ensureDir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return !ec;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

std::string pathJoin(const std::string& a, const std::string& b) {
    return (fs::path(a) / b).string();
}

}  // namespace util

// ===========================================================================
// JSON helpers
// ===========================================================================

namespace jsonx {

JsonValue parse(const std::string& text) {
    JsonValue value;
    try { value.fromJson(text); } catch (...) { return JsonValue(); }
    return value;
}

bool parseObject(const std::string& text, std::map<std::string, JsonValue>& out) {
    JsonValue value = parse(text);
    if (value.getType() != JsonValue::OBJECT) return false;
    out = value.asObject();
    return true;
}

std::string dump(const std::map<std::string, JsonValue>& obj) {
    return JsonValue(obj).toJson();
}

std::string dump(const JsonValue& value) { return value.toJson(); }

JsonValue str(const std::string& value) { return JsonValue(value); }
JsonValue num(long long value) { return JsonValue(static_cast<int>(value)); }
JsonValue boolean(bool value) { return JsonValue(value); }

std::string getString(const std::map<std::string, JsonValue>& obj,
                      const std::string& key, const std::string& fallback) {
    auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    if (it->second.getType() == JsonValue::STRING) return it->second.asString();
    return fallback;
}

bool getBool(const std::map<std::string, JsonValue>& obj,
             const std::string& key, bool fallback) {
    auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    if (it->second.getType() == JsonValue::BOOLEAN) return it->second.asBoolean();
    return fallback;
}

std::vector<std::string> getStringArray(const std::map<std::string, JsonValue>& obj,
                                        const std::string& key) {
    std::vector<std::string> out;
    auto it = obj.find(key);
    if (it == obj.end()) return out;
    if (it->second.getType() == JsonValue::ARRAY) {
        for (const auto& item : it->second.asArray()) {
            if (item.getType() == JsonValue::STRING) out.push_back(item.asString());
        }
    } else if (it->second.getType() == JsonValue::STRING) {
        std::string raw = it->second.asString();
        JsonValue parsed;
        try { parsed.fromJson(raw); } catch (...) {}
        if (parsed.getType() == JsonValue::ARRAY) {
            for (const auto& item : parsed.asArray()) {
                if (item.getType() == JsonValue::STRING) out.push_back(item.asString());
            }
        } else if (!raw.empty()) {
            out = scopes::split(raw);
        }
    }
    return out;
}

JsonValue stringArray(const std::vector<std::string>& values) {
    return JsonValue(values);
}

}  // namespace jsonx

// ===========================================================================
// Database
// ===========================================================================

namespace {
DBConnector* g_db = nullptr;
}

bool Db::init() {
    auto& cfg = AppConfig::get();
    try {
        DBConnector::initInstance(cfg.mysqlHost, cfg.mysqlUser, cfg.mysqlPassword,
                                  cfg.mysqlDb, static_cast<unsigned int>(cfg.mysqlPort));
        g_db = DBConnector::getInstance();
        if (!g_db) return false;
        if (!g_db->connect()) {
            std::cerr << "[db] connect failed: " << g_db->getError() << std::endl;
            return false;
        }
        g_db->query("SET NAMES utf8mb4 COLLATE utf8mb4_unicode_ci");
        auto collation = g_db->query("SELECT @@collation_connection AS c");
        if (!collation.empty()) {
            std::cout << "[db] collation=" << collation.front()["c"] << std::endl;
        }
        migrations::run();
        std::cout << "[db] connected " << cfg.mysqlHost << ":" << cfg.mysqlPort
                  << "/" << cfg.mysqlDb << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[db] init failed: " << e.what() << std::endl;
        return false;
    }
}

std::vector<std::map<std::string, std::string>> Db::query(const std::string& sql) {
    if (!g_db) return {};
    return g_db->query(sql);
}

int Db::exec(const std::string& sql) {
    if (!g_db) return -1;
    return g_db->execute(sql);
}

long long Db::lastInsertId() { return g_db ? g_db->insertId() : 0; }

std::string Db::esc(const std::string& value) {
    return DBConnector::escapeSqlLiteral(value);
}

std::string Db::quote(const std::string& value) {
    return "'" + DBConnector::escapeSqlLiteral(value) + "'";
}

std::string Db::nullable(const std::string& value) {
    return value.empty() ? "NULL" : quote(value);
}

bool Db::healthy() {
    auto rows = query("SELECT 1 AS ok");
    return !rows.empty();
}

std::string Db::lastError() { return g_db ? g_db->getError() : "database not initialised"; }

// ===========================================================================
// Redis
// ===========================================================================

namespace {
RdConnector* g_redis = nullptr;
}

bool Cache::init() {
    auto& cfg = AppConfig::get();
    g_redis = new RdConnector(cfg.redisHost, std::to_string(cfg.redisPort),
                              cfg.redisPassword, 0, cfg.redisUser);
    if (!g_redis->connect()) {
        std::cerr << "[redis] connect failed: " << g_redis->getError() << std::endl;
        return false;
    }
    std::cout << "[redis] connected " << cfg.redisHost << ":" << cfg.redisPort
              << " prefix=" << cfg.redisPrefix << std::endl;
    return true;
}

std::string Cache::key(const std::string& name) {
    return AppConfig::get().redisPrefix + ":" + name;
}

std::string Cache::get(const std::string& name) {
    return g_redis ? g_redis->get(key(name)) : "";
}

bool Cache::set(const std::string& name, const std::string& value, int ttl) {
    if (!g_redis) return false;
    return ttl > 0 ? g_redis->set(key(name), value, ttl) : g_redis->set(key(name), value);
}

bool Cache::setnx(const std::string& name, const std::string& value, int ttl) {
    return g_redis ? g_redis->setnx(key(name), value, ttl) : false;
}

bool Cache::del(const std::string& name) {
    return g_redis ? g_redis->del(key(name)) : false;
}

std::string Cache::getdel(const std::string& name) {
    return g_redis ? g_redis->getdel(key(name)) : "";
}

long long Cache::incr(const std::string& name, int ttl) {
    if (!g_redis) return -1;
    long long value = g_redis->incr(key(name));
    if (value == 1 && ttl > 0) g_redis->expire(key(name), ttl);
    return value;
}

bool Cache::exists(const std::string& name) { return !get(name).empty(); }

std::string Cache::getJson(const std::string& name) { return get(name); }

bool Cache::setJson(const std::string& name, const std::string& json, int ttl) {
    return set(name, json, ttl);
}

long long Cache::ttl(const std::string& name) {
    return g_redis ? g_redis->ttl(key(name)) : -2;
}

std::vector<std::string> Cache::scan(const std::string& pattern, size_t maxKeys) {
    std::vector<std::string> out;
    if (!g_redis) return out;
    std::string prefix = AppConfig::get().redisPrefix + ":";
    for (auto& full : g_redis->scan(key(pattern), maxKeys)) {
        if (full.rfind(prefix, 0) == 0) {
            out.push_back(full.substr(prefix.size()));
        } else {
            out.push_back(full);
        }
    }
    return out;
}

bool Cache::healthy() {
    if (!g_redis) return false;
    g_redis->set(key("healthcheck"), "1", 5);
    return g_redis->get(key("healthcheck")) == "1";
}

// ===========================================================================
// Password hashing (bcrypt via libxcrypt; compatible with passlib $2b$)
// ===========================================================================

namespace password {
namespace {
std::mutex cryptMutex;
}

std::string hash(const std::string& plain) {
    std::lock_guard<std::mutex> lock(cryptMutex);
    unsigned char raw[16];
    if (RAND_bytes(raw, sizeof(raw)) != 1) throw std::runtime_error("RAND_bytes failed");
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    if (!crypt_gensalt_rn("$2b$", 12, reinterpret_cast<const char*>(raw),
                          static_cast<int>(sizeof(raw)), salt, sizeof(salt))) {
        throw std::runtime_error("crypt_gensalt failed");
    }
    char* result = crypt(plain.c_str(), salt);
    if (!result) throw std::runtime_error("crypt failed");
    return std::string(result);
}

bool verify(const std::string& plain, const std::string& stored) {
    if (stored.empty()) return false;
    std::lock_guard<std::mutex> lock(cryptMutex);
    char* result = crypt(plain.c_str(), stored.c_str());
    if (!result) return false;
    return util::constantTimeEquals(std::string(result), stored);
}

void validateStrength(const std::string& plain) {
    if (plain.size() < 8) throw std::invalid_argument("密码长度不能少于 8 位");
    if (plain.size() > 128) throw std::invalid_argument("密码长度不能超过 128 位");
    bool upper = false, lower = false, digit = false, symbol = false;
    for (unsigned char c : plain) {
        upper |= std::isupper(c);
        lower |= std::islower(c);
        digit |= std::isdigit(c);
        symbol |= !std::isalnum(c) && !std::isspace(c);
    }
    int categories = static_cast<int>(upper) + static_cast<int>(lower) +
                     static_cast<int>(digit) + static_cast<int>(symbol);
    if (categories < 2) throw std::invalid_argument("密码至少需要包含两类字符");
    std::string lowered = util::lower(plain);
    static const std::vector<std::string> weak = {
        "password", "123456", "12345678", "qwerty", "abc123", "admin",
        "welcome", "letmein", "iloveyou", "dragon", "monkey", "sunshine",
    };
    for (const auto& w : weak) {
        if (lowered == w) throw std::invalid_argument("密码过于常见，请更换");
    }
    for (size_t i = 0; i + 3 < plain.size(); ++i) {
        if (plain[i] == plain[i + 1] && plain[i] == plain[i + 2] && plain[i] == plain[i + 3]) {
            throw std::invalid_argument("密码不能包含连续 4 个相同字符");
        }
    }
    static const std::vector<std::string> sequences = {
        "0123", "1234", "2345", "3456", "4567", "5678", "6789",
        "9876", "8765", "7654", "6543", "5432", "4321", "3210",
    };
    for (const auto& seq : sequences) {
        if (plain.find(seq) != std::string::npos) {
            throw std::invalid_argument("密码不能包含连续数字序列");
        }
    }
}
}  // namespace password

// ===========================================================================
// Scopes
// ===========================================================================

namespace scopes {

std::vector<std::string> split(const std::string& value) {
    std::vector<std::string> out;
    std::istringstream iss(value);
    std::string item;
    while (iss >> item) out.push_back(item);
    return out;
}

std::string join(const std::vector<std::string>& values) {
    return util::join(values, " ");
}

std::vector<std::string> expandLegacy(const std::vector<std::string>& values) {
    static const std::map<std::string, std::vector<std::string>> legacy = {
        {"profile", {"profile:username", "profile:name", "profile:avatar", "profile:student_id"}},
    };
    std::vector<std::string> out;
    for (const auto& value : values) {
        auto it = legacy.find(value);
        if (it == legacy.end()) {
            if (std::find(out.begin(), out.end(), value) == out.end()) out.push_back(value);
        } else {
            for (const auto& expanded : it->second) {
                if (std::find(out.begin(), out.end(), expanded) == out.end()) out.push_back(expanded);
            }
        }
    }
    return out;
}

std::vector<std::string> effective(const std::vector<std::string>& requested,
                                   const std::vector<std::string>& appAllowed,
                                   const std::optional<std::vector<std::string>>& consented) {
    std::vector<std::string> allowed = expandLegacy(appAllowed);
    if (std::find(allowed.begin(), allowed.end(), "openid") == allowed.end()) allowed.push_back("openid");
    std::vector<std::string> out;
    for (const auto& scope : expandLegacy(requested)) {
        // User groups are application-owned; UniSSO no longer issues them.
        if (scope == "groups" || scope == "organizations") continue;
        if (std::find(allowed.begin(), allowed.end(), scope) == allowed.end()) continue;
        if (consented.has_value()) {
            auto consentedExpanded = expandLegacy(*consented);
            if (std::find(consentedExpanded.begin(), consentedExpanded.end(), scope) == consentedExpanded.end()) continue;
        }
        out.push_back(scope);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string description(const std::string& name) {
    static const std::map<std::string, std::string> defs = {
        {"openid", "基础身份标识（用户唯一 ID）"},
        {"profile:username", "用户昵称"},
        {"profile:name", "真实姓名"},
        {"profile:avatar", "头像"},
        {"profile:student_id", "学号"},
        {"email", "邮箱地址"},
        {"roles", "用户角色列表"},
    };
    auto it = defs.find(name);
    return it == defs.end() ? name : it->second;
}

JsonValue catalog() {
    std::vector<JsonValue> items;
    for (const auto& name : {"openid", "profile:username", "profile:name",
                             "profile:avatar", "profile:student_id", "email", "roles"}) {
        std::map<std::string, JsonValue> item;
        item["name"] = JsonValue(std::string(name));
        item["description"] = JsonValue(description(name));
        items.push_back(JsonValue(item));
    }
    return JsonValue(items);
}

JsonValue userClaims(const User& user, const std::vector<std::string>& granted) {
    std::map<std::string, JsonValue> claims;
    claims["sub"] = JsonValue(user.id);
    claims["sso_sub"] = JsonValue(user.id);
    if (!user.sseUserId.empty()) claims["sse_sub"] = JsonValue(user.sseUserId);
    auto has = [&granted](const std::string& scope) {
        return std::find(granted.begin(), granted.end(), scope) != granted.end();
    };
    if (has("profile:username") && !user.username.empty())
        claims["preferred_username"] = JsonValue(user.username);
    if (has("profile:name") && !user.fullName.empty())
        claims["name"] = JsonValue(user.fullName);
    if (has("profile:avatar") && !user.avatar.empty())
        claims["picture"] = JsonValue(user.avatar);
    if (has("profile:student_id") && !user.studentId.empty())
        claims["student_id"] = JsonValue(user.studentId);
    if (has("email")) {
        claims["email"] = JsonValue(user.email);
        claims["email_verified"] = JsonValue(user.emailVerified);
    }
    if (has("roles")) {
        std::vector<std::string> roles = user.roles;
        claims["roles"] = JsonValue(roles);
    }
    claims["internal"] = JsonValue(user.isInternal);
    return JsonValue(claims);
}

}  // namespace scopes

// ===========================================================================
// HTTP helpers
// ===========================================================================

namespace http {

namespace {
std::string lowerHeaderName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}
}  // namespace

std::string header(const Request& req, const std::string& name) {
    std::string wanted = lowerHeaderName(name);
    for (const auto& [key, value] : req.headers) {
        if (lowerHeaderName(key) == wanted) return value;
    }
    return "";
}

std::string cookie(const Request& req, const std::string& name) {
    std::string raw = header(req, "Cookie");
    for (const auto& part : util::split(raw, ';')) {
        std::string item = util::trim(part);
        size_t eq = item.find('=');
        if (eq != std::string::npos && util::trim(item.substr(0, eq)) == name) {
            return util::trim(item.substr(eq + 1));
        }
    }
    return "";
}

std::string query(const Request& req, const std::string& name) {
    auto it = req.queryParams.find(name);
    return it == req.queryParams.end() ? "" : it->second;
}

std::string param(const Request& req, const std::string& name) {
    auto it = req.pathParams.find(name);
    return it == req.pathParams.end() ? "" : it->second;
}

std::string input(const Request& req, const std::string& name) {
    if (req.jsonBody) {
        auto obj = req.jsonBody->asObject();
        auto it = obj.find(name);
        if (it != obj.end()) {
            if (it->second.getType() == JsonValue::STRING) return it->second.asString();
            if (it->second.getType() == JsonValue::NUMBER) return std::to_string(static_cast<long long>(it->second.asNumber()));
            if (it->second.getType() == JsonValue::BOOLEAN) return it->second.asBoolean() ? "true" : "false";
            if (it->second.getType() == JsonValue::ARRAY) return it->second.toJson();
        }
    }
    std::string value = req.body_param(name);
    if (!value.empty()) return value;
    auto it = req.queryParams.find(name);
    return it == req.queryParams.end() ? "" : it->second;
}

std::vector<std::string> inputArray(const Request& req, const std::string& name) {
    std::string raw;
    // HTML forms / OAuth clients may repeat the same key (scopes=a&scopes=b).
    std::string contentType = util::lower(header(req, "Content-Type"));
    if (contentType.find("application/x-www-form-urlencoded") != std::string::npos &&
        !req.body.empty()) {
        std::vector<std::string> repeated;
        for (const auto& part : util::split(req.body, '&')) {
            size_t eq = part.find('=');
            if (eq == std::string::npos) continue;
            if (util::urlDecode(part.substr(0, eq)) == name) {
                repeated.push_back(util::urlDecode(part.substr(eq + 1)));
            }
        }
        if (repeated.size() > 1) return repeated;
        if (repeated.size() == 1) raw = repeated.front();
    }
    if (raw.empty() && req.jsonBody) {
        auto obj = req.jsonBody->asObject();
        auto it = obj.find(name);
        if (it != obj.end()) {
            if (it->second.getType() == JsonValue::ARRAY) {
                std::vector<std::string> out;
                for (const auto& item : it->second.asArray()) {
                    if (item.getType() == JsonValue::STRING) out.push_back(item.asString());
                }
                return out;
            }
            if (it->second.getType() == JsonValue::STRING) raw = it->second.asString();
        }
    }
    if (raw.empty()) raw = req.body_param(name);
    if (raw.empty()) return {};
    JsonValue parsed;
    try { parsed.fromJson(raw); } catch (...) {}
    if (parsed.getType() != JsonValue::ARRAY && raw.front() == '[') {
        std::string normalized = raw;
        std::replace(normalized.begin(), normalized.end(), '\'', '"');
        try { parsed.fromJson(normalized); } catch (...) {}
    }
    if (parsed.getType() == JsonValue::ARRAY) {
        std::vector<std::string> out;
        for (const auto& item : parsed.asArray()) {
            if (item.getType() == JsonValue::STRING) out.push_back(item.asString());
        }
        return out;
    }
    return util::split(raw, ',');
}

namespace {

std::string normalizeIpText(const std::string& value) {
    std::string ip = util::trim(value);
    if (ip.size() >= 2 && ip.front() == '[' && ip.back() == ']')
        ip = ip.substr(1, ip.size() - 2);
    if (ip.rfind("::ffff:", 0) == 0) ip = ip.substr(7);
    return ip;
}

// Parse an address into 16 bytes; IPv4 is stored as v4-in-v6 so the two
// families compare on equal footing.
bool ipBytes(const std::string& value, unsigned char out[16]) {
    std::string ip = normalizeIpText(value);
    if (ip.empty()) return false;
    in_addr v4;
    if (inet_pton(AF_INET, ip.c_str(), &v4) == 1) {
        std::memset(out, 0, 10);
        out[10] = 0xff;
        out[11] = 0xff;
        std::memcpy(out + 12, &v4.s_addr, 4);
        return true;
    }
    in6_addr v6;
    if (inet_pton(AF_INET6, ip.c_str(), &v6) == 1) {
        std::memcpy(out, v6.s6_addr, 16);
        return true;
    }
    return false;
}

bool ipInCidr(const std::string& ip, const std::string& cidrIn) {
    std::string cidr = util::trim(cidrIn);
    if (cidr.empty()) return false;
    size_t slash = cidr.find('/');
    std::string base = slash == std::string::npos ? cidr : cidr.substr(0, slash);
    bool baseV4 = base.find(':') == std::string::npos;
    int bits = baseV4 ? 32 : 128;
    if (slash != std::string::npos) {
        try { bits = std::stoi(cidr.substr(slash + 1)); }
        catch (...) { return false; }
    }
    if (bits < 0 || (baseV4 && bits > 32)) return false;
    if (bits > 128) bits = 128;
    int effective = (baseV4 ? 96 : 0) + bits;
    unsigned char a[16], b[16];
    if (!ipBytes(ip, a) || !ipBytes(base, b)) return false;
    for (int i = 0; i < 16; ++i) {
        int rem = effective - i * 8;
        if (rem <= 0) break;
        if (rem >= 8) {
            if (a[i] != b[i]) return false;
        } else {
            unsigned mask = (0xffu << (8 - rem)) & 0xffu;
            if ((a[i] & mask) != (b[i] & mask)) return false;
        }
    }
    return true;
}

bool hopTrusted(const std::string& ip) {
    for (const auto& cidr : AppConfig::get().trustedProxyCidrs) {
        if (ipInCidr(ip, cidr)) return true;
    }
    return false;
}

}  // namespace

std::string clientIp(const Request& req) {
    std::string peer = normalizeIpText(req.remoteAddr);
    // Only proxies inside the trusted CIDR list may supply forwarding headers.
    // Any other peer cannot choose its own client identity (rate limits,
    // device trust and audit all key off this value).
    if (!hopTrusted(peer)) return peer;
    std::string forwarded = header(req, "X-Forwarded-For");
    if (!forwarded.empty()) {
        auto parts = util::split(forwarded, ',');
        // Walk right-to-left: the first address appended by an untrusted hop
        // is the client. If every listed hop is trusted, keep the leftmost.
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            std::string candidate = normalizeIpText(*it);
            if (candidate.empty()) continue;
            if (!hopTrusted(candidate)) return candidate;
            if (it + 1 == parts.rend()) return candidate;
        }
    }
    std::string real = normalizeIpText(header(req, "X-Real-IP"));
    if (!real.empty()) return real;
    return peer;
}

bool sameIp(const std::string& a, const std::string& b) {
    unsigned char left[16], right[16];
    if (!ipBytes(a, left) || !ipBytes(b, right)) return false;
    return std::memcmp(left, right, 16) == 0;
}

std::string origin(const Request& req) { return header(req, "Origin"); }

bool secure(const Request& req) {
    return util::lower(header(req, "X-Forwarded-Proto")) == "https" ||
           util::lower(header(req, "X-Forwarded-Scheme")) == "https";
}

bool wantsJson(const Request& req) {
    std::string path = req.path;
    if (path.rfind("/api/", 0) == 0 || path.rfind("/.well-known/", 0) == 0) return true;
    return header(req, "Accept").find("application/json") != std::string::npos;
}

void json(Response& res, int status, const JsonValue& value) {
    res.statusCode = status;
    res.body = value.toJson();
    res.headers["Content-Type"] = "application/json; charset=utf-8";
}

void ok(Response& res, const JsonValue& value) {
    auto obj = value.asObject();
    obj["ok"] = JsonValue(true);
    json(res, 200, JsonValue(obj));
}

void ok(Response& res) {
    json(res, 200, JsonValue(std::map<std::string, JsonValue>{{"ok", JsonValue(true)}}));
}

void error(Response& res, int status, const std::string& code,
           const std::string& message) {
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(false);
    obj["error"] = JsonValue(code);
    if (!message.empty()) obj["message"] = JsonValue(message);
    json(res, status, JsonValue(obj));
    applog::error(std::to_string(status) + " " + code + " " + message);
}

void redirect(Response& res, const std::string& location, int status) {
    res.statusCode = status;
    res.headers["Location"] = location;
    res.body.clear();
    res.headers["Content-Type"] = "text/plain; charset=utf-8";
}

void setCookie(Response& res, const Request& req, const std::string& name,
               const std::string& value, int maxAge, bool httpOnly,
               const std::string& sameSite) {
    std::string cookie = name + "=" + value + "; Path=/; Max-Age=" + std::to_string(maxAge) +
                         "; SameSite=" + sameSite;
    if (httpOnly) cookie += "; HttpOnly";
    if (secure(req)) cookie += "; Secure";
    res.addHeader("Set-Cookie", cookie);
}

void clearCookie(Response& res, const Request& req, const std::string& name) {
    setCookie(res, req, name, "", 0, true);
}

void securityHeaders(Response& res, const Request& req) {
    auto& cfg = AppConfig::get();
    std::string issuerOrigin = originOf(cfg.issuer);
    res.headers["X-Content-Type-Options"] = "nosniff";
    res.headers["X-Frame-Options"] = "DENY";
    res.headers["Referrer-Policy"] = "strict-origin-when-cross-origin";
    res.headers["Permissions-Policy"] =
        "accelerometer=(), camera=(self), geolocation=(), gyroscope=(), magnetometer=(), "
        "microphone=(), payment=(), usb=()";
    res.headers["Content-Security-Policy"] =
        "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
        "img-src 'self' data: blob: https:; font-src 'self'; connect-src 'self' " +
        issuerOrigin + "; frame-ancestors 'none'; base-uri 'self'; form-action 'self' " +
        issuerOrigin;
    if (secure(req)) {
        res.headers["Strict-Transport-Security"] = "max-age=31536000; includeSubDomains";
    }
}

std::string originOf(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    size_t start = scheme + 3;
    size_t end = url.find('/', start);
    return end == std::string::npos ? url : url.substr(0, end);
}

}  // namespace http

// ===========================================================================
// Sessions / CSRF
// ===========================================================================


namespace {

// trusted_devices.known_ips is a JSON array of the most recent addresses this
// browser authenticated from (campus NAT and dual-stack flap would otherwise
// turn a returning device into a "new device" every few hours).
std::vector<std::string> knownIpList(const std::string& rawJson) {
    std::vector<std::string> out;
    if (rawJson.empty()) return out;
    JsonValue value;
    try { value.fromJson(rawJson); } catch (...) {}
    if (value.getType() == JsonValue::ARRAY) {
        for (const auto& item : value.asArray()) {
            if (item.getType() == JsonValue::STRING && !item.asString().empty())
                out.push_back(item.asString());
        }
    }
    return out;
}

std::string addKnownIp(const std::string& rawJson, const std::string& ip) {
    if (ip.empty()) return rawJson;
    auto list = knownIpList(rawJson);
    for (const auto& item : list) {
        if (http::sameIp(item, ip)) return JsonValue(list).toJson();
    }
    list.push_back(ip);
    if (list.size() > 8) list.erase(list.begin());
    return JsonValue(list).toJson();
}

bool ipSeenForDevice(const std::string& storedIp, const std::string& knownJson,
                     const std::string& current) {
    if (current.empty()) return false;
    if (!storedIp.empty() && http::sameIp(storedIp, current)) return true;
    for (const auto& item : knownIpList(knownJson)) {
        if (http::sameIp(item, current)) return true;
    }
    return false;
}

std::vector<std::string> configStringList(const std::string& raw) {
    std::vector<std::string> out;
    if (raw.empty()) return out;
    JsonValue value;
    try { value.fromJson(raw); } catch (...) {}
    if (value.getType() == JsonValue::ARRAY) {
        for (const auto& item : value.asArray()) {
            if (item.getType() == JsonValue::STRING) out.push_back(item.asString());
        }
        return out;
    }
    for (auto& item : util::split(raw, ',')) {
        item = util::trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}
}  // namespace

namespace authx {

std::string createSession(const std::string& userId, const std::string& username,
                          const Request& req, const std::string& deviceHash) {
    auto& cfg = AppConfig::get();
    std::string id = "sess_" + util::randomHex(16);
    std::string ua = http::header(req, "User-Agent");
    if (ua.size() > 255) ua.resize(255);
    long long now = util::nowEpoch();
    std::map<std::string, JsonValue> data;
    data["user_id"] = JsonValue(userId);
    data["username"] = JsonValue(username);
    data["user_agent"] = JsonValue(ua);
    data["ip"] = JsonValue(http::clientIp(req));
    data["created_at"] = JsonValue(util::nowIso());
    data["last_seen_at"] = jsonx::num(now);
    data["expires_at"] = jsonx::num(now + static_cast<long long>(cfg.sessionHours) * 3600);
    if (!deviceHash.empty()) data["device_hash"] = JsonValue(deviceHash);
    Cache::setJson("session:" + id, jsonx::dump(data), cfg.sessionHours * 3600);
    return id;
}

std::optional<std::string> sessionId(const Request& req) {
    std::string id = http::cookie(req, "unisso_session");
    if (id.empty()) return std::nullopt;
    return id;
}

std::optional<User> currentUser(const Request& req) {
    auto id = sessionId(req);
    if (!id) return std::nullopt;
    std::string raw = Cache::get("session:" + *id);
    if (raw.empty()) return std::nullopt;
    std::map<std::string, JsonValue> data;
    if (!jsonx::parseObject(raw, data)) return std::nullopt;
    std::string userId = jsonx::getString(data, "user_id");
    if (userId.empty()) return std::nullopt;

    // Backfill metadata for sessions created before device listing existed,
    // and refresh last_seen at most every five minutes.
    long long now = util::nowEpoch();
    bool dirty = false;
    if (!data.count("user_agent")) {
        std::string ua = http::header(req, "User-Agent");
        if (ua.size() > 255) ua.resize(255);
        data["user_agent"] = JsonValue(ua);
        data["ip"] = JsonValue(http::clientIp(req));
        if (!data.count("last_seen_at")) data["last_seen_at"] = jsonx::num(now);
        dirty = true;
    }
    long long lastSeen = 0;
    auto seenIt = data.find("last_seen_at");
    if (seenIt != data.end() && seenIt->second.getType() == JsonValue::NUMBER) {
        lastSeen = static_cast<long long>(seenIt->second.asNumber());
    }
    if (lastSeen <= 0 || now - lastSeen >= 300) {
        data["last_seen_at"] = jsonx::num(now);
        dirty = true;
    }
    if (dirty) {
        long long ttl = Cache::ttl("session:" + *id);
        if (ttl <= 0) ttl = AppConfig::get().sessionHours * 3600;
        Cache::setJson("session:" + *id, jsonx::dump(data), static_cast<int>(ttl));
    }

    auto user = repo::findUserById(userId);
    if (!user || !user->isActive) return std::nullopt;
    return user;
}

bool requireUser(const Request& req, Response& res, User& out) {
    auto user = currentUser(req);
    if (!user) {
        http::error(res, 401, "authentication_required", "请先登录");
        return false;
    }
    out = *user;
    return true;
}

bool requireAdmin(const Request& req, Response& res, User& out) {
    if (!requireUser(req, res, out)) return false;
    if (!out.isAdmin) {
        http::error(res, 403, "admin_required", "需要管理员权限");
        return false;
    }
    return true;
}

bool requireSuperAdmin(const Request& req, Response& res, User& out) {
    if (!requireUser(req, res, out)) return false;
    if (!out.isSuperAdmin) {
        http::error(res, 403, "super_admin_required", "需要超级管理员权限");
        return false;
    }
    return true;
}

std::string deviceHash(const std::string& token) {
    return util::hex(util::sha256Raw(token));
}

bool hasActiveSession(const std::string& userId) {
    for (const auto& name : Cache::scan("session:*", 500)) {
        std::string raw = Cache::get(name);
        if (raw.empty()) continue;
        std::map<std::string, JsonValue> data;
        if (!jsonx::parseObject(raw, data)) continue;
        if (jsonx::getString(data, "user_id") == userId) return true;
    }
    return false;
}

void cancelDeviceRequests(const std::string& stepUpToken) {
    if (stepUpToken.empty()) return;
    auto& cfg = AppConfig::get();
    std::string key = cfg.secretKey.empty() ? cfg.signingKeyId : cfg.secretKey;
    std::string hash = util::hex(util::hmacSha256Raw(key, stepUpToken));
    Db::exec("UPDATE step_up_device_requests SET status='canceled', decided_at=NOW() "
             "WHERE challenge_hash=" + Db::quote(hash) + " AND status='pending'");
}

bool deviceTrusted(const std::string& userId, const Request& req) {
    std::string raw = http::cookie(req, "unisso_device");
    if (raw.empty()) return false;
    auto rows = Db::query(
        "SELECT id, ip, known_ips FROM trusted_devices WHERE user_id=" + Db::quote(userId) +
        " AND token_hash=" + Db::quote(deviceHash(raw)) +
        " AND last_seen_at >= NOW() - INTERVAL " +
        std::to_string(AppConfig::get().trustDeviceDays) + " DAY LIMIT 1");
    if (rows.empty()) return false;
    const auto& row = rows.front();
    // The cookie proves which browser this is, the IP proves where it is.
    // A trusted browser showing up at an address we have never seen it at
    // counts as a new device and gets the second factor again.
    if (!ipSeenForDevice(row.count("ip") ? row.at("ip") : "",
                         row.count("known_ips") ? row.at("known_ips") : "",
                         http::clientIp(req))) {
        return false;
    }
    Db::exec("UPDATE trusted_devices SET last_seen_at=NOW() WHERE id=" +
             Db::quote(row.at("id")));
    return true;
}

void noteDeviceIp(const std::string& userId, const Request& req) {
    std::string raw = http::cookie(req, "unisso_device");
    if (raw.empty()) return;
    auto rows = Db::query(
        "SELECT id, known_ips FROM trusted_devices WHERE user_id=" + Db::quote(userId) +
        " AND token_hash=" + Db::quote(deviceHash(raw)) +
        " AND last_seen_at >= NOW() - INTERVAL " +
        std::to_string(AppConfig::get().trustDeviceDays) + " DAY LIMIT 1");
    if (rows.empty()) return;
    // This login was a second-factor proof in its own right (email code):
    // refresh the address book of an already-trusted device so the user does
    // not have to re-verify just because their campus IP rotated.
    std::string ip = http::clientIp(req);
    std::string known = addKnownIp(
        rows.front().count("known_ips") ? rows.front().at("known_ips") : "", ip);
    Db::exec("UPDATE trusted_devices SET ip=" + Db::nullable(ip) +
             ", known_ips=" + Db::nullable(known) + ", last_seen_at=NOW() WHERE id=" +
             Db::quote(rows.front().at("id")));
}

std::string trustDevice(const std::string& userId, const Request& req, Response& res) {
    std::string raw = http::cookie(req, "unisso_device");
    std::string ua = http::header(req, "User-Agent");
    if (ua.size() > 255) ua.resize(255);
    std::string ip = http::clientIp(req);
    auto link = [&](const std::string& token) -> std::string {
        std::string hash = deviceHash(token);
        auto existing = Db::query(
            "SELECT id, known_ips FROM trusted_devices WHERE user_id=" + Db::quote(userId) +
            " AND token_hash=" + Db::quote(hash) + " LIMIT 1");
        if (!existing.empty()) {
            std::string known = addKnownIp(
                existing.front().count("known_ips") ? existing.front().at("known_ips") : "", ip);
            Db::exec("UPDATE trusted_devices SET last_seen_at=NOW(), user_agent=" +
                     Db::nullable(ua) + ", ip=" + Db::nullable(ip) + ", known_ips=" +
                     Db::nullable(known) + " WHERE id=" + Db::quote(existing.front().at("id")));
            return hash;
        }
        Db::exec("INSERT INTO trusted_devices (id,user_id,token_hash,user_agent,ip,known_ips,"
                 "created_at,last_seen_at) VALUES (" + Db::quote(util::uuid4()) + "," +
                 Db::quote(userId) + "," + Db::quote(hash) + "," + Db::nullable(ua) + "," +
                 Db::nullable(ip) + "," + Db::nullable(addKnownIp("", ip)) + ",NOW(),NOW())");
        return hash;
    };
    if (!raw.empty()) {
        // Only reuse a cookie that is not already bound to another account.
        auto other = Db::query("SELECT user_id FROM trusted_devices WHERE token_hash=" +
                               Db::quote(deviceHash(raw)) + " LIMIT 1");
        if (other.empty() || other.front().at("user_id") == userId) {
            return link(raw);
        }
    }
    std::string token = util::randomToken(32);
    std::string hash = link(token);
    http::setCookie(res, req, "unisso_device", token,
                    AppConfig::get().trustDeviceDays * 24 * 3600);
    return hash;
}

void untrustDevice(const std::string& userId, const Request& req) {
    std::string raw = http::cookie(req, "unisso_device");
    if (raw.empty()) return;
    Db::exec("DELETE FROM trusted_devices WHERE user_id=" + Db::quote(userId) +
             " AND token_hash=" + Db::quote(deviceHash(raw)));
}

void setSessionCookie(Response& res, const Request& req, const std::string& session) {
    auto& cfg = AppConfig::get();
    http::setCookie(res, req, "unisso_session", session, cfg.sessionHours * 3600, true);
}

void destroySession(const Request& req, Response& res) {
    auto id = sessionId(req);
    if (id) {
        Cache::del("session:" + *id);
        Cache::del("browser_csrf:session:" + *id);
    }
    http::clearCookie(res, req, "unisso_session");
}

std::string issueCsrf(const Request& req, Response& res) {
    std::string context;
    auto sid = sessionId(req);
    if (sid && Cache::exists("session:" + *sid)) {
        context = "session:" + *sid;
    } else {
        std::string anon = http::cookie(req, "unisso_csrf_context");
        if (anon.size() != 43) {
            anon = util::randomToken(32);
            http::setCookie(res, req, "unisso_csrf_context", anon, 3600, true);
        }
        context = "anonymous:" + anon;
    }
    std::string key = "browser_csrf:" + context;
    std::string token = util::randomToken(32);
    Cache::setnx(key, token, 3600);
    std::string stored = Cache::get(key);
    return stored.empty() ? token : stored;
}

bool validateCsrf(const Request& req, Response& res) {
    std::string method = req.method;
    if (method == "GET" || method == "HEAD" || method == "OPTIONS") return true;
    if (req.path == "/api/oauth/token" || req.path == "/api/oauth/introspect" ||
        req.path == "/api/logs/client") return true;

    auto& cfg = AppConfig::get();
    std::vector<std::string> allowed;
    allowed.push_back(http::originOf(cfg.issuer));
    for (const auto& item : configStringList(cfg.csrfOriginsJson)) {
        if (std::find(allowed.begin(), allowed.end(), item) == allowed.end()) {
            allowed.push_back(item);
        }
    }
    std::string origin = http::origin(req);
    if (!origin.empty() && std::find(allowed.begin(), allowed.end(), origin) == allowed.end()) {
        http::error(res, 403, "csrf_origin_rejected", "请求来源不被允许");
        return false;
    }
    if (util::lower(http::header(req, "Sec-Fetch-Site")) == "cross-site") {
        http::error(res, 403, "csrf_origin_rejected", "跨站请求被拒绝");
        return false;
    }

    std::string token = http::header(req, "X-CSRF-Token");
    if (token.empty()) token = http::input(req, "csrf_token");
    std::string sid = "";
    auto maybeSid = sessionId(req);
    if (maybeSid && Cache::exists("session:" + *maybeSid)) {
        sid = "session:" + *maybeSid;
    } else {
        std::string anon = http::cookie(req, "unisso_csrf_context");
        if (!anon.empty()) sid = "anonymous:" + anon;
    }
    std::string expected;
    if (!sid.empty()) expected = Cache::get("browser_csrf:" + sid);
    if (expected.empty() || token.empty() || !util::constantTimeEquals(expected, token)) {
        http::error(res, 403, "csrf_validation_failed", "安全校验失败，请刷新后重试");
        return false;
    }
    return true;
}

bool allowHost(const Request& req) {
    auto& cfg = AppConfig::get();
    if (cfg.allowedHostsJson.empty()) return true;
    std::string host = http::header(req, "Host");
    std::string bare = host;
    size_t colon = bare.find(':');
    if (colon != std::string::npos) bare = bare.substr(0, colon);
    if (bare == "localhost" || bare == "127.0.0.1" || bare == "[::1]" || bare == "::1") return true;
    // Allow raw IPs for platform-internal health checks.
    if (!bare.empty() && (std::isdigit(static_cast<unsigned char>(bare[0])) || bare.find(':') != std::string::npos)) return true;
    for (const auto& item : configStringList(cfg.allowedHostsJson)) {
        if (item == bare) return true;
    }
    return false;
}

}  // namespace authx

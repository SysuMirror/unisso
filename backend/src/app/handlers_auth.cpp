#include "core.h"

#include <algorithm>
#include <filesystem>
#include <regex>

namespace {

std::string safeNextUrl(const std::string& value) {
    std::string next = util::trim(value);
    if (next.empty() || next[0] != '/') return "/";
    if (next.size() > 1 && next[1] == '/') return "/";
    if (next.find('\\') != std::string::npos) return "/";
    if (next.find('\r') != std::string::npos || next.find('\n') != std::string::npos) return "/";
    return next;
}

bool validEmail(const std::string& email) {
    static const std::regex pattern(R"(^[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}$)");
    return std::regex_match(email, pattern) && email.size() <= 128;
}

std::string loginAttemptKey(const std::string& ip, const std::string& email) {
    return "login_attempts:" + ip + ":" + (email.empty() ? "-" : email);
}

bool deliverableEmail(const std::string& email) {
    size_t at = email.find('@');
    return at != std::string::npos && email.rfind("@sse.local") == std::string::npos;
}

bool needsStepUp(const User& user, int days, bool trustedDevice) {
    if (!user.stepUpEnabled) return false;
    // A new browser/device is a risk signal on its own.
    if (!trustedDevice) return true;
    if (days <= 0) return false;
    auto rows = Db::query(
        "SELECT TIMESTAMPDIFF(SECOND,last_login_at,NOW()) AS age FROM users "
        "WHERE id=" + Db::quote(user.id) + " AND last_login_at IS NOT NULL LIMIT 1");
    if (rows.empty()) return false;
    long long age = 0;
    try { age = std::stoll(rows.front().at("age")); } catch (...) { return false; }
    return age >= static_cast<long long>(days) * 86400;
}

bool isLocked(const std::string& email, const std::string& ip, long long& lockedUntil) {
    std::string raw = Cache::get(loginAttemptKey(ip, email));
    if (raw.empty()) return false;
    std::map<std::string, JsonValue> data;
    if (!jsonx::parseObject(raw, data)) return false;
    auto it = data.find("locked_until");
    if (it == data.end() || it->second.getType() != JsonValue::NUMBER) return false;
    lockedUntil = static_cast<long long>(it->second.asNumber());
    return lockedUntil > util::nowEpoch();
}

void recordFailure(const std::string& email, const std::string& ip) {
    std::string key = loginAttemptKey(ip, email);
    std::string raw = Cache::get(key);
    std::map<std::string, JsonValue> data;
    if (!jsonx::parseObject(raw, data)) data.clear();
    long long count = 0;
    auto it = data.find("count");
    if (it != data.end() && it->second.getType() == JsonValue::NUMBER) {
        count = static_cast<long long>(it->second.asNumber());
    }
    ++count;
    data["count"] = jsonx::num(count);
    if (count >= 5) data["locked_until"] = jsonx::num(util::nowEpoch() + 900);
    Cache::setJson(key, jsonx::dump(data), 900);
}

std::string avatarExtension(const std::string& contentType, const std::string& data,
                            bool& valid) {
    valid = false;
    if (contentType == "image/jpeg" && data.size() > 3 &&
        static_cast<unsigned char>(data[0]) == 0xff &&
        static_cast<unsigned char>(data[1]) == 0xd8 &&
        static_cast<unsigned char>(data[2]) == 0xff) {
        valid = true;
        return ".jpg";
    }
    if (contentType == "image/png" && data.size() > 8 &&
        data.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0) {
        valid = true;
        return ".png";
    }
    if (contentType == "image/gif" && data.size() > 6 &&
        (data.compare(0, 6, "GIF87a") == 0 || data.compare(0, 6, "GIF89a") == 0)) {
        valid = true;
        return ".gif";
    }
    if (contentType == "image/webp" && data.size() > 12 &&
        data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0) {
        valid = true;
        return ".webp";
    }
    return "";
}

}  // namespace

namespace handlers {

void csrfToken(const Request& req, Response& res) {
    std::string token = authx::issueCsrf(req, res);
    std::map<std::string, JsonValue> obj;
    obj["csrf_token"] = JsonValue(token);
    http::json(res, 200, JsonValue(obj));
}

void authLogin(const Request& req, Response& res) {
    std::string email = util::lower(util::trim(http::input(req, "email")));
    std::string password = http::input(req, "password");
    std::string next = safeNextUrl(http::input(req, "next"));
    std::string ip = http::clientIp(req);

    long long lockedUntil = 0;
    if (isLocked(email, ip, lockedUntil)) {
        http::error(res, 429, "too_many_attempts", "登录尝试次数过多，请 15 分钟后再试");
        return;
    }
    if (email.empty() || password.empty()) {
        http::error(res, 400, "invalid_credentials", "邮箱或密码错误");
        return;
    }

    auto user = repo::findUserByEmail(email);
    bool authenticated = user && user->isActive && password::verify(password, user->passwordHash);
    bool locallyAuthenticated = authenticated;

    // If the account is not known locally (or the local password does not
    // match), ask SSE. A successful SSE login means the user is 院内.
    if (!authenticated && AppConfig::get().sseEnabled) {
        sse::Profile profile;
        std::string sseError;
        if (sse::login(email, password, profile, sseError)) {
            auto sseUser = sse::upsertUser(profile, password);
            if (sseUser && sseUser->isActive) {
                user = sseUser;
                authenticated = true;
            }
        } else if (!sseError.empty()) {
            std::cerr << "[sse] login failed for " << email << ": " << sseError << std::endl;
        }
    }

    if (!authenticated) {
        recordFailure(email, ip);
        http::error(res, 401, "invalid_credentials", "邮箱或密码错误");
        return;
    }

    // New devices and long-dormant accounts must pass a second factor before
    // a session is created. The password step is valid, but no session cookie
    // is issued yet.
    auto& cfg = AppConfig::get();
    bool hasFace = user && cfg.faceEnabled && repo::faceCount(user->id) > 0;
    bool canEmail = user && cfg.emailAvailable() && deliverableEmail(user->email);
    bool hasDevice = user && authx::hasActiveSession(user->id);
    bool stepUpPossible = hasFace || canEmail || hasDevice;
    bool trustedDevice = user && authx::deviceTrusted(user->id, req);
    if (user && stepUpPossible && needsStepUp(*user, cfg.stepUpDays, trustedDevice)) {
        Cache::del(loginAttemptKey(ip, email));
        beginStepUp(req, res, *user, next);
        return;
    }
    std::string deviceHash;
    if (user) deviceHash = authx::trustDevice(user->id, req, res);

    // One-time sync of the SSE avatar for existing internal accounts. Once an
    // avatar exists (SSE or uploaded), no cross-service call is made.
    if (locallyAuthenticated && user && user->isInternal && user->avatar.empty() &&
        AppConfig::get().sseEnabled) {
        sse::Profile profile;
        std::string sseError;
        if (sse::login(email, password, profile, sseError) && !profile.avatar.empty()) {
            Db::exec("UPDATE users SET avatar=" + Db::quote(profile.avatar) +
                     " WHERE id=" + Db::quote(user->id));
            user->avatar = profile.avatar;
        }
    }

    // A Redis-cached SSE verification lets an already-local account keep its
    // internal marker without another cross-service call.
    if (user && !user->isInternal) {
        std::string cached = Cache::get("sse_login:" + email);
        if (!cached.empty()) {
            Db::exec("UPDATE users SET is_internal=1 WHERE id=" + Db::quote(user->id));
            user->isInternal = true;
        }
    }

    Cache::del(loginAttemptKey(ip, email));
    repo::updateUserLastLogin(user->id);
    std::string session = authx::createSession(
        user->id, user->username.empty() ? email : user->username, req, deviceHash);
    authx::setSessionCookie(res, req, session);

    auto fresh = repo::findUserById(user->id);
    std::map<std::string, JsonValue> obj;
    obj["user"] = repo::userJson(fresh ? *fresh : *user, true);
    obj["next"] = JsonValue(next);
    http::ok(res, JsonValue(obj));
}

void authLogout(const Request& req, Response& res) {
    authx::destroySession(req, res);
    http::ok(res);
}

void authMe(const Request& req, Response& res) {
    auto user = authx::currentUser(req);
    if (!user) {
        http::error(res, 401, "authentication_required", "未登录");
        return;
    }
    std::map<std::string, JsonValue> obj;
    obj["user"] = repo::userJson(*user, true);
    http::json(res, 200, JsonValue(obj));
}

void authSession(const Request& req, Response& res) {
    auto user = authx::currentUser(req);
    std::map<std::string, JsonValue> obj;
    obj["user"] = user ? repo::userJson(*user, true) : JsonValue();
    http::json(res, 200, JsonValue(obj));
}

void authRegister(const Request& req, Response& res) {
    auto& cfg = AppConfig::get();
    if (!cfg.emailAvailable()) {
        http::error(res, 403, "registration_unavailable",
                    "邮箱验证服务尚未配置，暂不开放自助注册");
        return;
    }
    std::string email = util::lower(util::trim(http::input(req, "email")));
    std::string password = http::input(req, "password");
    std::string username = util::trim(http::input(req, "username"));
    std::string fullName = util::trim(http::input(req, "full_name"));
    if (!validEmail(email)) {
        http::error(res, 400, "invalid_input", "请填写有效的邮箱地址");
        return;
    }
    if (!username.empty() && username.size() > 64) {
        http::error(res, 400, "invalid_input", "昵称不能超过 64 个字符");
        return;
    }
    if (fullName.size() > 128) {
        http::error(res, 400, "invalid_input", "姓名不能超过 128 个字符");
        return;
    }
    try {
        password::validateStrength(password);
    } catch (const std::exception& e) {
        http::error(res, 400, "invalid_input", e.what());
        return;
    }

    std::string inviteCode = util::trim(http::input(req, "invite_code"));
    std::string errorCode;
    std::string errorMessage;
    bool sseCodeRequired = false;
    if (!registration::start(email, password, username, fullName, http::clientIp(req),
                             inviteCode, sseCodeRequired, errorCode, errorMessage)) {
        int status = errorCode == "rate_limited" ? 429
                     : errorCode == "sse_email_failed" ? 400
                     : errorCode == "email_unavailable" ? 503 : 400;
        std::string message = !errorMessage.empty() ? errorMessage
                              : errorCode == "rate_limited" ? "操作过于频繁，请稍后再试"
                              : errorCode == "sse_email_failed" ? "集市验证码发送失败"
                                                               : "邮件发送失败，请稍后重试";
        http::error(res, status, errorCode.empty() ? "email_unavailable" : errorCode, message);
        return;
    }
    std::map<std::string, JsonValue> obj;
    obj["pending_email"] = JsonValue(email);
    obj["sse_code_required"] = JsonValue(sseCodeRequired);
    http::ok(res, JsonValue(obj));
}

void authVerify(const Request& req, Response& res) {
    std::string email = util::lower(util::trim(http::input(req, "email")));
    std::string code = util::trim(http::input(req, "code"));
    std::string sseCode = util::trim(http::input(req, "sse_code"));
    std::string password = http::input(req, "password");
    std::optional<User> created;
    std::string errorCode;
    std::string errorMessage;
    if (!registration::verify(email, code, sseCode, password, created, errorCode, errorMessage)) {
        int status = errorCode == "rate_limited" ? 429
                     : errorCode == "conflict" ? 409
                     : errorCode == "sse_code_required" ? 400
                     : errorCode == "sse_register_failed" ? 400
                                                          : 400;
        std::string message = !errorMessage.empty() ? errorMessage
                              : errorCode == "rate_limited" ? "操作过于频繁，请稍后再试"
                              : errorCode == "conflict"  ? "该邮箱已注册，请直接登录"
                                                          : "验证码无效或已过期";
        http::error(res, status, errorCode.empty() ? "invalid_code" : errorCode, message);
        return;
    }
    std::string session = authx::createSession(
        created->id, created->username.empty() ? created->email : created->username, req);
    authx::setSessionCookie(res, req, session);
    std::map<std::string, JsonValue> obj;
    obj["user"] = repo::userJson(*created, true);
    http::ok(res, JsonValue(obj));
}

void authResend(const Request& req, Response& res) {
    std::string email = util::lower(util::trim(http::input(req, "email")));
    std::string errorCode;
    if (!registration::resend(email, http::clientIp(req), errorCode)) {
        http::error(res, errorCode == "rate_limited" ? 429 : 503,
                    errorCode.empty() ? "email_unavailable" : errorCode,
                    "验证码发送失败，请稍后重试");
        return;
    }
    http::ok(res);
}

void avatarGet(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::map<std::string, JsonValue> obj;
    obj["url"] = JsonValue(user.avatar);
    http::ok(res, JsonValue(obj));
}

void avatarUpload(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    const UploadedFile* file = req.file("file");
    if (!file) {
        http::error(res, 400, "invalid_file", "缺少文件");
        return;
    }
    if (file->data.size() > 5 * 1024 * 1024) {
        http::error(res, 413, "file_too_large", "头像不能超过 5MB");
        return;
    }
    bool valid = false;
    std::string ext = avatarExtension(file->contentType, file->data, valid);
    if (!valid) {
        http::error(res, 400, "invalid_file", "只支持 JPEG、PNG、GIF、WebP 图片");
        return;
    }
    auto& cfg = AppConfig::get();
    std::string dir = util::pathJoin(cfg.dataRoot, "uploads/avatars");
    if (!util::ensureDir(dir)) {
        http::error(res, 500, "storage_unavailable", "无法保存头像");
        return;
    }
    std::string fileName = user.id + ext;
    std::string path = util::pathJoin(dir, fileName);
    if (!util::writeFile(path, file->data)) {
        http::error(res, 500, "storage_unavailable", "无法保存头像");
        return;
    }
    std::string url = "/uploads/avatars/" + fileName;
    Db::exec("UPDATE users SET avatar=" + Db::quote(url) + ", updated_at=NOW() WHERE id=" +
             Db::quote(user.id));
    std::map<std::string, JsonValue> obj;
    obj["url"] = JsonValue(url);
    http::ok(res, JsonValue(obj));
}

void identitiesList(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::vector<JsonValue> items;
    for (const auto& identity : repo::listIdentities(user.id)) {
        items.push_back(repo::identityJson(identity));
    }
    http::json(res, 200, JsonValue(std::map<std::string, JsonValue>{
                                 {"ok", JsonValue(true)}, {"identities", JsonValue(items)}}));
}

void identitiesBind(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string provider = util::trim(http::input(req, "provider"));
    std::string providerUserId = util::trim(http::input(req, "provider_user_id"));
    if (provider.empty() || providerUserId.empty()) {
        http::error(res, 400, "invalid_input", "请填写平台和外部用户 ID");
        return;
    }
    Identity identity;
    identity.id = util::uuid4();
    identity.userId = user.id;
    identity.provider = provider;
    identity.providerUserId = providerUserId;
    identity.providerUsername = util::trim(http::input(req, "provider_username"));
    identity.providerEmail = util::trim(http::input(req, "provider_email"));
    if (!repo::bindIdentity(identity)) {
        http::error(res, 500, "bind_failed", "绑定失败");
        return;
    }
    http::ok(res);
}

void identitiesUnbind(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string id = http::param(req, "id");
    if (!repo::unbindIdentity(user.id, id)) {
        http::error(res, 500, "unbind_failed", "解绑失败");
        return;
    }
    http::ok(res);
}

}  // namespace handlers

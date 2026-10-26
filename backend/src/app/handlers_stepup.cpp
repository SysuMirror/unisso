#include "core.h"
#include "app/logger.h"

#include <atomic>
#include <random>

namespace {

constexpr int kChallengeSeconds = 600;

std::string challengeKey(const std::string& token) {
    return "step_up:" + token;
}

std::string codeKey(const std::string& token) {
    return "step_up_code:" + token;
}

std::string codeHash(const std::string& code) {
    auto& cfg = AppConfig::get();
    std::string key = cfg.secretKey.empty() ? cfg.signingKeyId : cfg.secretKey;
    return util::hex(util::hmacSha256Raw(key, code));
}

std::string challengeHash(const std::string& token) {
    auto& cfg = AppConfig::get();
    std::string key = cfg.secretKey.empty() ? cfg.signingKeyId : cfg.secretKey;
    return util::hex(util::hmacSha256Raw(key, token));
}

std::string randomCode() {
    static std::atomic<unsigned int> counter{0};
    std::random_device rd;
    unsigned int value = rd() + counter.fetch_add(1);
    return std::to_string(100000 + (value % 900000));
}

bool truthy(const std::string& value) {
    std::string lowered = util::lower(util::trim(value));
    return lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on";
}

std::string maskEmail(const std::string& email) {
    size_t at = email.find('@');
    if (at == std::string::npos) return "***";
    if (at == 0) return "***" + email.substr(at);
    return email.substr(0, 1) + "***" + email.substr(at);
}

struct Challenge {
    std::string token;
    std::string userId;
    std::string email;
    std::string next;
    long long expiresAt = 0;
};

bool loadChallenge(const Request& req, Response& res, Challenge& out) {
    std::string token = util::trim(http::input(req, "token"));
    std::string cookie = http::cookie(req, "unisso_stepup");
    if (token.empty()) token = cookie;
    if (token.empty() || cookie.empty() || token != cookie) {
        http::error(res, 403, "step_up_required", "二次验证会话已失效，请重新登录");
        return false;
    }
    std::string raw = Cache::get(challengeKey(token));
    if (raw.empty()) {
        http::error(res, 401, "step_up_expired", "二次验证已过期，请重新登录");
        return false;
    }
    std::map<std::string, JsonValue> data;
    if (!jsonx::parseObject(raw, data)) {
        http::error(res, 401, "step_up_expired", "二次验证已过期，请重新登录");
        return false;
    }
    out.token = token;
    out.userId = jsonx::getString(data, "user_id");
    out.email = jsonx::getString(data, "email");
    out.next = jsonx::getString(data, "next", "/");
    auto exp = data.find("expires_at");
    if (exp != data.end() && exp->second.getType() == JsonValue::NUMBER) {
        out.expiresAt = static_cast<long long>(exp->second.asNumber());
    }
    if (out.userId.empty() || out.expiresAt <= util::nowEpoch()) {
        Cache::del(challengeKey(token));
        http::error(res, 401, "step_up_expired", "二次验证已过期，请重新登录");
        return false;
    }
    return true;
}

}  // namespace

namespace handlers {

void beginStepUp(const Request& req, Response& res, const User& user,
                 const std::string& next) {
    auto& cfg = AppConfig::get();
    bool emailMethod = cfg.emailAvailable() && !user.email.empty() &&
                       user.email.rfind("@sse.local") == std::string::npos;
    bool faceMethod = cfg.faceEnabled && repo::faceCount(user.id) > 0;
    bool deviceMethod = authx::hasActiveSession(user.id);
    std::vector<std::string> methods;
    if (emailMethod) methods.push_back("email");
    if (faceMethod) methods.push_back("face");
    if (deviceMethod) methods.push_back("device");
    if (methods.empty()) {
        http::error(res, 503, "step_up_unavailable", "二次验证暂不可用，请稍后重试");
        return;
    }
    std::string token = util::randomToken(24);
    std::map<std::string, JsonValue> data;
    data["user_id"] = JsonValue(user.id);
    data["email"] = JsonValue(user.email);
    data["next"] = JsonValue(next.empty() ? "/" : next);
    data["created_at"] = jsonx::num(util::nowEpoch());
    data["expires_at"] = jsonx::num(util::nowEpoch() + kChallengeSeconds);
    Cache::setJson(challengeKey(token), jsonx::dump(data), kChallengeSeconds);
    http::setCookie(res, req, "unisso_stepup", token, kChallengeSeconds);

    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["step_up_required"] = JsonValue(true);
    obj["step_up_token"] = JsonValue(token);
    obj["methods"] = JsonValue(methods);
    obj["email_hint"] = JsonValue(emailMethod ? maskEmail(user.email) : "");
    obj["trust_days"] = jsonx::num(cfg.trustDeviceDays);
    obj["next"] = JsonValue(next.empty() ? "/" : next);
    obj["expires_in"] = jsonx::num(kChallengeSeconds);
    http::json(res, 200, JsonValue(obj));
}

void stepUpMethods(const Request& req, Response& res) {
    Challenge challenge;
    if (!loadChallenge(req, res, challenge)) return;
    auto user = repo::findUserById(challenge.userId);
    if (!user || !user->isActive) {
        http::error(res, 401, "invalid_account", "账号不可用，请重新登录");
        return;
    }
    auto& cfg = AppConfig::get();
    bool emailMethod = cfg.emailAvailable() && !user->email.empty() &&
                       user->email.rfind("@sse.local") == std::string::npos;
    bool faceMethod = cfg.faceEnabled && repo::faceCount(user->id) > 0;
    bool deviceMethod = authx::hasActiveSession(user->id);
    std::vector<std::string> methods;
    if (emailMethod) methods.push_back("email");
    if (faceMethod) methods.push_back("face");
    if (deviceMethod) methods.push_back("device");
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["methods"] = JsonValue(methods);
    obj["email_hint"] = JsonValue(emailMethod ? maskEmail(user->email) : "");
    obj["trust_days"] = jsonx::num(cfg.trustDeviceDays);
    http::json(res, 200, JsonValue(obj));
}

void stepUpDeviceStart(const Request& req, Response& res) {
    Challenge challenge;
    if (!loadChallenge(req, res, challenge)) return;
    auto user = repo::findUserById(challenge.userId);
    if (!user || !user->isActive) {
        http::error(res, 401, "invalid_account", "账号不可用，请重新登录");
        return;
    }
    if (!authx::hasActiveSession(challenge.userId)) {
        http::error(res, 409, "no_device", "当前没有其他已登录设备可以确认");
        return;
    }
    long long starts = Cache::incr("step_up_device_start:" + challenge.token, kChallengeSeconds);
    if (starts > 10) {
        http::error(res, 429, "rate_limited", "请求过于频繁，请稍后再试");
        return;
    }
    std::string hash = challengeHash(challenge.token);
    auto existing = Db::query(
        "SELECT id,expires_at FROM step_up_device_requests WHERE challenge_hash=" +
        Db::quote(hash) + " AND user_id=" + Db::quote(challenge.userId) +
        " AND status='pending' AND expires_at>NOW() ORDER BY created_at DESC LIMIT 1");
    if (!existing.empty()) {
        std::map<std::string, JsonValue> obj;
        obj["ok"] = JsonValue(true);
        obj["approval_id"] = JsonValue(existing.front().at("id"));
        obj["status"] = JsonValue("pending");
        http::json(res, 200, JsonValue(obj));
        return;
    }
    std::string id = util::randomToken(24);
    std::string ua = http::header(req, "User-Agent");
    if (ua.size() > 255) ua.resize(255);
    if (Db::exec("INSERT INTO step_up_device_requests "
                 "(id,user_id,challenge_hash,status,requester_ua,requester_ip,created_at,expires_at) "
                 "VALUES (" + Db::quote(id) + "," + Db::quote(challenge.userId) + "," +
                 Db::quote(hash) + ",'pending'," + Db::nullable(ua) + "," +
                 Db::nullable(http::clientIp(req)) + ",NOW(),FROM_UNIXTIME(" +
                 std::to_string(challenge.expiresAt) + "))") < 0) {
        http::error(res, 500, "create_failed", "无法创建设备确认请求");
        return;
    }
    applog::access("[stepup] device request created user=" + challenge.userId +
                   " approval=" + id.substr(0, 10));
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["approval_id"] = JsonValue(id);
    obj["status"] = JsonValue("pending");
    http::json(res, 200, JsonValue(obj));
}

void stepUpDeviceStatus(const Request& req, Response& res) {
    Challenge challenge;
    if (!loadChallenge(req, res, challenge)) return;
    std::string hash = challengeHash(challenge.token);
    auto rows = Db::query(
        "SELECT id,status,expires_at,UNIX_TIMESTAMP(expires_at) AS exp FROM step_up_device_requests "
        "WHERE challenge_hash=" + Db::quote(hash) + " AND user_id=" +
        Db::quote(challenge.userId) + " ORDER BY created_at DESC LIMIT 1");
    if (rows.empty()) {
        std::map<std::string, JsonValue> obj;
        obj["ok"] = JsonValue(true);
        obj["status"] = JsonValue("none");
        http::json(res, 200, JsonValue(obj));
        return;
    }
    std::string id = rows.front().at("id");
    std::string status = rows.front().at("status");
    long long exp = 0;
    try { exp = std::stoll(rows.front().at("exp")); } catch (...) {}
    if (status == "pending" && exp <= util::nowEpoch()) {
        Db::exec("UPDATE step_up_device_requests SET status='expired' WHERE id=" +
                 Db::quote(id) + " AND status='pending'");
        status = "expired";
    }
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["status"] = JsonValue(status);
    if (status == "pending") {
        obj["approval_id"] = JsonValue(id);
        http::json(res, 200, JsonValue(obj));
        return;
    }
    if (status == "denied" || status == "expired" || status == "canceled") {
        http::json(res, 200, JsonValue(obj));
        return;
    }
    if (status == "approved") {
        // Only one poll may turn the approval into a real session.
        int claimed = Db::exec(
            "UPDATE step_up_device_requests SET status='consumed', decided_at=COALESCE(decided_at,NOW()) "
            "WHERE id=" + Db::quote(id) + " AND status='approved'");
        if (claimed <= 0) {
            obj["status"] = JsonValue("consumed");
            obj["already"] = JsonValue(true);
            http::json(res, 200, JsonValue(obj));
            return;
        }
        auto user = repo::findUserById(challenge.userId);
        if (!user || !user->isActive) {
            http::error(res, 401, "invalid_account", "账号不可用，请重新登录");
            return;
        }
        std::string trustRaw = util::trim(http::input(req, "trust_device"));
        bool trust = !trustRaw.empty() && truthy(trustRaw);
        repo::updateUserLastLogin(user->id);
        std::string deviceHash;
        if (trust) {
            deviceHash = authx::trustDevice(user->id, req, res);
        } else {
            authx::untrustDevice(user->id, req);
        }
        std::string session = authx::createSession(
            user->id, user->username.empty() ? user->email : user->username, req, deviceHash);
        authx::setSessionCookie(res, req, session);
        Cache::del(challengeKey(challenge.token));
        Cache::del(codeKey(challenge.token));
        Cache::del("step_up_attempts:" + challenge.token);
        Cache::del("step_up_send_total:" + challenge.token);
        Cache::del("step_up_send_cooldown:" + challenge.token);
        http::clearCookie(res, req, "unisso_stepup");
        authx::cancelDeviceRequests(challenge.token);
        obj["status"] = JsonValue("approved");
        obj["next"] = JsonValue(challenge.next.empty() ? "/" : challenge.next);
        obj["user"] = repo::userJson(*user, true);
        http::json(res, 200, JsonValue(obj));
        return;
    }
    if (status == "consumed") {
        obj["already"] = JsonValue(true);
    }
    http::json(res, 200, JsonValue(obj));
}

void stepUpDevicePending(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto rows = Db::query(
        "SELECT id,requester_ua,requester_ip,created_at,UNIX_TIMESTAMP(created_at) AS ts "
        "FROM step_up_device_requests WHERE user_id=" + Db::quote(user.id) +
        " AND status='pending' AND expires_at>NOW() ORDER BY created_at DESC LIMIT 5");
    std::vector<JsonValue> items;
    for (const auto& row : rows) {
        std::map<std::string, JsonValue> obj;
        obj["id"] = JsonValue(row.at("id"));
        obj["user_agent"] = JsonValue(row.count("requester_ua") ? row.at("requester_ua") : "");
        obj["ip"] = JsonValue(row.count("requester_ip") ? row.at("requester_ip") : "");
        long long ts = 0;
        try { ts = std::stoll(row.at("ts")); } catch (...) {}
        obj["created_at"] = JsonValue(ts > 0 ? util::formatTs(ts) : "");
        items.push_back(JsonValue(obj));
    }
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["requests"] = JsonValue(items);
    http::json(res, 200, JsonValue(obj));
}

void stepUpDeviceDecide(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string id = util::trim(http::param(req, "id"));
    std::string approveRaw = util::trim(http::input(req, "approve"));
    if (id.empty() || approveRaw.empty()) {
        http::error(res, 400, "invalid_request", "缺少确认参数");
        return;
    }
    bool approve = truthy(approveRaw);
    std::string ua = http::header(req, "User-Agent");
    if (ua.size() > 255) ua.resize(255);
    int changed = Db::exec(
        "UPDATE step_up_device_requests SET status=" + Db::quote(approve ? "approved" : "denied") +
        ", approver_user_id=" + Db::quote(user.id) + ", approver_ua=" + Db::nullable(ua) +
        ", approver_ip=" + Db::nullable(http::clientIp(req)) +
        ", decided_at=NOW() WHERE id=" + Db::quote(id) + " AND user_id=" +
        Db::quote(user.id) + " AND status='pending' AND expires_at>NOW()");
    if (changed <= 0) {
        http::error(res, 404, "not_found", "该登录请求已处理或已过期");
        return;
    }
    applog::access("[stepup] device request " + std::string(approve ? "approved" : "denied") +
                   " user=" + user.id + " approval=" + id.substr(0, 10));
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["approved"] = JsonValue(approve);
    http::json(res, 200, JsonValue(obj));
}

void stepUpSendEmail(const Request& req, Response& res) {
    Challenge challenge;
    if (!loadChallenge(req, res, challenge)) return;

    auto& cfg = AppConfig::get();
    if (!cfg.emailAvailable() || challenge.email.empty() ||
        challenge.email.rfind("@sse.local") != std::string::npos) {
        http::error(res, 503, "email_unavailable", "邮箱验证暂不可用");
        return;
    }
    long long sends = Cache::incr("step_up_send_total:" + challenge.token, kChallengeSeconds);
    if (sends > 5) {
        http::error(res, 429, "rate_limited", "验证码发送次数过多，请重新登录");
        return;
    }
    long long cooldown = Cache::incr("step_up_send_cooldown:" + challenge.token, 60);
    if (cooldown > 1) {
        http::error(res, 429, "rate_limited", "验证码发送过于频繁，请稍后再试");
        return;
    }

    std::string code = randomCode();
    std::map<std::string, JsonValue> record;
    record["code_hash"] = JsonValue(codeHash(code));
    record["expires_at"] = jsonx::num(util::nowEpoch() + kChallengeSeconds);
    Cache::setJson(codeKey(challenge.token), jsonx::dump(record), kChallengeSeconds);

    std::string html =
        "<div style=\"font-family:-apple-system,Segoe UI,PingFang SC,sans-serif;color:#171717\">"
        "<h2 style=\"font-size:20px;margin:0 0 12px\">UniSSO 二次验证</h2>"
        "<p style=\"color:#5f6368;font-size:14px\">你正在登录一个较长时间未登录的账号，验证码：</p>"
        "<p style=\"font-size:30px;font-weight:700;letter-spacing:6px;margin:16px 0\">" + code + "</p>"
        "<p style=\"color:#5f6368;font-size:13px\">验证码 10 分钟内有效。如果不是你本人操作，请立即修改密码。</p>"
        "</div>";
    std::string mailError;
    if (!mail::send(challenge.email, "UniSSO 二次验证验证码", html, mailError)) {
        Cache::del(codeKey(challenge.token));
        std::cerr << "[step-up] mail failed: " << mailError << std::endl;
        http::error(res, 503, "email_unavailable", "验证码发送失败，请稍后重试");
        return;
    }
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["email_hint"] = JsonValue(maskEmail(challenge.email));
    obj["expires_in"] = jsonx::num(kChallengeSeconds);
    http::ok(res, JsonValue(obj));
}

void stepUpVerify(const Request& req, Response& res) {
    Challenge challenge;
    if (!loadChallenge(req, res, challenge)) return;

    std::string code = util::trim(http::input(req, "code"));
    if (code.size() != 6) {
        http::error(res, 400, "invalid_code", "请输入 6 位验证码");
        return;
    }
    long long attempts = Cache::incr("step_up_attempts:" + challenge.token, 900);
    if (attempts > 5) {
        Cache::del(challengeKey(challenge.token));
        Cache::del(codeKey(challenge.token));
        http::clearCookie(res, req, "unisso_stepup");
        http::error(res, 429, "too_many_attempts", "验证失败次数过多，请重新登录");
        return;
    }

    std::string raw = Cache::get(codeKey(challenge.token));
    std::map<std::string, JsonValue> record;
    if (raw.empty() || !jsonx::parseObject(raw, record)) {
        http::error(res, 401, "invalid_code", "验证码不正确或已过期");
        return;
    }
    long long expiresAt = 0;
    auto exp = record.find("expires_at");
    if (exp != record.end() && exp->second.getType() == JsonValue::NUMBER) {
        expiresAt = static_cast<long long>(exp->second.asNumber());
    }
    std::string expected = jsonx::getString(record, "code_hash");
    if (expiresAt <= util::nowEpoch() || expected.empty() ||
        !util::constantTimeEquals(expected, codeHash(code))) {
        http::error(res, 401, "invalid_code", "验证码不正确或已过期");
        return;
    }

    auto user = repo::findUserById(challenge.userId);
    if (!user || !user->isActive) {
        http::error(res, 401, "invalid_account", "账号不可用");
        return;
    }

    Cache::del(challengeKey(challenge.token));
    Cache::del(codeKey(challenge.token));
    Cache::del("step_up_attempts:" + challenge.token);
    Cache::del("step_up_send_total:" + challenge.token);
    Cache::del("step_up_send_cooldown:" + challenge.token);
    http::clearCookie(res, req, "unisso_stepup");
    authx::cancelDeviceRequests(challenge.token);

    repo::updateUserLastLogin(user->id);
    std::string trustRaw = util::trim(http::input(req, "trust_device"));
    bool trustDevice = trustRaw.empty() ? true : truthy(trustRaw);
    std::string deviceHash;
    if (trustDevice) {
        deviceHash = authx::trustDevice(user->id, req, res);
    } else {
        authx::untrustDevice(user->id, req);
    }
    auto fresh = repo::findUserById(user->id);
    std::string session = authx::createSession(
        user->id, user->username.empty() ? user->email : user->username, req, deviceHash);
    authx::setSessionCookie(res, req, session);

    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["user"] = repo::userJson(fresh ? *fresh : *user, true);
    obj["next"] = JsonValue(challenge.next.empty() ? "/" : challenge.next);
    http::ok(res, JsonValue(obj));
}

void stepUpCancel(const Request& req, Response& res) {
    std::string token = util::trim(http::input(req, "token"));
    if (token.empty()) token = http::cookie(req, "unisso_stepup");
    if (!token.empty()) {
        Cache::del(challengeKey(token));
        Cache::del(codeKey(token));
        Cache::del("step_up_attempts:" + token);
        Cache::del("step_up_send_total:" + token);
        Cache::del("step_up_send_cooldown:" + token);
        authx::cancelDeviceRequests(token);
    }
    http::clearCookie(res, req, "unisso_stepup");
    http::ok(res);
}

}  // namespace handlers

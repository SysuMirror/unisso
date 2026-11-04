#include "core.h"
#include "app/logger.h"

#include <atomic>
#include <random>
#include <regex>

namespace {

constexpr int kCodeSeconds = 600;

std::string normalizeEmail(const std::string& value) {
    return util::lower(util::trim(value));
}

bool validEmail(const std::string& email) {
    static const std::regex pattern(
        R"(^[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}$)");
    return std::regex_match(email, pattern);
}

bool deliverableEmail(const std::string& email) {
    return !email.empty() && email.find("@sse.local") == std::string::npos;
}

std::string digestEmail(const std::string& email) {
    return util::hex(util::sha256Raw(normalizeEmail(email)));
}

std::string randomCode() {
    static std::atomic<unsigned int> counter{0};
    std::random_device rd;
    unsigned int value = rd() + counter.fetch_add(1);
    return std::to_string(100000 + (value % 900000));
}

std::string codeHash(const std::string& code) {
    auto& cfg = AppConfig::get();
    std::string key = cfg.secretKey.empty() ? cfg.signingKeyId : cfg.secretKey;
    return util::hex(util::hmacSha256Raw(key, code));
}

std::string codeKey(const std::string& kind, const std::string& email) {
    return kind + "_code:" + digestEmail(email);
}

bool rateLimited(const std::string& kind, const std::string& email,
                 const std::string& ip, std::string& error) {
    std::string digest = digestEmail(email);
    long long emailCount = Cache::incr(kind + "_send_total:" + digest, kCodeSeconds);
    long long ipCount = Cache::incr(kind + "_send_ip:" + ip, kCodeSeconds);
    if (emailCount > 5 || ipCount > 20) {
        error = "rate_limited";
        return true;
    }
    if (!Cache::get(kind + "_cooldown:" + digest).empty()) {
        error = "cooldown";
        return true;
    }
    return false;
}

bool sendCode(const std::string& email, const std::string& subject,
              const std::string& title, const std::string& code,
              std::string& errorCode) {
    std::string html =
        "<div style=\"font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
        "max-width:520px;margin:0 auto;padding:28px;color:#171717\">"
        "<h2 style=\"font-size:20px;margin:0 0 18px\">" + title + "</h2>"
        "<p style=\"font-size:14px;line-height:1.8\">你的验证码是：</p>"
        "<div style=\"font-size:32px;letter-spacing:8px;font-weight:700;margin:16px 0\">" +
        code + "</div>"
        "<p style=\"font-size:13px;line-height:1.8;color:#666\">验证码 10 分钟内有效。"
        "如果不是你本人操作，请忽略这封邮件。</p></div>";
    std::string error;
    if (!mail::send(email, subject, html, error)) {
        applog::error("[auth] mail send failed: " + error);
        errorCode = "email_unavailable";
        return false;
    }
    return true;
}

void revokeUserSessions(const std::string& userId) {
    for (const auto& name : Cache::scan("session:*", 500)) {
        std::string raw = Cache::get(name);
        if (raw.empty()) continue;
        std::map<std::string, JsonValue> data;
        if (!jsonx::parseObject(raw, data)) continue;
        if (jsonx::getString(data, "user_id") != userId) continue;
        Cache::del(name);
        if (name.rfind("session:", 0) == 0) {
            Cache::del("browser_csrf:session:" + name.substr(8));
        }
    }
}


bool readCodeRecord(const std::string& kind, const std::string& email,
                    const std::string& code, std::string& userId,
                    std::string& errorCode) {
    std::string raw = Cache::get(codeKey(kind, email));
    std::map<std::string, JsonValue> record;
    if (raw.empty() || !jsonx::parseObject(raw, record)) {
        errorCode = "invalid_code";
        return false;
    }
    long long expiresAt = 0;
    auto exp = record.find("expires_at");
    if (exp != record.end() && exp->second.getType() == JsonValue::NUMBER) {
        expiresAt = static_cast<long long>(exp->second.asNumber());
    }
    std::string expected = jsonx::getString(record, "code_hash");
    userId = jsonx::getString(record, "user_id");
    if (expiresAt <= util::nowEpoch() || expected.empty() || userId.empty() ||
        !util::constantTimeEquals(expected, codeHash(code))) {
        errorCode = "invalid_code";
        return false;
    }
    return true;
}

}  // namespace

namespace handlers {


void passwordResetStart(const Request& req, Response& res) {
    std::string email = normalizeEmail(http::input(req, "email"));
    if (!validEmail(email)) {
        http::error(res, 400, "invalid_email", "邮箱格式不正确");
        return;
    }
    auto& cfg = AppConfig::get();
    if (!cfg.emailAvailable()) {
        http::error(res, 503, "email_unavailable", "密码重置暂不可用");
        return;
    }
    std::string errorCode;
    if (rateLimited("password_reset", email, http::clientIp(req), errorCode)) {
        if (errorCode == "rate_limited") {
            http::error(res, 429, "rate_limited", "请求过于频繁，请稍后再试");
        } else {
            http::error(res, 429, "cooldown", "请稍后再发送验证码");
        }
        return;
    }
    auto user = repo::findUserByEmail(email);
    if (user && user->isActive && deliverableEmail(user->email)) {
        std::string code = randomCode();
        std::map<std::string, JsonValue> record;
        record["user_id"] = JsonValue(user->id);
        record["code_hash"] = JsonValue(codeHash(code));
        record["expires_at"] = jsonx::num(util::nowEpoch() + kCodeSeconds);
        Cache::setJson(codeKey("password_reset", email), jsonx::dump(record), kCodeSeconds);
        Cache::set("password_reset_cooldown:" + digestEmail(email), "1", 60);
        sendCode(email, "UniSSO 重置密码验证码", "重置密码验证码", code, errorCode);
    } else {
        Cache::set("password_reset_cooldown:" + digestEmail(email), "1", 60);
    }
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["sent"] = JsonValue(true);
    obj["expires_in"] = jsonx::num(kCodeSeconds);
    obj["cooldown"] = jsonx::num(60);
    http::json(res, 200, JsonValue(obj));
}

void passwordResetConfirm(const Request& req, Response& res) {
    std::string email = normalizeEmail(http::input(req, "email"));
    std::string code = util::trim(http::input(req, "code"));
    std::string newPassword = http::input(req, "new_password");
    if (!validEmail(email) || code.size() != 6) {
        http::error(res, 400, "invalid_code", "验证码不正确或已过期");
        return;
    }
    std::string digest = digestEmail(email);
    long long attempts = Cache::incr("password_reset_attempts:" + digest, kCodeSeconds);
    if (attempts > 5) {
        Cache::del(codeKey("password_reset", email));
        Cache::del("password_reset_attempts:" + digest);
        http::error(res, 429, "too_many_attempts", "尝试次数过多，请重新获取验证码");
        return;
    }
    std::string userId;
    std::string errorCode;
    if (!readCodeRecord("password_reset", email, code, userId, errorCode)) {
        http::error(res, 401, "invalid_code", "验证码不正确或已过期");
        return;
    }
    auto user = repo::findUserById(userId);
    if (!user || !user->isActive || normalizeEmail(user->email) != email) {
        http::error(res, 401, "invalid_code", "验证码不正确或已过期");
        return;
    }
    try {
        password::validateStrength(newPassword);
    } catch (const std::exception& e) {
        http::error(res, 400, "weak_password", e.what());
        return;
    }
    if (Db::exec("UPDATE users SET password_hash=" + Db::quote(password::hash(newPassword)) +
                 ", updated_at=NOW() WHERE id=" + Db::quote(user->id)) < 0) {
        http::error(res, 500, "update_failed", "密码重置失败，请稍后重试");
        return;
    }
    Cache::del(codeKey("password_reset", email));
    Cache::del("password_reset_attempts:" + digest);
    Cache::del("password_reset_cooldown:" + digest);
    revokeUserSessions(user->id);
    Db::exec("UPDATE access_tokens SET revoked=1 WHERE user_id=" + Db::quote(user->id) +
             " AND revoked=0");
    Db::exec("UPDATE refresh_tokens SET revoked=1 WHERE user_id=" + Db::quote(user->id) +
             " AND revoked=0");
    applog::access("[auth] password reset user=" + user->id);
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    http::json(res, 200, JsonValue(obj));
}

}  // namespace handlers

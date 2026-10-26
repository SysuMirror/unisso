#include "core.h"

#include <algorithm>
#include <atomic>
#include <random>
#include <regex>

namespace {

std::string pairKey(const std::string& a, const std::string& b) {
    return a < b ? a + ":" + b : b + ":" + a;
}

bool pairExists(const std::string& a, const std::string& b) {
    std::string low = std::min(a, b);
    std::string high = std::max(a, b);
    auto rows = Db::query("SELECT id FROM account_peer_links WHERE user_a_id=" +
                          Db::quote(low) + " AND user_b_id=" + Db::quote(high) + " LIMIT 1");
    return !rows.empty();
}

std::string linkCodeKey(const std::string& a, const std::string& b) {
    return "account_link_code:" + pairKey(a, b);
}

std::string codeHash(const std::string& code) {
    auto& cfg = AppConfig::get();
    std::string key = cfg.secretKey.empty() ? cfg.signingKeyId : cfg.secretKey;
    return util::hex(util::hmacSha256Raw(key, code));
}

bool truthy(const std::string& value) {
    std::string lowered = util::lower(util::trim(value));
    return lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on";
}

std::string randomCode() {
    static std::atomic<unsigned int> counter{0};
    std::random_device rd;
    unsigned int value = rd() + counter.fetch_add(1);
    return std::to_string(100000 + (value % 900000));
}

bool validEmail(const std::string& email) {
    static const std::regex pattern(R"(^[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}$)");
    return std::regex_match(email, pattern) && email.size() <= 128;
}

std::optional<User> bindableTarget(const User& current, const std::string& email,
                                   std::string& errorCode, std::string& message) {
    auto target = repo::findUserByEmail(email);
    if (!target) {
        errorCode = "account_not_found";
        message = "该邮箱没有 UniSSO 账号";
        return std::nullopt;
    }
    if (target->id == current.id) {
        errorCode = "cannot_bind_self";
        message = "不能关联当前账号";
        return std::nullopt;
    }
    if (pairExists(current.id, target->id)) {
        errorCode = "already_linked";
        message = "这两个账号已经关联";
        return std::nullopt;
    }
    return target;
}

JsonValue targetJson(const User& target) {
    std::map<std::string, JsonValue> obj;
    obj["id"] = JsonValue(target.id);
    obj["email"] = JsonValue(target.email);
    obj["username"] = JsonValue(target.username);
    obj["full_name"] = JsonValue(target.fullName);
    return JsonValue(obj);
}

}  // namespace

namespace handlers {

void accountUpdateProfile(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto hasField = [&req](const std::string& name) {
        if (!req.jsonBody || req.jsonBody->getType() != JsonValue::OBJECT) return false;
        auto obj = req.jsonBody->asObject();
        return obj.find(name) != obj.end();
    };
    bool hasFullName = hasField("full_name");
    bool hasStudentId = hasField("student_id");
    bool hasStepUp = hasField("step_up_enabled");
    if (!hasFullName && !hasStudentId && !hasStepUp) {
        http::error(res, 400, "invalid_input", "没有可更新的字段");
        return;
    }
    std::string fullName = util::trim(http::input(req, "full_name"));
    std::string studentId = util::trim(http::input(req, "student_id"));
    if (hasFullName && fullName.size() > 100) {
        http::error(res, 400, "invalid_input", "显示名不能超过 100 个字符");
        return;
    }
    if (hasStudentId && studentId.size() > 64) {
        http::error(res, 400, "invalid_input", "学号不能超过 64 个字符");
        return;
    }
    std::vector<std::string> sets;
    if (hasFullName) sets.push_back("full_name=" + Db::nullable(fullName));
    if (hasStudentId) sets.push_back("student_id=" + Db::nullable(studentId));
    if (hasStepUp) {
        bool enabled = truthy(http::input(req, "step_up_enabled"));
        sets.push_back("step_up_enabled=" + std::string(enabled ? "1" : "0"));
    }
    sets.push_back("updated_at=NOW()");
    if (Db::exec("UPDATE users SET " + util::join(sets, ", ") + " WHERE id=" +
                 Db::quote(user.id)) < 0) {
        http::error(res, 500, "update_failed", "资料更新失败，请稍后重试");
        return;
    }
    auto fresh = repo::findUserById(user.id);
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    if (fresh) obj["user"] = repo::userJson(*fresh, true);
    http::json(res, 200, JsonValue(obj));
}

void accountLinksList(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto peers = repo::linkedPeers(user.id);
    std::map<std::string, JsonValue> obj;
    obj["accounts"] = JsonValue(repo::linkedAccountsJson(user.id));
    obj["role"] = JsonValue(peers.empty() ? "none" : "linked");
    http::json(res, 200, JsonValue(obj));
}

void accountLinkSendCode(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string email = util::lower(util::trim(http::input(req, "email")));
    if (!validEmail(email)) {
        http::error(res, 400, "invalid_email", "请输入有效邮箱");
        return;
    }
    std::string errorCode, message;
    auto target = bindableTarget(user, email, errorCode, message);
    if (!target) {
        http::error(res, errorCode == "account_not_found" ? 404 : 409, errorCode, message);
        return;
    }

    std::string pair = pairKey(user.id, target->id);
    long long sends = Cache::incr("account_link_send:" + pair, 60);
    if (sends > 1) {
        http::error(res, 429, "rate_limited", "验证码发送过于频繁，请稍后再试");
        return;
    }

    std::string code = randomCode();
    std::map<std::string, JsonValue> record;
    record["code_hash"] = JsonValue(codeHash(code));
    record["expires_at"] = jsonx::num(util::nowEpoch() + 600);
    Cache::setJson(linkCodeKey(user.id, target->id), jsonx::dump(record), 600);

    std::string html =
        "<div style=\"font-family:-apple-system,Segoe UI,PingFang SC,sans-serif;color:#171717\">"
        "<h2 style=\"font-size:20px;margin:0 0 12px\">关联账号验证码</h2>"
        "<p style=\"color:#5f6368;font-size:14px\">另一个 UniSSO 账号申请与你关联，验证码：</p>"
        "<p style=\"font-size:30px;font-weight:700;letter-spacing:6px;margin:16px 0\">" + code + "</p>"
        "<p style=\"color:#5f6368;font-size:13px\">验证码 10 分钟内有效。如果不是你本人操作，请忽略此邮件。</p>"
        "</div>";
    std::string mailError;
    if (!mail::send(target->email, "UniSSO 关联账号验证码", html, mailError)) {
        Cache::del(linkCodeKey(user.id, target->id));
        http::error(res, 503, "email_unavailable", "验证码发送失败，请稍后重试");
        return;
    }
    http::ok(res);
}

void accountLinkBind(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string email = util::lower(util::trim(http::input(req, "email")));
    std::string password = http::input(req, "password");
    std::string code = util::trim(http::input(req, "code"));
    if (!validEmail(email)) {
        http::error(res, 400, "invalid_email", "请输入有效邮箱");
        return;
    }
    std::string errorCode, message;
    auto target = bindableTarget(user, email, errorCode, message);
    if (!target) {
        http::error(res, errorCode == "account_not_found" ? 404 : 409, errorCode, message);
        return;
    }
    if (password.empty() && code.empty()) {
        http::error(res, 400, "verification_required", "请输入对方密码或邮箱验证码");
        return;
    }
    std::string pair = pairKey(user.id, target->id);
    std::string attemptKey = "account_link_attempts:" + pair;
    long long attempts = 0;
    try { attempts = std::stoll(Cache::get(attemptKey)); } catch (...) {}
    if (attempts >= 5) {
        http::error(res, 429, "too_many_attempts", "验证失败次数过多，请 15 分钟后再试");
        return;
    }

    bool verified = false;
    if (!password.empty()) {
        verified = password::verify(password, target->passwordHash);
    }
    if (!verified && !code.empty()) {
        std::string raw = Cache::get(linkCodeKey(user.id, target->id));
        std::map<std::string, JsonValue> record;
        if (!raw.empty() && jsonx::parseObject(raw, record)) {
            long long expiresAt = 0;
            auto it = record.find("expires_at");
            if (it != record.end() && it->second.getType() == JsonValue::NUMBER) {
                expiresAt = static_cast<long long>(it->second.asNumber());
            }
            std::string expected = jsonx::getString(record, "code_hash");
            if (expiresAt > util::nowEpoch() && !expected.empty() &&
                util::constantTimeEquals(expected, codeHash(code))) {
                verified = true;
                Cache::del(linkCodeKey(user.id, target->id));
            }
        }
    }
    if (!verified) {
        Cache::incr(attemptKey, 900);
        http::error(res, 401, "verification_failed", "密码或验证码不正确");
        return;
    }
    Cache::del(attemptKey);

    std::string low = std::min(user.id, target->id);
    std::string high = std::max(user.id, target->id);
    std::string linkId = util::uuid4();
    int inserted = Db::exec(
        "INSERT INTO account_peer_links (id,user_a_id,user_b_id,created_by,created_at) "
        "VALUES (" + Db::quote(linkId) + "," + Db::quote(low) + "," +
        Db::quote(high) + "," + Db::quote(user.id) + ",NOW())");
    if (inserted <= 0) {
        http::error(res, 409, "already_linked", "这两个账号已经关联");
        return;
    }
    std::map<std::string, JsonValue> obj;
    obj["linked_account"] = targetJson(*target);
    obj["link_id"] = JsonValue(linkId);
    http::ok(res, JsonValue(obj));
}

void accountLinkUnbind(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string linkId = http::param(req, "id");
    auto rows = Db::query("SELECT user_a_id,user_b_id FROM account_peer_links WHERE id=" +
                          Db::quote(linkId) + " LIMIT 1");
    if (rows.empty()) {
        http::error(res, 404, "not_found", "关联关系不存在");
        return;
    }
    std::string a = rows.front().at("user_a_id");
    std::string b = rows.front().at("user_b_id");
    if (a != user.id && b != user.id) {
        http::error(res, 403, "forbidden", "无权解除该关联");
        return;
    }
    Db::exec("DELETE FROM account_peer_links WHERE id=" + Db::quote(linkId));
    // Keep the legacy table in sync so the startup migration cannot resurrect it.
    Db::exec("DELETE FROM account_links WHERE "
             "(canonical_user_id=" + Db::quote(a) + " AND linked_user_id=" + Db::quote(b) + ") OR "
             "(canonical_user_id=" + Db::quote(b) + " AND linked_user_id=" + Db::quote(a) + ")");
    http::ok(res);
}

}  // namespace handlers

#include "core.h"

#include <cctype>

namespace {

std::string successHtml() {
    return R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>登录成功</title></head>
<body style="margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#fbfbfa;color:#171717;font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Microsoft YaHei',sans-serif">
<div style="text-align:center;padding:32px">
<img src="/logo.png" alt="" style="width:64px;height:auto;margin-bottom:18px">
<h1 style="font-size:22px;letter-spacing:-.02em;margin:0 0 8px">登录成功</h1>
<p style="font-size:13px;color:#5d5c58;margin:0">请回到电脑端继续操作。</p>
</div></body></html>)HTML";
}

std::string htmlEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

bool validState(const std::string& state) {
    if (state.size() < 16 || state.size() > 128) return false;
    for (unsigned char c : state) {
        if (!std::isalnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}

std::string errorHtml(const std::string& message) {
    return R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>登录失败</title></head>
<body style="margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#fbfbfa;color:#171717;font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Microsoft YaHei',sans-serif">
<div style="text-align:center;padding:32px;max-width:360px">
<h1 style="font-size:20px;letter-spacing:-.02em;margin:0 0 10px">登录未完成</h1>
<p style="font-size:13px;color:#5d5c58;line-height:1.6;margin:0">)HTML" + htmlEscape(message) +
        R"HTML(</p></div></body></html>)HTML";
}

void html(Response& res, int status, const std::string& body) {
    res.statusCode = status;
    res.headers["Content-Type"] = "text/html; charset=utf-8";
    res.headers["Cache-Control"] = "no-store";
    res.body = body;
}

}  // namespace

namespace handlers {

void sseQrStatus(const Request&, Response& res) {
    std::map<std::string, JsonValue> obj;
    obj["enabled"] = JsonValue(AppConfig::get().sseOauthEnabled);
    http::json(res, 200, JsonValue(obj));
}

void sseQrStart(const Request& req, Response& res) {
    auto& cfg = AppConfig::get();
    if (!cfg.sseOauthEnabled) {
        http::error(res, 501, "sse_oauth_not_configured", "扫码登录尚未配置");
        return;
    }
    long long starts = Cache::incr("sse_qr_start:" + http::clientIp(req), 60);
    if (starts > 20) {
        http::error(res, 429, "rate_limited", "请求过于频繁，请稍后再试");
        return;
    }
    std::string state = util::randomToken(24);
    std::map<std::string, JsonValue> pending;
    pending["status"] = JsonValue("pending");
    pending["created_at"] = jsonx::num(util::nowEpoch());
    Cache::setJson("sse_qr:" + state, jsonx::dump(pending), 300);
    // The browser that displays the QR must be the one that claims the
    // resulting session. Without this, a crafted poll URL could log a victim
    // into the attacker's account.
    http::setCookie(res, req, "unisso_qr_state", state, 300);

    std::map<std::string, JsonValue> obj;
    obj["state"] = JsonValue(state);
    obj["qr_url"] = JsonValue(sse::authorizeUrl(state));
    obj["expires_in"] = jsonx::num(300);
    http::json(res, 200, JsonValue(obj));
}

void sseQrPoll(const Request& req, Response& res) {
    std::string state = http::query(req, "state");
    if (!validState(state)) {
        http::error(res, 400, "invalid_state", "缺少 state");
        return;
    }
    if (http::cookie(req, "unisso_qr_state") != state) {
        http::error(res, 403, "qr_state_mismatch", "请从发起扫码的浏览器继续");
        return;
    }
    std::string raw = Cache::get("sse_qr:" + state);
    std::map<std::string, JsonValue> obj;
    if (raw.empty()) {
        obj["status"] = JsonValue("expired");
        http::json(res, 200, JsonValue(obj));
        return;
    }
    std::map<std::string, JsonValue> data;
    if (!jsonx::parseObject(raw, data)) {
        obj["status"] = JsonValue("expired");
        http::json(res, 200, JsonValue(obj));
        return;
    }
    std::string status = jsonx::getString(data, "status", "pending");
    if (status != "authorized") {
        obj["status"] = JsonValue("pending");
        http::json(res, 200, JsonValue(obj));
        return;
    }
    std::string session = jsonx::getString(data, "session_id");
    if (session.empty()) {
        obj["status"] = JsonValue("expired");
        http::json(res, 200, JsonValue(obj));
        return;
    }
    authx::setSessionCookie(res, req, session);
    Cache::del("sse_qr:" + state);
    Cache::del("sse_qr_claim:" + state);
    obj["status"] = JsonValue("authorized");
    http::json(res, 200, JsonValue(obj));
}

void sseBindStart(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto& cfg = AppConfig::get();
    if (!cfg.sseOauthEnabled) {
        http::error(res, 501, "sse_oauth_not_configured", "集市账号绑定尚未配置");
        return;
    }
    std::string state = util::randomToken(24);
    std::map<std::string, JsonValue> pending;
    pending["user_id"] = JsonValue(user.id);
    pending["created_at"] = jsonx::num(util::nowEpoch());
    Cache::setJson("sse_bind:" + state, jsonx::dump(pending), 300);
    std::map<std::string, JsonValue> obj;
    obj["state"] = JsonValue(state);
    obj["authorize_url"] = JsonValue(sse::authorizeUrl(state));
    http::json(res, 200, JsonValue(obj));
}

void sseOauthCallback(const Request& req, Response& res) {
    std::string error = http::query(req, "error");
    if (!error.empty()) {
        html(res, 400, errorHtml("集市授权未完成：" + error));
        return;
    }
    std::string state = http::query(req, "state");
    std::string code = http::query(req, "code");
    if (!validState(state) || code.empty()) {
        html(res, 400, errorHtml("缺少授权参数，请重新扫码。"));
        return;
    }
    // Binding an SSE identity to an already logged-in local account.
    std::string bindRaw = Cache::get("sse_bind:" + state);
    if (!bindRaw.empty()) {
        std::map<std::string, JsonValue> bindData;
        std::string bindUser = jsonx::parseObject(bindRaw, bindData)
            ? jsonx::getString(bindData, "user_id") : "";
        if (bindUser.empty()) {
            Cache::del("sse_bind:" + state);
            html(res, 400, errorHtml("绑定请求已失效，请重新发起。"));
            return;
        }
        auto current = authx::currentUser(req);
        if (!current || current->id != bindUser) {
            html(res, 403, errorHtml("请在同一浏览器完成账号绑定。"));
            return;
        }
        if (!Cache::setnx("sse_bind_claim:" + state, "1", 300)) {
            html(res, 409, errorHtml("绑定请求已处理，请重新发起。"));
            return;
        }
        sse::Profile profile;
        std::string sseError;
        if (!sse::exchangeOauthCode(code, profile, sseError)) {
            html(res, 502, errorHtml("集市账号信息获取失败，请重试。"));
            return;
        }
        std::string bindError;
        if (!sse::bindIdentityToUser(bindUser, profile, bindError)) {
            Cache::del("sse_bind:" + state);
            http::redirect(res, AppConfig::get().issuer + "/profile?bind=conflict");
            return;
        }
        Cache::del("sse_bind:" + state);
        http::redirect(res, AppConfig::get().issuer + "/profile?bind=success");
        return;
    }

    std::string raw = Cache::get("sse_qr:" + state);
    if (raw.empty()) {
        html(res, 400, errorHtml("二维码已过期，请回到电脑端刷新。"));
        return;
    }
    std::map<std::string, JsonValue> pending;
    if (jsonx::parseObject(raw, pending) &&
        jsonx::getString(pending, "status") == "authorized") {
        html(res, 409, errorHtml("这个二维码已经被使用，请回到电脑端刷新。"));
        return;
    }

    // One callback may claim a QR state. A concurrent second scan of the same
    // QR is rejected instead of overwriting the first session.
    std::string claimKey = "sse_qr_claim:" + state;
    if (!Cache::setnx(claimKey, "1", 300)) {
        html(res, 409, errorHtml("这个二维码正在处理中，请回到电脑端刷新。"));
        return;
    }

    sse::Profile profile;
    std::string sseError;
    if (!sse::exchangeOauthCode(code, profile, sseError)) {
        Cache::del(claimKey);
        html(res, 502, errorHtml("集市账号信息获取失败，请重试。"));
        return;
    }
    auto user = sse::upsertUser(profile, "");
    if (!user || !user->isActive) {
        Cache::del(claimKey);
        html(res, 400, errorHtml("本地账号创建失败，请联系管理员。"));
        return;
    }
    std::string deviceHash = authx::trustDevice(user->id, req, res);
    std::string session = authx::createSession(
        user->id, user->username.empty() ? user->email : user->username, req, deviceHash);
    std::map<std::string, JsonValue> authorized;
    authorized["status"] = JsonValue("authorized");
    authorized["session_id"] = JsonValue(session);
    authorized["user_id"] = JsonValue(user->id);
    Cache::setJson("sse_qr:" + state, jsonx::dump(authorized), 300);
    html(res, 200, successHtml());
}

}  // namespace handlers

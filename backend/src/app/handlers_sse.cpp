#include "core.h"
#include "app/logger.h"

#include <cctype>

namespace {

std::string successHtml() {
    return R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>已确认</title></head>
<body style="margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#fbfbfa;color:#171717;font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Microsoft YaHei',sans-serif">
<div style="text-align:center;padding:32px">
<img src="/logo.png" alt="" style="width:64px;height:auto;margin-bottom:18px">
<h1 style="font-size:22px;letter-spacing:-.02em;margin:0 0 8px">已确认登录</h1>
<p style="font-size:13px;color:#5d5c58;margin:0">请回到电脑端继续（新设备可能还需二次验证）。</p>
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

// Relative in-app paths only (e.g. /authorize?...); anything else lands on /.
std::string safeNextPath(const std::string& next) {
    return next.rfind("/", 0) == 0 && next.rfind("//", 0) != 0 ? next : "/";
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

// Some webviews (notably WeChat's XWeb) do not store Set-Cookie headers that
// arrive on a 302 inside a cross-site redirect chain — which would silently
// drop the session/step-up cookie these responses carry. Answer 200 HTML with
// a meta-refresh instead: the cookie rides a normal first-party document
// response, which is reliably stored. (CSP script-src 'self' forbids an
// inline <script> here; meta refresh is not CSP-restricted.)
std::string redirectHtml(const std::string& url) {
    std::string safe = htmlEscape(url);
    return R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="0;url=)HTML" + safe + R"HTML("><title>正在完成登录…</title></head>
<body style="margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#fbfbfa;color:#171717;font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Microsoft YaHei',sans-serif">
<p style="font-size:13px;color:#5d5c58">正在完成登录… 若没有自动跳转，请<a href=")HTML" + safe +
        R"HTML(" style="color:#171717">点这里继续</a>。</p></body></html>)HTML";
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
    // The login page reached us from /authorize (third-party SSO). Remember
    // where to go after login so the browser returns to the consent page
    // instead of the UniSSO home.
    pending["next"] = JsonValue(safeNextPath(util::trim(http::query(req, "next"))));
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
    // Exactly one poll may convert the approval into a session.
    if (!Cache::setnx("sse_qr_poll:" + state, "1", 300)) {
        obj["status"] = JsonValue("pending");
        http::json(res, 200, JsonValue(obj));
        return;
    }
    auto user = repo::findUserById(jsonx::getString(data, "user_id"));
    if (!user || !user->isActive) {
        Cache::del("sse_qr:" + state);
        Cache::del("sse_qr_claim:" + state);
        obj["status"] = JsonValue("expired");
        http::json(res, 200, JsonValue(obj));
        return;
    }
    // The session is created here (the desktop browser), not on the phone,
    // so the gate sees the desktop's UA/IP/cookie.
    std::string next = safeNextPath(jsonx::getString(data, "next", "/"));
    Cache::del("sse_qr:" + state);
    Cache::del("sse_qr_claim:" + state);
    repo::updateUserLastLogin(user->id);
    std::string session = authx::createSession(
        user->id, user->username.empty() ? user->email : user->username, req);
    authx::setSessionCookie(res, req, session);
    applog::access("[auth] sse-qr login user=" + user->id);
    obj["status"] = JsonValue("authorized");
    obj["next"] = JsonValue(next);
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
    std::string rejectReason;
    auto user = sse::upsertUser(profile, "", &rejectReason);
    if (!user || !user->isActive) {
        Cache::del(claimKey);
        if (rejectReason == "sse_already_bound") {
            html(res, 409, errorHtml("该集市账号已绑定其他 UniSSO 账号。请到该账号的「个人中心 → 外部身份」确认绑定关系，"
                                     "或先解绑后重新扫码。"));
        } else {
            html(res, 400, errorHtml("本地账号创建失败，请联系管理员。"));
        }
        return;
    }

    // Same-browser flow ("集市账号登录" button or tapping/long-pressing the QR
    // on the phone that opened it): the unisso_qr_state cookie identifies the
    // browser that started this. Complete the login here instead of leaving a
    // dead-end "back to your computer" page. A second device that scanned has
    // no such cookie and keeps the original approval-only path.
    if (http::cookie(req, "unisso_qr_state") == state) {
        std::string next = safeNextPath(jsonx::getString(pending, "next", "/"));
        repo::updateUserLastLogin(user->id);
        std::string session = authx::createSession(
            user->id, user->username.empty() ? user->email : user->username, req);
        authx::setSessionCookie(res, req, session);
        Cache::del("sse_qr:" + state);
        Cache::del(claimKey);
        applog::access("[sse-oauth] same-device login user=" + user->id);
        html(res, 200, redirectHtml(next));
        return;
    }
    // The phone only records the approval. The desktop session (and any
    // device binding) is created by the polling desktop browser itself, so it
    // carries the desktop's UA/IP and is subject to the normal step-up gate.
    std::map<std::string, JsonValue> authorized;
    authorized["status"] = JsonValue("authorized");
    authorized["user_id"] = JsonValue(user->id);
    // Carry the original destination over so the polling desktop browser can
    // return to /authorize after claiming the session.
    authorized["next"] = JsonValue(safeNextPath(jsonx::getString(pending, "next", "/")));
    authorized["decided_at"] = jsonx::num(util::nowEpoch());
    Cache::setJson("sse_qr:" + state, jsonx::dump(authorized), 300);
    applog::access("[sse-qr] login approved user=" + user->id);
    html(res, 200, successHtml());
}

}  // namespace handlers

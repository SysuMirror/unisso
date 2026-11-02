// Scan-to-login with the UniSSO mobile session: the PC shows a QR that links
// to /qr/<id>; a phone already logged into UniSSO (or logging in first)
// confirms the request, and the PC browser then claims the new session.
// This is separate from the SSE (集市) OAuth QR in handlers_sse.cpp.
#include "core.h"
#include "app/logger.h"

namespace {

constexpr int kQrSeconds = 300;

bool validQrId(const std::string& id) {
    if (id.size() < 16 || id.size() > 64) return false;
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool loadRecord(const std::string& id, std::map<std::string, JsonValue>& rec) {
    std::string raw = Cache::get("qr_login:" + id);
    return !raw.empty() && jsonx::parseObject(raw, rec);
}

int remainingTtl(const std::string& id) {
    long long left = Cache::ttl("qr_login:" + id);
    return left > 0 ? static_cast<int>(left) : kQrSeconds;
}

std::string safeNextPath(const std::string& next) {
    return next.rfind("/", 0) == 0 && next.rfind("//", 0) != 0 ? next : "/";
}

// Coarse display strings only — never the raw UA.
void summarizeAgent(const std::string& ua, std::string& browser, std::string& os) {
    std::string l = util::lower(ua);
    auto has = [&](const char* needle) { return l.find(needle) != std::string::npos; };
    if (has("micromessenger")) browser = "微信";
    else if (has("quark")) browser = "夸克";
    else if (has("edg/") || has("edge")) browser = "Edge";
    else if (has("firefox")) browser = "Firefox";
    else if (has("chrome") || has("crios")) browser = "Chrome";
    else if (has("safari")) browser = "Safari";
    else browser = "浏览器";
    if (has("iphone") || has("ios")) os = "iPhone";
    else if (has("ipad")) os = "iPad";
    else if (has("android")) os = "安卓";
    else if (has("windows")) os = "Windows";
    else if (has("mac os x") || has("macintosh")) os = "macOS";
    else if (has("linux")) os = "Linux";
    else os = "未知系统";
}

std::string statusOf(const std::map<std::string, JsonValue>& rec) {
    return jsonx::getString(rec, "status", "pending");
}

void replyStatus(Response& res, const std::string& status) {
    std::map<std::string, JsonValue> obj;
    obj["status"] = JsonValue(status);
    http::json(res, 200, JsonValue(obj));
}

}  // namespace

namespace handlers {

void qrLoginStart(const Request& req, Response& res) {
    long long starts = Cache::incr("qr_login_start:" + http::clientIp(req), 60);
    if (starts > 10) {
        http::error(res, 429, "rate_limited", "请求过于频繁，请稍后再试");
        return;
    }
    std::string id = util::randomToken(24);
    std::string ua = http::header(req, "User-Agent");
    if (ua.size() > 255) ua.resize(255);
    std::map<std::string, JsonValue> rec;
    rec["status"] = JsonValue("pending");
    rec["created_at"] = jsonx::num(util::nowEpoch());
    rec["user_agent"] = JsonValue(ua);
    rec["ip"] = JsonValue(http::clientIp(req));
    rec["next"] = JsonValue(safeNextPath(util::trim(http::query(req, "next"))));
    Cache::setJson("qr_login:" + id, jsonx::dump(rec), kQrSeconds);
    // Only the browser that rendered the QR may claim the resulting session.
    http::setCookie(res, req, "unisso_qr_login", id, kQrSeconds);

    std::map<std::string, JsonValue> obj;
    obj["id"] = JsonValue(id);
    obj["qr_url"] = JsonValue(AppConfig::get().issuer + "/qr/" + id);
    obj["expires_in"] = jsonx::num(kQrSeconds);
    http::json(res, 200, JsonValue(obj));
}

void qrLoginPoll(const Request& req, Response& res) {
    std::string id = util::trim(http::query(req, "id"));
    if (!validQrId(id)) {
        http::error(res, 400, "invalid_id", "缺少二维码参数");
        return;
    }
    if (http::cookie(req, "unisso_qr_login") != id) {
        http::error(res, 403, "qr_state_mismatch", "请从发起扫码的浏览器继续");
        return;
    }
    std::map<std::string, JsonValue> rec;
    if (!loadRecord(id, rec)) {
        replyStatus(res, "expired");
        return;
    }
    std::string status = statusOf(rec);
    if (status == "pending" || status == "scanned") {
        replyStatus(res, status);
        return;
    }
    if (status == "denied") {
        replyStatus(res, "denied");
        return;
    }
    if (status != "confirmed") {
        replyStatus(res, "expired");
        return;
    }
    // Exactly one poll may convert the approval into a session.
    if (!Cache::setnx("qr_login_claim:" + id, "1", kQrSeconds)) {
        replyStatus(res, "expired");
        return;
    }
    auto user = repo::findUserById(jsonx::getString(rec, "user_id"));
    if (!user || !user->isActive) {
        Cache::del("qr_login:" + id);
        Cache::del("qr_login_claim:" + id);
        replyStatus(res, "expired");
        return;
    }
    std::string next = safeNextPath(jsonx::getString(rec, "next", "/"));
    // A scan is only as strong as the phone session behind it. New device /
    // dormant accounts must still pass a second factor on the desktop.
    // allowDeviceMethod=false: the phone that just approved the scan must not
    // also serve as the "other logged-in device" confirming it.
    if (loginNeedsStepUp(*user, req, false)) {
        Cache::del("qr_login:" + id);
        Cache::del("qr_login_claim:" + id);
        applog::access("[qr] step-up required before claim user=" + user->id);
        beginStepUp(req, res, *user, next, false);
        return;
    }
    std::string deviceHash;
    if (authx::deviceTrusted(user->id, req)) {
        deviceHash = authx::deviceHash(http::cookie(req, "unisso_device"));
    }
    repo::updateUserLastLogin(user->id);
    std::string session = authx::createSession(
        user->id, user->username.empty() ? user->email : user->username, req, deviceHash);
    authx::setSessionCookie(res, req, session);
    Cache::del("qr_login:" + id);
    Cache::del("qr_login_claim:" + id);
    applog::access("[qr] mobile-confirmed login user=" + user->id);
    std::map<std::string, JsonValue> obj;
    obj["status"] = JsonValue("ok");
    obj["user"] = repo::userJson(*user, true);
    obj["next"] = JsonValue(next);
    http::json(res, 200, JsonValue(obj));
}

// Public beacon: fires as soon as the phone opens the QR link, even before
// (or without) the mobile user being logged in. Only flips pending→scanned;
// reveals nothing about the request.
void qrLoginScan(const Request& req, Response& res) {
    std::string id = util::trim(http::query(req, "id"));
    if (!validQrId(id)) {
        http::error(res, 400, "invalid_id", "二维码参数不正确");
        return;
    }
    long long scans = Cache::incr("qr_login_scan:" + http::clientIp(req), 60);
    if (scans > 60) {
        http::error(res, 429, "rate_limited", "请求过于频繁，请稍后再试");
        return;
    }
    std::map<std::string, JsonValue> rec;
    if (loadRecord(id, rec) && statusOf(rec) == "pending" &&
        Cache::setnx("qr_login_seen:" + id, "1", remainingTtl(id))) {
        rec["status"] = JsonValue("scanned");
        Cache::setJson("qr_login:" + id, jsonx::dump(rec), remainingTtl(id));
    }
    http::ok(res);
}

void qrLoginDetail(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string id = util::trim(http::query(req, "id"));
    if (!validQrId(id)) {
        http::error(res, 400, "invalid_id", "二维码参数不正确");
        return;
    }
    std::map<std::string, JsonValue> rec;
    if (!loadRecord(id, rec)) {
        replyStatus(res, "expired");
        return;
    }
    std::string status = statusOf(rec);
    if (status == "pending" &&
        Cache::setnx("qr_login_seen:" + id, "1", remainingTtl(id))) {
        status = "scanned";
        rec["status"] = JsonValue(status);
        Cache::setJson("qr_login:" + id, jsonx::dump(rec), remainingTtl(id));
    }
    std::map<std::string, JsonValue> obj;
    obj["status"] = JsonValue(status);
    std::string browser, os;
    summarizeAgent(jsonx::getString(rec, "user_agent"), browser, os);
    std::map<std::string, JsonValue> device;
    device["browser"] = JsonValue(browser);
    device["os"] = JsonValue(os);
    device["ip"] = JsonValue(jsonx::getString(rec, "ip"));
    long long createdAt = 0;
    auto it = rec.find("created_at");
    if (it != rec.end() && it->second.getType() == JsonValue::NUMBER) {
        createdAt = static_cast<long long>(it->second.asNumber());
    }
    device["requested_at"] = JsonValue(util::formatTs(createdAt));
    device["requested_epoch"] = jsonx::num(createdAt);
    obj["device"] = JsonValue(device);
    http::json(res, 200, JsonValue(obj));
}

void qrLoginConfirm(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string id = util::trim(http::input(req, "id"));
    if (!validQrId(id)) {
        http::error(res, 400, "invalid_id", "二维码参数不正确");
        return;
    }
    long long decides = Cache::incr("qr_login_decide:" + user.id, 60);
    if (decides > 20) {
        http::error(res, 429, "rate_limited", "操作过于频繁，请稍后再试");
        return;
    }
    std::map<std::string, JsonValue> rec;
    if (!loadRecord(id, rec)) {
        http::error(res, 410, "qr_expired", "二维码已过期，请回到电脑上刷新");
        return;
    }
    std::string status = statusOf(rec);
    if (status == "confirmed") {
        if (jsonx::getString(rec, "user_id") == user.id) {
            http::ok(res);
            return;
        }
        http::error(res, 409, "already_handled", "这个二维码已在其他设备上处理");
        return;
    }
    if (status == "denied") {
        http::error(res, 409, "already_denied", "这个二维码已被拒绝");
        return;
    }
    if (status != "pending" && status != "scanned") {
        http::error(res, 409, "invalid_state", "二维码状态已变化，请刷新后重试");
        return;
    }
    if (!Cache::setnx("qr_login_decided:" + id, "1", remainingTtl(id))) {
        http::error(res, 409, "already_handled", "这个二维码刚刚已被处理");
        return;
    }
    rec["status"] = JsonValue("confirmed");
    rec["user_id"] = JsonValue(user.id);
    rec["decided_at"] = jsonx::num(util::nowEpoch());
    Cache::setJson("qr_login:" + id, jsonx::dump(rec), remainingTtl(id));
    applog::access("[qr] login approved user=" + user.id + " qr=" + id.substr(0, 8));
    http::ok(res);
}

void qrLoginDeny(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string id = util::trim(http::input(req, "id"));
    if (!validQrId(id)) {
        http::error(res, 400, "invalid_id", "二维码参数不正确");
        return;
    }
    long long decides = Cache::incr("qr_login_decide:" + user.id, 60);
    if (decides > 20) {
        http::error(res, 429, "rate_limited", "操作过于频繁，请稍后再试");
        return;
    }
    std::map<std::string, JsonValue> rec;
    if (!loadRecord(id, rec)) {
        http::error(res, 410, "qr_expired", "二维码已过期，请回到电脑上刷新");
        return;
    }
    std::string status = statusOf(rec);
    if (status == "denied") {
        http::ok(res);
        return;
    }
    if (status == "confirmed") {
        http::error(res, 409, "already_handled", "这个二维码已确认，无法拒绝");
        return;
    }
    if (!Cache::setnx("qr_login_decided:" + id, "1", remainingTtl(id))) {
        http::error(res, 409, "already_handled", "这个二维码刚刚已被处理");
        return;
    }
    rec["status"] = JsonValue("denied");
    rec["decided_at"] = jsonx::num(util::nowEpoch());
    Cache::setJson("qr_login:" + id, jsonx::dump(rec), remainingTtl(id));
    applog::access("[qr] login denied user=" + user.id + " qr=" + id.substr(0, 8));
    http::ok(res);
}

}  // namespace handlers

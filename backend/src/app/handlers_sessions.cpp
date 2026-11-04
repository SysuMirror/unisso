#include "core.h"
#include "app/logger.h"

#include <algorithm>

namespace {

struct SessionInfo {
    std::string id;
    std::string userId;
    std::string username;
    std::string userAgent;
    std::string ip;
    std::string createdAt;
    long long lastSeenAt = 0;
    long long expiresAt = 0;
    bool current = false;
};

std::string publicSessionId(const std::string& sessionId) {
    auto& cfg = AppConfig::get();
    std::string key = cfg.secretKey.empty() ? cfg.signingKeyId : cfg.secretKey;
    return util::hex(util::hmacSha256Raw(key, sessionId));
}

bool parseSession(const std::string& name, const std::string& raw, SessionInfo& out) {
    if (name.rfind("session:", 0) != 0) return false;
    std::map<std::string, JsonValue> data;
    if (!jsonx::parseObject(raw, data)) return false;
    out.id = name.substr(8);
    out.userId = jsonx::getString(data, "user_id");
    out.username = jsonx::getString(data, "username");
    out.userAgent = jsonx::getString(data, "user_agent");
    out.ip = jsonx::getString(data, "ip");
    out.createdAt = jsonx::getString(data, "created_at");
    auto number = [&data](const char* key) -> long long {
        auto it = data.find(key);
        if (it == data.end() || it->second.getType() != JsonValue::NUMBER) return 0;
        return static_cast<long long>(it->second.asNumber());
    };
    out.lastSeenAt = number("last_seen_at");
    out.expiresAt = number("expires_at");
    return !out.userId.empty();
}

void deleteSession(const std::string& sessionId) {
    Cache::del("session:" + sessionId);
    Cache::del("browser_csrf:session:" + sessionId);
}

}  // namespace

namespace handlers {

void sessionsList(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto currentId = authx::sessionId(req);
    std::vector<SessionInfo> sessions;
    for (const auto& name : Cache::scan("session:*", 500)) {
        std::string raw = Cache::get(name);
        if (raw.empty()) continue;
        SessionInfo info;
        if (!parseSession(name, raw, info) || info.userId != user.id) continue;
        info.current = currentId && info.id == *currentId;
        sessions.push_back(info);
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionInfo& a, const SessionInfo& b) {
                  if (a.current != b.current) return a.current;
                  return a.lastSeenAt > b.lastSeenAt;
              });
    if (sessions.size() > 50) sessions.resize(50);

    std::vector<JsonValue> items;
    for (const auto& info : sessions) {
        std::map<std::string, JsonValue> obj;
        obj["id"] = JsonValue(publicSessionId(info.id));
        obj["current"] = JsonValue(info.current);
        obj["username"] = JsonValue(info.username);
        obj["user_agent"] = JsonValue(info.userAgent);
        obj["ip"] = JsonValue(info.ip);
        obj["created_at"] = JsonValue(info.createdAt);
        obj["last_seen_at"] = JsonValue(info.lastSeenAt > 0 ? util::formatTs(info.lastSeenAt) : "");
        obj["expires_at"] = JsonValue(info.expiresAt > 0 ? util::formatTs(info.expiresAt) : "");
        items.push_back(JsonValue(obj));
    }
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["sessions"] = JsonValue(items);
    obj["count"] = jsonx::num(static_cast<long long>(items.size()));
    http::json(res, 200, JsonValue(obj));
}

void sessionRevoke(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string target = util::trim(http::param(req, "id"));
    if (target.empty()) {
        http::error(res, 400, "invalid_session", "无效的登录设备");
        return;
    }
    auto currentId = authx::sessionId(req);
    for (const auto& name : Cache::scan("session:*", 500)) {
        std::string raw = Cache::get(name);
        if (raw.empty()) continue;
        SessionInfo info;
        if (!parseSession(name, raw, info) || info.userId != user.id) continue;
        if (publicSessionId(info.id) != target) continue;
        if (currentId && info.id == *currentId) {
            http::error(res, 400, "current_session", "不能在这里退出当前设备，请使用退出登录");
            return;
        }
        deleteSession(info.id);
        applog::access("[session] user=" + user.id + " revoked session_hash=" +
                       publicSessionId(info.id).substr(0, 12));
        std::map<std::string, JsonValue> obj;
        obj["ok"] = JsonValue(true);
        http::ok(res, JsonValue(obj));
        return;
    }
    http::error(res, 404, "not_found", "该设备已退出或不存在");
}

void sessionRevokeOthers(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto currentId = authx::sessionId(req);
    long long revoked = 0;
    for (const auto& name : Cache::scan("session:*", 500)) {
        std::string raw = Cache::get(name);
        if (raw.empty()) continue;
        SessionInfo info;
        if (!parseSession(name, raw, info) || info.userId != user.id) continue;
        if (currentId && info.id == *currentId) continue;
        deleteSession(info.id);
        ++revoked;
    }
    applog::access("[session] user=" + user.id + " revoked_others=" + std::to_string(revoked));
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["revoked"] = jsonx::num(revoked);
    http::ok(res, JsonValue(obj));
}

}  // namespace handlers

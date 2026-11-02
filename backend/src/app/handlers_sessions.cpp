#include "core.h"
#include "app/logger.h"

#include <algorithm>
#include <set>

namespace {

struct SessionInfo {
    std::string id;
    std::string userId;
    std::string username;
    std::string userAgent;
    std::string ip;
    std::string deviceHash;
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
    out.deviceHash = jsonx::getString(data, "device_hash");
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

bool truthy(const std::string& value) {
    std::string lowered = util::lower(util::trim(value));
    return lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on";
}

// When trust is enabled from the profile page, attach the device hash to the
// already-created current session so per-device revoke can map it later.
void attachDeviceHash(const Request& req, const std::string& deviceHash) {
    if (deviceHash.empty()) return;
    auto sid = authx::sessionId(req);
    if (!sid) return;
    std::string raw = Cache::get("session:" + *sid);
    if (raw.empty()) return;
    std::map<std::string, JsonValue> data;
    if (!jsonx::parseObject(raw, data)) return;
    data["device_hash"] = JsonValue(deviceHash);
    long long ttl = Cache::ttl("session:" + *sid);
    if (ttl <= 0) ttl = AppConfig::get().sessionHours * 3600;
    Cache::setJson("session:" + *sid, jsonx::dump(data), static_cast<int>(ttl));
}

// Older sessions were created before device_hash was stored. Devices are
// identified by IP (UA alone was too coarse), so match the trusted device
// whose recorded IP — current or in its known-IP book — equals the session's
// login IP, created within ±120s. Give up if two different devices match.
std::string resolveDeviceHash(const std::string& userId, const SessionInfo& info) {
    if (!info.deviceHash.empty()) return info.deviceHash;
    if (info.ip.empty() || info.createdAt.empty()) return "";
    std::string ts = info.createdAt;
    std::replace(ts.begin(), ts.end(), 'T', ' ');
    if (!ts.empty() && ts.back() == 'Z') ts.pop_back();
    auto rows = Db::query(
        "SELECT token_hash, ip, known_ips FROM trusted_devices WHERE user_id=" +
        Db::quote(userId) +
        " AND ABS(TIMESTAMPDIFF(SECOND, created_at, " + Db::quote(ts) + ")) <= 120 LIMIT 20");
    std::string match;
    for (const auto& row : rows) {
        std::string storedIp = row.count("ip") ? row.at("ip") : "";
        bool hit = !storedIp.empty() && http::sameIp(storedIp, info.ip);
        if (!hit) {
            JsonValue known;
            try { known.fromJson(row.count("known_ips") ? row.at("known_ips") : ""); }
            catch (...) {}
            if (known.getType() == JsonValue::ARRAY) {
                for (const auto& item : known.asArray()) {
                    if (item.getType() == JsonValue::STRING &&
                        http::sameIp(item.asString(), info.ip)) {
                        hit = true;
                        break;
                    }
                }
            }
        }
        if (!hit) continue;
        const std::string& hash = row.at("token_hash");
        if (match.empty()) match = hash;
        else if (match != hash) return "";
    }
    return match;
}

std::string currentDeviceHashForUser(const std::string& userId,
                                     const std::string& currentId) {
    if (currentId.empty()) return "";
    for (const auto& name : Cache::scan("session:*", 500)) {
        std::string raw = Cache::get(name);
        if (raw.empty()) continue;
        SessionInfo info;
        if (!parseSession(name, raw, info)) continue;
        if (info.userId == userId && info.id == currentId) {
            return resolveDeviceHash(userId, info);
        }
    }
    return "";
}

// Delete the trusted-device row only after the last session for that device
// is gone. A shared browser with two sessions keeps trust until both leave.
void revokeTrustIfUnused(const std::string& userId, const std::string& deviceHash,
                         const std::string& currentDeviceHash) {
    if (deviceHash.empty() || deviceHash == currentDeviceHash) return;
    for (const auto& name : Cache::scan("session:*", 500)) {
        std::string raw = Cache::get(name);
        if (raw.empty()) continue;
        SessionInfo info;
        if (!parseSession(name, raw, info)) continue;
        if (info.userId == userId && info.deviceHash == deviceHash) return;
    }
    int deleted = Db::exec("DELETE FROM trusted_devices WHERE user_id=" + Db::quote(userId) +
                           " AND token_hash=" + Db::quote(deviceHash));
    if (deleted > 0) {
        applog::access("[session] user=" + userId + " removed device trust hash=" +
                       deviceHash.substr(0, 12));
    }
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
    std::string currentHash = currentId ? currentDeviceHashForUser(user.id, *currentId) : "";
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
        std::string deviceHash = resolveDeviceHash(user.id, info);
        deleteSession(info.id);
        revokeTrustIfUnused(user.id, deviceHash, currentHash);
        applog::access("[session] user=" + user.id + " revoked session_hash=" +
                       publicSessionId(info.id).substr(0, 12) +
                       (deviceHash.empty() ? "" : " with_device_trust"));
        std::map<std::string, JsonValue> obj;
        obj["ok"] = JsonValue(true);
        http::ok(res, JsonValue(obj));
        return;
    }
    http::error(res, 404, "not_found", "该设备已退出或不存在");
}

void deviceTrustStatus(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto& cfg = AppConfig::get();
    bool trusted = false;
    std::string lastSeen;
    std::string raw = http::cookie(req, "unisso_device");
    if (!raw.empty()) {
        auto rows = Db::query(
            "SELECT UNIX_TIMESTAMP(last_seen_at) AS ts FROM trusted_devices "
            "WHERE user_id=" + Db::quote(user.id) + " AND token_hash=" +
            Db::quote(authx::deviceHash(raw)) +
            " AND last_seen_at >= NOW() - INTERVAL " +
            std::to_string(cfg.trustDeviceDays) + " DAY LIMIT 1");
        if (!rows.empty()) {
            trusted = true;
            try {
                lastSeen = util::formatTs(std::stoll(rows.front().at("ts")));
            } catch (...) {
                lastSeen = "";
            }
        }
    }
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["trusted"] = JsonValue(trusted);
    obj["days"] = jsonx::num(cfg.trustDeviceDays);
    obj["last_seen_at"] = JsonValue(lastSeen);
    http::json(res, 200, JsonValue(obj));
}

void deviceTrustSet(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string raw = util::trim(http::input(req, "trusted"));
    if (raw.empty()) {
        http::error(res, 400, "invalid_request", "缺少设备信任参数");
        return;
    }
    bool trusted = truthy(raw);
    if (trusted) {
        std::string deviceHash = authx::trustDevice(user.id, req, res);
        attachDeviceHash(req, deviceHash);
    } else {
        authx::untrustDevice(user.id, req);
    }
    applog::access("[device] user=" + user.id +
                   (trusted ? " trusted current device" : " removed current device trust"));
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["trusted"] = JsonValue(trusted);
    obj["days"] = jsonx::num(AppConfig::get().trustDeviceDays);
    http::json(res, 200, JsonValue(obj));
}

void sessionRevokeOthers(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto currentId = authx::sessionId(req);
    std::string currentHash = currentId ? currentDeviceHashForUser(user.id, *currentId) : "";
    std::set<std::string> deviceHashes;
    long long revoked = 0;
    for (const auto& name : Cache::scan("session:*", 500)) {
        std::string raw = Cache::get(name);
        if (raw.empty()) continue;
        SessionInfo info;
        if (!parseSession(name, raw, info) || info.userId != user.id) continue;
        if (currentId && info.id == *currentId) continue;
        std::string deviceHash = resolveDeviceHash(user.id, info);
        if (!deviceHash.empty()) deviceHashes.insert(deviceHash);
        deleteSession(info.id);
        ++revoked;
    }
    for (const auto& hash : deviceHashes) {
        revokeTrustIfUnused(user.id, hash, currentHash);
    }
    applog::access("[session] user=" + user.id + " revoked_others=" + std::to_string(revoked));
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["revoked"] = jsonx::num(revoked);
    http::ok(res, JsonValue(obj));
}

}  // namespace handlers

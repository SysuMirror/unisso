#include "core.h"
#include "app/logger.h"

#include <algorithm>
#include <atomic>
#include <random>

namespace {

constexpr int kLiveSessionSeconds = 120;
constexpr int kBindTicketSeconds = 300;
constexpr size_t kFrameMinBytes = 1024;
constexpr size_t kFrameMaxBytes = 10 * 1024 * 1024;

std::string codeHash(const std::string& code) {
    auto& cfg = AppConfig::get();
    std::string key = cfg.secretKey.empty() ? cfg.signingKeyId : cfg.secretKey;
    return util::hex(util::hmacSha256Raw(key, code));
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
    if (at == std::string::npos || at == 0) return "***";
    return email.substr(0, 1) + "***" + email.substr(at);
}

bool isImage(const std::string& data) {
    if (data.size() < 12) return false;
    if (static_cast<unsigned char>(data[0]) == 0xff &&
        static_cast<unsigned char>(data[1]) == 0xd8) return true;  // JPEG
    if (data.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0) return true;
    if (data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0) return true;
    return false;
}

bool collectFrames(const Request& req, std::vector<std::string>& frames,
                   std::string& error) {
    for (const auto& file : req.uploadedFileList) {
        if (file.name != "frames") continue;
        if (file.data.size() < kFrameMinBytes) {
            error = "invalid_frame";
            return false;
        }
        if (file.data.size() > kFrameMaxBytes) {
            error = "frame_too_large";
            return false;
        }
        if (!isImage(file.data)) {
            error = "invalid_frame";
            return false;
        }
        frames.push_back(file.data);
    }
    if (frames.size() < 3) {
        error = "too_few_frames";
        return false;
    }
    if (frames.size() > 8) {
        error = "too_many_frames";
        return false;
    }
    return true;
}

std::string liveKey(const std::string& token) {
    return "face_live:" + token;
}

std::string bindKey(const std::string& token) {
    return "face_bind:" + token;
}

std::string bindCodeKey(const std::string& token) {
    return "face_bind_code:" + token;
}

bool loadLive(const Request& req, const std::string& token, const std::string& userId,
              const std::string& purpose, std::map<std::string, JsonValue>& data) {
    if (token.empty()) return false;
    std::string raw = Cache::get(liveKey(token));
    if (raw.empty() || !jsonx::parseObject(raw, data)) return false;
    if (jsonx::getString(data, "user_id") != userId) return false;
    if (jsonx::getString(data, "purpose") != purpose) return false;
    long long expiresAt = 0;
    auto it = data.find("expires_at");
    if (it != data.end() && it->second.getType() == JsonValue::NUMBER) {
        expiresAt = static_cast<long long>(it->second.asNumber());
    }
    return expiresAt > util::nowEpoch();
}

void audit(const User& user, const std::string& event, const std::string& personId,
           double score, const std::string& ip) {
    Db::exec("INSERT INTO face_audits (id,user_id,event,provider_person_id,score,ip,created_at) "
             "VALUES (" + Db::quote(util::uuid4()) + "," + Db::quote(user.id) + "," +
             Db::quote(event) + "," + Db::nullable(personId) + "," +
             (score > 0 ? Db::quote(std::to_string(score)) : "NULL") + "," +
             Db::nullable(ip) + ",NOW())");
}

void faceFailure(Response& res, const std::string& error) {
    if (error == "invalid_frame" || error == "frame_too_large") {
        http::error(res, 400, "invalid_frame", "图片帧无效或过大，请重新采集");
    } else if (error == "no_face") {
        http::error(res, 400, "no_face", "未检测到人脸，请正对镜头、光线充足后重试");
    } else if (error == "multiple_faces") {
        http::error(res, 400, "multiple_faces", "画面中有多张人脸，请只保留一个人");
    } else if (error == "challenge_not_completed") {
        http::error(res, 400, "challenge_not_completed", "没有完成指定动作，请按提示重试");
    } else if (error == "spoof_suspected") {
        http::error(res, 403, "spoof_suspected",
                    "活体防伪未通过（可能光线、反光或镜头质量），请面向光源后用本人实时人脸重试");
    } else if (error == "identity_mismatch") {
        http::error(res, 403, "identity_mismatch", "画面中的人脸前后不一致，请重试");
    } else if (error == "too_few_frames") {
        http::error(res, 400, "too_few_frames", "采集帧数不足，请按提示重新采集");
    } else if (error == "too_many_frames") {
        http::error(res, 400, "too_many_frames", "采集帧数过多，请重新采集");
    } else if (error == "session_expired" || error == "missing_session") {
        http::error(res, 401, "session_expired", "活体会话已过期，请重新开始");
    } else if (error == "conflict") {
        http::error(res, 409, "session_used", "活体会话已使用，请重新开始");
    } else if (error == "overloaded") {
        res.headers["Retry-After"] = "3";
        http::error(res, 503, "overloaded", "人脸服务繁忙，请稍后再试");
    } else if (error == "rate_limited") {
        res.headers["Retry-After"] = "2";
        http::error(res, 429, "rate_limited", "人脸服务繁忙，请稍后再试");
    } else if (error == "face_not_configured" || error == "face_service_unavailable") {
        http::error(res, 503, "face_service_unavailable", "人脸服务暂不可用，请使用邮箱验证码");
    } else {
        http::error(res, 502, "face_failed", "人脸验证失败，请重试或使用邮箱验证码");
    }
}

std::optional<std::map<std::string, std::string>> facePersonByProvider(
    const std::string& providerPersonId) {
    auto rows = Db::query("SELECT id,provider_person_id,external_id FROM face_persons "
                          "WHERE provider_person_id=" + Db::quote(providerPersonId) + " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    return rows.front();
}

bool userHasFace(const std::string& userId, const std::string& facePersonId) {
    auto rows = Db::query("SELECT id FROM user_faces WHERE user_id=" + Db::quote(userId) +
                          " AND face_person_id=" + Db::quote(facePersonId) + " LIMIT 1");
    return !rows.empty();
}

JsonValue faceRowJson(const std::map<std::string, std::string>& row) {
    std::map<std::string, JsonValue> obj;
    obj["id"] = JsonValue(row.at("id"));
    obj["created_at"] = JsonValue(row.count("created_at") ? row.at("created_at") : "");
    return JsonValue(obj);
}

std::optional<std::map<std::string, JsonValue>> loadStepUp(const Request& req,
                                                           Response& res) {
    std::string token = util::trim(http::input(req, "token"));
    std::string cookie = http::cookie(req, "unisso_stepup");
    if (token.empty()) token = cookie;
    if (token.empty() || cookie.empty() || token != cookie) {
        http::error(res, 403, "step_up_required", "二次验证会话已失效，请重新登录");
        return std::nullopt;
    }
    std::string raw = Cache::get("step_up:" + token);
    std::map<std::string, JsonValue> data;
    if (raw.empty() || !jsonx::parseObject(raw, data)) {
        http::error(res, 401, "step_up_expired", "二次验证已过期，请重新登录");
        return std::nullopt;
    }
    long long expiresAt = 0;
    auto it = data.find("expires_at");
    if (it != data.end() && it->second.getType() == JsonValue::NUMBER) {
        expiresAt = static_cast<long long>(it->second.asNumber());
    }
    if (expiresAt <= util::nowEpoch()) {
        http::error(res, 401, "step_up_expired", "二次验证已过期，请重新登录");
        return std::nullopt;
    }
    data["token"] = JsonValue(token);
    return data;
}

}  // namespace

namespace handlers {

void faceStatus(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto& cfg = AppConfig::get();
    std::map<std::string, JsonValue> obj;
    obj["enabled"] = JsonValue(cfg.faceEnabled);
    obj["face_count"] = jsonx::num(repo::faceCount(user.id));
    obj["login_threshold"] = JsonValue(cfg.faceLoginThreshold);
    obj["bind_threshold"] = JsonValue(cfg.faceBindThreshold);
    http::json(res, 200, JsonValue(obj));
}

void facesList(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto rows = Db::query(
        "SELECT uf.id,uf.created_at FROM user_faces uf "
        "WHERE uf.user_id=" + Db::quote(user.id) + " ORDER BY uf.created_at");
    std::vector<JsonValue> items;
    for (const auto& row : rows) items.push_back(faceRowJson(row));
    std::map<std::string, JsonValue> obj;
    obj["enabled"] = JsonValue(AppConfig::get().faceEnabled);
    obj["faces"] = JsonValue(items);
    http::json(res, 200, JsonValue(obj));
}

void faceSession(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto& cfg = AppConfig::get();
    if (!cfg.faceEnabled) {
        http::error(res, 503, "face_service_unavailable", "人脸服务尚未配置");
        return;
    }
    std::string purpose = util::trim(http::input(req, "purpose"));
    if (purpose.empty()) purpose = "probe";
    if (purpose != "probe" && purpose != "enroll") {
        http::error(res, 400, "invalid_purpose", "无效的采集目的");
        return;
    }
    long long creates = Cache::incr("face_session:" + user.id, 60);
    if (creates > 20) {
        http::error(res, 429, "rate_limited", "采集过于频繁，请稍后再试");
        return;
    }
    face::LivenessSession upstream;
    std::string error;
    if (!face::createLivenessSession(upstream, error)) {
        faceFailure(res, error);
        return;
    }
    std::string token = util::randomToken(24);
    std::map<std::string, JsonValue> data;
    data["upstream_session_id"] = JsonValue(upstream.sessionId);
    data["user_id"] = JsonValue(user.id);
    data["purpose"] = JsonValue(purpose);
    data["challenge"] = JsonValue(upstream.challenge);
    data["expires_at"] = jsonx::num(util::nowEpoch() + upstream.expiresIn);
    Cache::setJson(liveKey(token), jsonx::dump(data), upstream.expiresIn);

    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["session_token"] = JsonValue(token);
    obj["challenge"] = JsonValue(upstream.challenge);
    obj["expires_in"] = jsonx::num(upstream.expiresIn);
    obj["min_frames"] = jsonx::num(upstream.minFrames);
    obj["max_frames"] = jsonx::num(upstream.maxFrames);
    http::json(res, 200, JsonValue(obj));
}

void faceProbe(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto& cfg = AppConfig::get();
    if (!cfg.faceEnabled) {
        http::error(res, 503, "face_service_unavailable", "人脸服务尚未配置");
        return;
    }
    std::string token = util::trim(http::input(req, "session_token"));
    std::map<std::string, JsonValue> local;
    if (!loadLive(req, token, user.id, "probe", local)) {
        http::error(res, 401, "session_expired", "采集会话已过期，请重新开始");
        return;
    }
    std::vector<std::string> frames;
    std::string error;
    if (!collectFrames(req, frames, error)) {
        faceFailure(res, error);
        return;
    }
    std::string upstream = jsonx::getString(local, "upstream_session_id");
    Cache::del(liveKey(token));

    face::LiveResult result;
    if (!face::recognizeLive(upstream, frames, cfg.faceBindThreshold, result, error)) {
        audit(user, "probe_failed", "", 0, http::clientIp(req));
        faceFailure(res, error);
        return;
    }
    if (!result.passed) {
        audit(user, "probe_liveness_failed", result.match.providerPersonId, result.match.score,
              http::clientIp(req));
        http::error(res, 400, "liveness_failed", "活体检测未通过，请按提示重试");
        return;
    }
    if (result.match.providerPersonId.empty()) {
        audit(user, "probe_no_match", "", 0, http::clientIp(req));
        std::map<std::string, JsonValue> obj;
        obj["ok"] = JsonValue(true);
        obj["matched"] = JsonValue(false);
        http::json(res, 200, JsonValue(obj));
        return;
    }

    std::string provider = result.match.providerPersonId;
    auto localPerson = facePersonByProvider(provider);
    if (localPerson && userHasFace(user.id, localPerson->at("id"))) {
        audit(user, "probe_already_linked", provider, result.match.score, http::clientIp(req));
        std::map<std::string, JsonValue> obj;
        obj["ok"] = JsonValue(true);
        obj["matched"] = JsonValue(true);
        obj["already_linked"] = JsonValue(true);
        http::json(res, 200, JsonValue(obj));
        return;
    }

    std::string ticket = util::randomToken(24);
    std::map<std::string, JsonValue> pending;
    pending["user_id"] = JsonValue(user.id);
    pending["provider_person_id"] = JsonValue(provider);
    pending["external_id"] = JsonValue(result.match.externalId);
    pending["score"] = JsonValue(result.match.score);
    pending["expires_at"] = jsonx::num(util::nowEpoch() + kBindTicketSeconds);
    Cache::setJson(bindKey(ticket), jsonx::dump(pending), kBindTicketSeconds);
    audit(user, "probe_matched", provider, result.match.score, http::clientIp(req));

    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["matched"] = JsonValue(true);
    obj["bind_ticket"] = JsonValue(ticket);
    obj["email_hint"] = JsonValue(maskEmail(user.email));
    obj["expires_in"] = jsonx::num(kBindTicketSeconds);
    http::json(res, 200, JsonValue(obj));
}

void faceSendCode(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    if (!AppConfig::get().emailAvailable()) {
        http::error(res, 503, "email_unavailable", "邮箱验证暂不可用");
        return;
    }
    std::string ticket = util::trim(http::input(req, "bind_ticket"));
    std::string raw = Cache::get(bindKey(ticket));
    std::map<std::string, JsonValue> pending;
    if (raw.empty() || !jsonx::parseObject(raw, pending) ||
        jsonx::getString(pending, "user_id") != user.id) {
        http::error(res, 401, "bind_ticket_expired", "绑定请求已过期，请重新识别人脸");
        return;
    }
    long long sends = Cache::incr("face_bind_send:" + ticket, kBindTicketSeconds);
    if (sends > 5) {
        http::error(res, 429, "rate_limited", "验证码发送次数过多");
        return;
    }
    long long cooldown = Cache::incr("face_bind_cooldown:" + ticket, 60);
    if (cooldown > 1) {
        http::error(res, 429, "rate_limited", "验证码发送过于频繁，请稍后再试");
        return;
    }
    std::string code = randomCode();
    std::map<std::string, JsonValue> record;
    record["code_hash"] = JsonValue(codeHash(code));
    record["expires_at"] = jsonx::num(util::nowEpoch() + kBindTicketSeconds);
    Cache::setJson(bindCodeKey(ticket), jsonx::dump(record), kBindTicketSeconds);

    std::string html =
        "<div style=\"font-family:-apple-system,Segoe UI,PingFang SC,sans-serif;color:#171717\">"
        "<h2 style=\"font-size:20px;margin:0 0 12px\">人脸关联确认</h2>"
        "<p style=\"color:#5f6368;font-size:14px\">识别到一张已登记的人脸。确认关联到当前账号的验证码：</p>"
        "<p style=\"font-size:30px;font-weight:700;letter-spacing:6px;margin:16px 0\">" + code + "</p>"
        "<p style=\"color:#5f6368;font-size:13px\">验证码 5 分钟内有效。如果不是你本人操作，请立即修改密码。</p>"
        "</div>";
    std::string mailError;
    if (!mail::send(user.email, "UniSSO 人脸关联确认", html, mailError)) {
        Cache::del(bindCodeKey(ticket));
        std::cerr << "[face] mail failed: " << mailError << std::endl;
        http::error(res, 503, "email_unavailable", "验证码发送失败，请稍后重试");
        return;
    }
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["email_hint"] = JsonValue(maskEmail(user.email));
    http::ok(res, JsonValue(obj));
}

void faceConfirm(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string ticket = util::trim(http::input(req, "bind_ticket"));
    std::string code = util::trim(http::input(req, "code"));
    std::string raw = Cache::get(bindKey(ticket));
    std::map<std::string, JsonValue> pending;
    if (raw.empty() || !jsonx::parseObject(raw, pending) ||
        jsonx::getString(pending, "user_id") != user.id) {
        http::error(res, 401, "bind_ticket_expired", "绑定请求已过期，请重新识别人脸");
        return;
    }
    long long attempts = Cache::incr("face_bind_attempts:" + ticket, kBindTicketSeconds);
    if (attempts > 5) {
        Cache::del(bindKey(ticket));
        Cache::del(bindCodeKey(ticket));
        http::error(res, 429, "too_many_attempts", "验证失败次数过多，请重新识别人脸");
        return;
    }
    std::string codeRaw = Cache::get(bindCodeKey(ticket));
    std::map<std::string, JsonValue> record;
    if (codeRaw.empty() || !jsonx::parseObject(codeRaw, record)) {
        http::error(res, 401, "invalid_code", "验证码不正确或已过期");
        return;
    }
    std::string expected = jsonx::getString(record, "code_hash");
    long long expiresAt = 0;
    auto exp = record.find("expires_at");
    if (exp != record.end() && exp->second.getType() == JsonValue::NUMBER) {
        expiresAt = static_cast<long long>(exp->second.asNumber());
    }
    if (expiresAt <= util::nowEpoch() || expected.empty() ||
        !util::constantTimeEquals(expected, codeHash(code))) {
        http::error(res, 401, "invalid_code", "验证码不正确或已过期");
        return;
    }

    std::string provider = jsonx::getString(pending, "provider_person_id");
    std::string external = jsonx::getString(pending, "external_id");
    if (provider.empty()) {
        http::error(res, 400, "invalid_ticket", "绑定信息不完整");
        return;
    }
    auto existing = facePersonByProvider(provider);
    std::string facePersonId;
    if (existing) {
        facePersonId = existing->at("id");
    } else {
        facePersonId = util::uuid4();
        Db::exec("INSERT INTO face_persons (id,provider_person_id,external_id,created_by,created_at) "
                 "VALUES (" + Db::quote(facePersonId) + "," + Db::quote(provider) + "," +
                 Db::quote(external.empty() ? ("unisso_face_" + facePersonId) : external) + "," +
                 Db::quote(user.id) + ",NOW())");
    }
    if (!userHasFace(user.id, facePersonId)) {
        Db::exec("INSERT IGNORE INTO user_faces (id,user_id,face_person_id,created_at) VALUES (" +
                 Db::quote(util::uuid4()) + "," + Db::quote(user.id) + "," +
                 Db::quote(facePersonId) + ",NOW())");
    }
    double score = 0;
    auto scoreIt = pending.find("score");
    if (scoreIt != pending.end() && scoreIt->second.getType() == JsonValue::NUMBER) {
        score = scoreIt->second.asNumber();
    }
    audit(user, "bind", provider, score, http::clientIp(req));
    Cache::del(bindKey(ticket));
    Cache::del(bindCodeKey(ticket));
    Cache::del("face_bind_attempts:" + ticket);
    Cache::del("face_bind_send:" + ticket);
    Cache::del("face_bind_cooldown:" + ticket);
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["face_count"] = jsonx::num(repo::faceCount(user.id));
    http::ok(res, JsonValue(obj));
}

void faceEnroll(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    auto& cfg = AppConfig::get();
    if (!cfg.faceEnabled) {
        http::error(res, 503, "face_service_unavailable", "人脸服务尚未配置");
        return;
    }
    std::string token = util::trim(http::input(req, "session_token"));
    std::map<std::string, JsonValue> local;
    if (!loadLive(req, token, user.id, "enroll", local)) {
        http::error(res, 401, "session_expired", "采集会话已过期，请重新开始");
        return;
    }
    std::vector<std::string> frames;
    std::string error;
    if (!collectFrames(req, frames, error)) {
        faceFailure(res, error);
        return;
    }
    std::string upstream = jsonx::getString(local, "upstream_session_id");
    Cache::del(liveKey(token));

    std::string external = "unisso_face_" + util::randomHex(16);
    std::string provider;
    if (!face::enrollLive(upstream, external, "UniSSO face", frames, provider, error)) {
        audit(user, "enroll_failed", "", 0, http::clientIp(req));
        faceFailure(res, error);
        return;
    }

    std::string facePersonId = util::uuid4();
    if (Db::exec("INSERT INTO face_persons (id,provider_person_id,external_id,created_by,created_at) "
                 "VALUES (" + Db::quote(facePersonId) + "," + Db::quote(provider) + "," +
                 Db::quote(external) + "," + Db::quote(user.id) + ",NOW())") < 0) {
        std::string deleteError;
        face::deletePerson(provider, deleteError);
        http::error(res, 500, "store_failed", "人脸登记失败，请稍后重试");
        return;
    }
    Db::exec("INSERT IGNORE INTO user_faces (id,user_id,face_person_id,created_at) VALUES (" +
             Db::quote(util::uuid4()) + "," + Db::quote(user.id) + "," +
             Db::quote(facePersonId) + ",NOW())");
    audit(user, "enroll", provider, 0, http::clientIp(req));
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["face_count"] = jsonx::num(repo::faceCount(user.id));
    http::ok(res, JsonValue(obj));
}

void faceUnbind(const Request& req, Response& res) {
    User user;
    if (!authx::requireUser(req, res, user)) return;
    std::string id = http::param(req, "id");
    auto rows = Db::query(
        "SELECT uf.face_person_id,fp.provider_person_id,fp.external_id FROM user_faces uf "
        "JOIN face_persons fp ON fp.id=uf.face_person_id "
        "WHERE uf.id=" + Db::quote(id) + " AND uf.user_id=" + Db::quote(user.id) + " LIMIT 1");
    if (rows.empty()) {
        http::error(res, 404, "not_found", "人脸记录不存在");
        return;
    }
    std::string facePersonId = rows.front().at("face_person_id");
    std::string provider = rows.front().at("provider_person_id");
    std::string external = rows.front().count("external_id") ? rows.front().at("external_id") : "";
    auto countRows = Db::query("SELECT COUNT(*) AS c FROM user_faces WHERE face_person_id=" +
                               Db::quote(facePersonId));
    long long count = 0;
    if (!countRows.empty()) {
        try { count = std::stoll(countRows.front().at("c")); } catch (...) {}
    }
    if (count <= 1) {
        // Last link: remove the face from the upstream service as well, so
        // unbinding does not leave an orphan feature record there. Prefer the
        // idempotent external_id endpoint (200 + deleted=false if already
        // gone), then fall back to person_id for older face-service builds.
        if (external.empty() && provider.empty()) {
            applog::error("[face] unbind missing upstream id face_person=" + facePersonId);
            http::error(res, 500, "face_person_missing",
                        "人脸记录不完整，请稍后重试或联系管理员");
            return;
        }
        std::string error;
        bool deleted = false;
        std::string used;
        if (!external.empty() &&
            face::deletePersonByExternalId(external, error)) {
            deleted = true;
            used = "external_id=" + external;
        }
        if (!deleted && !provider.empty() &&
            face::deletePerson(provider, error)) {
            deleted = true;
            used = "person=" + provider;
        }
        if (!deleted) {
            faceFailure(res, error);
            return;
        }
        applog::access("[face] unbind deleted upstream " + used);
    } else {
        applog::access("[face] unbind kept shared upstream external_id=" + external +
                       " remaining_links=" + std::to_string(count - 1));
    }
    Db::exec("DELETE FROM user_faces WHERE id=" + Db::quote(id) + " AND user_id=" +
             Db::quote(user.id));
    if (count <= 1) {
        Db::exec("DELETE FROM face_persons WHERE id=" + Db::quote(facePersonId));
    }
    audit(user, "unbind", provider, 0, http::clientIp(req));
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["face_count"] = jsonx::num(repo::faceCount(user.id));
    http::ok(res, JsonValue(obj));
}

void stepUpFaceSession(const Request& req, Response& res) {
    auto challenge = loadStepUp(req, res);
    if (!challenge) return;
    auto& cfg = AppConfig::get();
    std::string userId = jsonx::getString(*challenge, "user_id");
    if (!cfg.faceEnabled || repo::faceCount(userId) == 0) {
        http::error(res, 403, "face_not_available", "当前账号没有可用的人脸验证方式");
        return;
    }
    long long sessions = Cache::incr("step_up_face_sessions:" + jsonx::getString(*challenge, "token"), 60);
    if (sessions > 10) {
        http::error(res, 429, "rate_limited", "人脸验证请求过于频繁，请稍后再试");
        return;
    }
    face::LivenessSession upstream;
    std::string error;
    if (!face::createLivenessSession(upstream, error)) {
        faceFailure(res, error);
        return;
    }
    std::string token = util::randomToken(24);
    std::map<std::string, JsonValue> data;
    data["upstream_session_id"] = JsonValue(upstream.sessionId);
    data["user_id"] = JsonValue(userId);
    data["purpose"] = JsonValue("stepup");
    data["step_up_token"] = JsonValue(jsonx::getString(*challenge, "token"));
    data["challenge"] = JsonValue(upstream.challenge);
    data["expires_at"] = jsonx::num(util::nowEpoch() + upstream.expiresIn);
    Cache::setJson(liveKey(token), jsonx::dump(data), upstream.expiresIn);

    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["session_token"] = JsonValue(token);
    obj["challenge"] = JsonValue(upstream.challenge);
    obj["expires_in"] = jsonx::num(upstream.expiresIn);
    obj["min_frames"] = jsonx::num(upstream.minFrames);
    obj["max_frames"] = jsonx::num(upstream.maxFrames);
    http::json(res, 200, JsonValue(obj));
}

void stepUpFaceVerify(const Request& req, Response& res) {
    auto challenge = loadStepUp(req, res);
    if (!challenge) return;
    auto& cfg = AppConfig::get();
    std::string userId = jsonx::getString(*challenge, "user_id");
    std::string stepToken = jsonx::getString(*challenge, "token");
    long long attempts = Cache::incr("step_up_face_attempts:" + stepToken, 900);
    if (attempts > 5) {
        http::error(res, 429, "too_many_attempts", "人脸验证失败次数过多，请改用邮箱验证码");
        return;
    }
    std::string token = util::trim(http::input(req, "session_token"));
    std::map<std::string, JsonValue> local;
    if (!loadLive(req, token, userId, "stepup", local) ||
        jsonx::getString(local, "step_up_token") != stepToken) {
        http::error(res, 401, "session_expired", "人脸采集会话已过期，请重新开始");
        return;
    }
    std::vector<std::string> frames;
    std::string error;
    if (!collectFrames(req, frames, error)) {
        faceFailure(res, error);
        return;
    }
    std::string upstream = jsonx::getString(local, "upstream_session_id");
    Cache::del(liveKey(token));

    auto user = repo::findUserById(userId);
    if (!user || !user->isActive) {
        http::error(res, 401, "invalid_account", "账号不可用");
        return;
    }
    face::LiveResult result;
    if (!face::recognizeLive(upstream, frames, cfg.faceLoginThreshold, result, error)) {
        audit(*user, "stepup_face_failed", "", 0, http::clientIp(req));
        faceFailure(res, error);
        return;
    }
    if (!result.passed) {
        audit(*user, "stepup_face_liveness_failed", result.match.providerPersonId,
              result.match.score, http::clientIp(req));
        http::error(res, 400, "liveness_failed", "活体检测未通过，请重试或使用邮箱验证码");
        return;
    }
    // A face can have several person records (e.g. re-enrollment). Accept the
    // best candidate that is actually linked to this account.
    if (result.candidates.empty() && !result.match.providerPersonId.empty()) {
        result.candidates.push_back(result.match);
    }
    std::string provider;
    double bestScore = 0;
    for (const auto& candidate : result.candidates) {
        if (candidate.score < cfg.faceLoginThreshold) continue;
        auto local = facePersonByProvider(candidate.providerPersonId);
        if (!local || !userHasFace(userId, local->at("id"))) continue;
        if (candidate.score > bestScore) {
            bestScore = candidate.score;
            provider = candidate.providerPersonId;
        }
    }
    if (provider.empty()) {
        audit(*user, "stepup_face_mismatch", result.match.providerPersonId,
              result.match.score, http::clientIp(req));
        http::error(res, 403, "identity_mismatch", "人脸与当前账号不匹配，请使用邮箱验证码");
        return;
    }

    Cache::del("step_up:" + stepToken);
    Cache::del("step_up_code:" + stepToken);
    Cache::del("step_up_attempts:" + stepToken);
    Cache::del("step_up_send_total:" + stepToken);
    Cache::del("step_up_send_cooldown:" + stepToken);
    Cache::del("step_up_face_attempts:" + stepToken);
    http::clearCookie(res, req, "unisso_stepup");
    authx::cancelDeviceRequests(stepToken);
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
    audit(*user, "stepup_face_ok", provider, bestScore, http::clientIp(req));

    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(true);
    obj["user"] = repo::userJson(fresh ? *fresh : *user, true);
    obj["next"] = JsonValue(jsonx::getString(*challenge, "next", "/"));
    http::ok(res, JsonValue(obj));
}

}  // namespace handlers

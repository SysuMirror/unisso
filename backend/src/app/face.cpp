#include "core.h"
#include "app/logger.h"

#include <curl/curl.h>

#include <mutex>
#include <sstream>

namespace {

struct HttpResponse {
    long status = 0;
    std::string body;
};

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::once_flag curlOnce;

std::string baseUrl() {
    std::string base = AppConfig::get().faceBaseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

std::string apiError(const JsonValue& root, const std::string& fallback) {
    if (root.getType() == JsonValue::OBJECT) {
        auto obj = root.asObject();
        auto err = obj.find("error");
        if (err != obj.end() && err->second.getType() == JsonValue::OBJECT) {
            auto e = err->second.asObject();
            std::string code = jsonx::getString(e, "code");
            if (!code.empty()) return code;
            std::string message = jsonx::getString(e, "message");
            if (!message.empty()) return message;
        }
    }
    return fallback;
}

bool perform(CURL* curl, HttpResponse& response, std::string& error) {
    char errBuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errBuf);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     static_cast<long>(AppConfig::get().faceTimeoutSeconds));
    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        error = "face_service_unavailable";
        applog::error(std::string("[face] transport error: ") +
                      (errBuf[0] ? errBuf : curl_easy_strerror(code)));
        return false;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    return true;
}

CURL* makeCurl(struct curl_slist*& headers) {
    auto& cfg = AppConfig::get();
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;
    headers = curl_slist_append(headers, ("X-API-Key: " + cfg.faceApiKey).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: UniSSO/2.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    return curl;
}

bool parseJson(const std::string& body, JsonValue& root, std::string& error) {
    try {
        root.fromJson(body);
        return true;
    } catch (const std::exception&) {
        error = "invalid_face_response";
        return false;
    }
}

void parseMatch(const JsonValue& value, face::Match& out) {
    if (value.getType() != JsonValue::OBJECT) return;
    auto obj = value.asObject();
    const JsonValue* person = &value;
    auto personIt = obj.find("person");
    if (personIt != obj.end() && personIt->second.getType() == JsonValue::OBJECT) {
        person = &personIt->second;
    }
    auto personObj = person->asObject();
    out.providerPersonId = jsonx::getString(personObj, "person_id");
    if (out.providerPersonId.empty()) out.providerPersonId = jsonx::getString(personObj, "id");
    out.externalId = jsonx::getString(personObj, "external_id");
    for (const auto* name : {"score", "similarity", "confidence"}) {
        auto it = obj.find(name);
        if (it != obj.end() && it->second.getType() == JsonValue::NUMBER) {
            out.score = it->second.asNumber();
            return;
        }
    }
    auto it = personObj.find("score");
    if (it != personObj.end() && it->second.getType() == JsonValue::NUMBER) {
        out.score = it->second.asNumber();
    }
}

void addCandidate(face::LiveResult& out, const face::Match& match) {
    if (match.providerPersonId.empty() && match.externalId.empty()) return;
    for (const auto& existing : out.candidates) {
        if ((!match.providerPersonId.empty() &&
             existing.providerPersonId == match.providerPersonId) ||
            (match.providerPersonId.empty() && existing.externalId == match.externalId)) {
            return;
        }
    }
    out.candidates.push_back(match);
}

std::string frameMime(const std::string& data) {
    if (data.size() > 8 && data.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0) {
        return "image/png";
    }
    if (data.size() > 12 && data.compare(0, 4, "RIFF") == 0 &&
        data.compare(8, 4, "WEBP") == 0) {
        return "image/webp";
    }
    return "image/jpeg";
}

std::string frameName(const std::string& data) {
    std::string type = frameMime(data);
    if (type == "image/png") return "frame.png";
    if (type == "image/webp") return "frame.webp";
    return "frame.jpg";
}

}  // namespace

namespace face {

bool createLivenessSession(LivenessSession& out, std::string& error) {
    auto& cfg = AppConfig::get();
    if (!cfg.faceEnabled) {
        error = "face_not_configured";
        return false;
    }
    std::call_once(curlOnce, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    struct curl_slist* headers = nullptr;
    CURL* curl = makeCurl(headers);
    if (!curl) {
        error = "face_service_unavailable";
        return false;
    }
    std::string url = baseUrl() + "/v1/liveness/session";
    std::string body = "{}";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    HttpResponse response;
    bool ok = perform(curl, response, error);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (!ok) return false;

    JsonValue root;
    if (!parseJson(response.body, root, error)) return false;
    if (response.status < 200 || response.status >= 300) {
        error = apiError(root, "face_liveness_unavailable");
        return false;
    }
    auto obj = root.asObject();
    auto dataIt = obj.find("data");
    if (dataIt == obj.end() || dataIt->second.getType() != JsonValue::OBJECT) {
        error = "invalid_face_response";
        return false;
    }
    auto data = dataIt->second.asObject();
    out.sessionId = jsonx::getString(data, "session_id");
    out.challenge = jsonx::getString(data, "challenge");
    if (out.sessionId.empty() || out.challenge.empty()) {
        error = "invalid_face_response";
        return false;
    }
    auto num = [&data](const char* key, int fallback) {
        auto it = data.find(key);
        if (it != data.end() && it->second.getType() == JsonValue::NUMBER) {
            return static_cast<int>(it->second.asNumber());
        }
        return fallback;
    };
    out.expiresIn = num("expires_in_s", 120);
    out.minFrames = num("min_frames", 3);
    out.maxFrames = num("max_frames", 8);
    return true;
}

bool recognizeLive(const std::string& sessionId,
                   const std::vector<std::string>& frames, double minScore,
                   LiveResult& out, std::string& error) {
    auto& cfg = AppConfig::get();
    if (!cfg.faceEnabled) {
        error = "face_not_configured";
        return false;
    }
    std::call_once(curlOnce, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    struct curl_slist* headers = nullptr;
    CURL* curl = makeCurl(headers);
    if (!curl) {
        error = "face_service_unavailable";
        return false;
    }
    curl_mime* mime = curl_mime_init(curl);
    auto field = [&](const char* name, const std::string& value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
    };
    field("session_id", sessionId);
    field("min_score", std::to_string(minScore));
    field("top_k", "3");
    for (const auto& frame : frames) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "frames");
        curl_mime_filename(part, frameName(frame).c_str());
        curl_mime_type(part, frameMime(frame).c_str());
        curl_mime_data(part, frame.data(), frame.size());
    }
    std::string url = baseUrl() + "/v1/recognize/live";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    HttpResponse response;
    bool ok = perform(curl, response, error);
    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (!ok) return false;

    JsonValue root;
    if (!parseJson(response.body, root, error)) return false;
    if (response.status < 200 || response.status >= 300) {
        error = apiError(root, "face_recognition_failed");
        applog::error("[face] recognize/live failed frames=" +
                      std::to_string(frames.size()) + " code=" + error +
                      " body=" + response.body.substr(0, 1600));
        return false;
    }
    auto obj = root.asObject();
    auto dataIt = obj.find("data");
    if (dataIt == obj.end() || dataIt->second.getType() != JsonValue::OBJECT) {
        error = "invalid_face_response";
        return false;
    }
    auto data = dataIt->second.asObject();
    const JsonValue* liveness = &dataIt->second;
    auto livenessIt = data.find("liveness");
    if (livenessIt != data.end() && livenessIt->second.getType() == JsonValue::OBJECT) {
        liveness = &livenessIt->second;
    }
    if (liveness->getType() == JsonValue::OBJECT) {
        auto live = liveness->asObject();
        auto passedIt = live.find("passed");
        const std::map<std::string, JsonValue>* passedMap = &live;
        if (passedIt == live.end()) {
            passedIt = data.find("passed");
            passedMap = &data;
        }
        out.passed = passedIt != passedMap->end() &&
                     passedIt->second.getType() == JsonValue::BOOLEAN &&
                     passedIt->second.asBoolean();
        out.challenge = jsonx::getString(live, "challenge");
        if (out.challenge.empty()) out.challenge = jsonx::getString(data, "challenge");
    }
    auto bestIt = data.find("best_frame");
    if (bestIt == data.end()) bestIt = data.find("best_frame_index");
    if (bestIt != data.end() && bestIt->second.getType() == JsonValue::NUMBER) {
        out.bestFrame = static_cast<int>(bestIt->second.asNumber());
    }
    auto matchIt = data.find("match");
    if (matchIt != data.end() && matchIt->second.getType() == JsonValue::OBJECT) {
        face::Match match;
        parseMatch(matchIt->second, match);
        addCandidate(out, match);
    }
    for (const char* listName : {"candidates", "faces"}) {
        auto listIt = data.find(listName);
        if (listIt == data.end() || listIt->second.getType() != JsonValue::ARRAY) continue;
        for (const auto& candidateValue : listIt->second.asArray()) {
            if (candidateValue.getType() != JsonValue::OBJECT) continue;
            auto candidateObj = candidateValue.asObject();
            auto nestedMatch = candidateObj.find("match");
            if (nestedMatch != candidateObj.end() &&
                nestedMatch->second.getType() == JsonValue::OBJECT) {
                face::Match match;
                parseMatch(nestedMatch->second, match);
                addCandidate(out, match);
            } else {
                face::Match match;
                parseMatch(candidateValue, match);
                addCandidate(out, match);
            }
        }
    }
    if (!out.candidates.empty()) {
        out.match = out.candidates.front();
        for (const auto& candidate : out.candidates) {
            if (candidate.score > out.match.score) out.match = candidate;
        }
    }
    if (!out.passed) {
        // Upstream returns 200 with passed=false for liveness/identity failures;
        // keep its diagnostics (motion_range, identity_median, usable frames)
        // in the web-visible log so a failed attempt can be tuned without guessing.
        applog::access("[face] liveness not passed frames=" + std::to_string(frames.size()) +
                       " body=" + response.body.substr(0, 1600));
    }
    return true;
}

bool enrollLive(const std::string& sessionId, const std::string& externalId,
                const std::string& name, const std::vector<std::string>& frames,
                std::string& providerPersonId, std::string& error) {
    auto& cfg = AppConfig::get();
    if (!cfg.faceEnabled) {
        error = "face_not_configured";
        return false;
    }
    std::call_once(curlOnce, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    struct curl_slist* headers = nullptr;
    CURL* curl = makeCurl(headers);
    if (!curl) {
        error = "face_service_unavailable";
        return false;
    }
    curl_mime* mime = curl_mime_init(curl);
    auto field = [&](const char* fieldName, const std::string& value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, fieldName);
        curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
    };
    field("session_id", sessionId);
    field("external_id", externalId);
    field("name", name);
    for (const auto& frame : frames) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "frames");
        curl_mime_filename(part, frameName(frame).c_str());
        curl_mime_type(part, frameMime(frame).c_str());
        curl_mime_data(part, frame.data(), frame.size());
    }
    std::string url = baseUrl() + "/v1/persons/live";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    HttpResponse response;
    bool ok = perform(curl, response, error);
    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (!ok) return false;

    JsonValue root;
    if (!parseJson(response.body, root, error)) return false;
    if (response.status < 200 || response.status >= 300) {
        error = apiError(root, "face_enrollment_failed");
        applog::error("[face] persons/live failed frames=" +
                      std::to_string(frames.size()) + " code=" + error +
                      " body=" + response.body.substr(0, 1600));
        return false;
    }
    auto obj = root.asObject();
    auto dataIt = obj.find("data");
    if (dataIt == obj.end() || dataIt->second.getType() != JsonValue::OBJECT) {
        error = "invalid_face_response";
        return false;
    }
    auto data = dataIt->second.asObject();
    providerPersonId = jsonx::getString(data, "person_id");
    if (providerPersonId.empty()) {
        auto personIt = data.find("person");
        if (personIt != data.end() && personIt->second.getType() == JsonValue::OBJECT) {
            providerPersonId = jsonx::getString(personIt->second.asObject(), "person_id");
        }
    }
    if (providerPersonId.empty()) {
        error = "invalid_face_response";
        return false;
    }
    return true;
}

bool deletePersonByExternalId(const std::string& externalId, std::string& error) {
    auto& cfg = AppConfig::get();
    if (!cfg.faceEnabled) {
        error = "face_not_configured";
        return false;
    }
    if (externalId.empty()) {
        error = "invalid_external_id";
        return false;
    }
    std::call_once(curlOnce, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    struct curl_slist* headers = nullptr;
    CURL* curl = makeCurl(headers);
    if (!curl) {
        error = "face_service_unavailable";
        return false;
    }
    char* escaped = curl_easy_escape(curl, externalId.c_str(), 0);
    std::string encoded = escaped ? escaped : "";
    if (escaped) curl_free(escaped);
    std::string url = baseUrl() + "/v1/persons?external_id=" + encoded;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    HttpResponse response;
    bool ok = perform(curl, response, error);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (!ok) return false;
    // The idempotent endpoint returns 200 with deleted=false when the person
    // was never there or was already removed; treat both as success.
    if (response.status >= 200 && response.status < 300) return true;
    if (response.status == 404) {
        error = "not_found";
        return false;
    }
    JsonValue root;
    if (parseJson(response.body, root, error)) {
        error = apiError(root, "face_delete_failed");
    }
    applog::error("[face] delete by external_id failed code=" + error +
                  " body=" + response.body.substr(0, 800));
    return false;
}

bool deletePerson(const std::string& providerPersonId, std::string& error) {
    auto& cfg = AppConfig::get();
    if (!cfg.faceEnabled) {
        error = "face_not_configured";
        return false;
    }
    std::call_once(curlOnce, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    struct curl_slist* headers = nullptr;
    CURL* curl = makeCurl(headers);
    if (!curl) {
        error = "face_service_unavailable";
        return false;
    }
    std::string url = baseUrl() + "/v1/persons/" + providerPersonId;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    HttpResponse response;
    bool ok = perform(curl, response, error);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (!ok) return false;
    if (response.status == 404) return true;
    if (response.status < 200 || response.status >= 300) {
        JsonValue root;
        if (parseJson(response.body, root, error)) {
            error = apiError(root, "face_delete_failed");
        }
        return false;
    }
    return true;
}

}  // namespace face

#include "core.h"

#include <curl/curl.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

bool pathIsSafe(const std::string& path) {
    if (path.empty() || path.front() != '/') return false;
    if (path.find("..") != std::string::npos) return false;
    if (path.find('\\') != std::string::npos) return false;
    if (path.find('\0') != std::string::npos) return false;
    if (path.find("//") != std::string::npos) return false;
    return true;
}

bool safeJoin(const std::string& root, const std::string& relative, std::string& out) {
    if (relative.empty() || relative.front() == '/') return false;
    if (relative.find("..") != std::string::npos) return false;
    if (relative.find('\\') != std::string::npos) return false;
    if (relative.find('\0') != std::string::npos) return false;
    std::error_code ec;
    auto rootPath = std::filesystem::weakly_canonical(std::filesystem::path(root), ec);
    if (ec) return false;
    auto candidate = std::filesystem::weakly_canonical(rootPath / relative, ec);
    if (ec) return false;
    auto rootString = rootPath.string();
    auto candidateString = candidate.string();
    if (candidateString != rootString &&
        candidateString.rfind(rootString + "/", 0) != 0) {
        return false;
    }
    out = candidateString;
    return true;
}

// Transport (cpp-httplib) already applies brotli/gzip per Accept-Encoding;
// serve raw bytes here.
bool serveFile(Response& res, const std::string& filePath, bool immutable) {
    std::string data;
    if (!util::readFile(filePath, data)) return false;
    res.statusCode = 200;
    res.headers["Content-Type"] = util::mimeType(filePath);
    res.headers["Content-Length"] = std::to_string(data.size());
    res.headers["Cache-Control"] = immutable
        ? "public, max-age=31536000, immutable"
        : "no-cache";
    res.body = std::move(data);
    return true;
}

size_t discardResponse(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

}  // namespace

bool spaFallback(const Request& req, Response& res) {
    if (req.method != "GET" && req.method != "HEAD") return false;
    if (req.path.rfind("/api/", 0) == 0 || req.path.rfind("/.well-known/", 0) == 0) {
        return false;
    }
    if (!pathIsSafe(req.path)) return false;

    auto& cfg = AppConfig::get();
    std::string path = req.path;

    // Persistent uploads (avatars, app icons).
    if (path.rfind("/uploads/", 0) == 0) {
        std::string filePath;
        if (!safeJoin(cfg.dataRoot, path.substr(1), filePath)) return false;
        return serveFile(res, filePath, true);
    }

    // Vite assets have content hashes and can be cached aggressively.
    if (path.rfind("/assets/", 0) == 0) {
        std::string filePath;
        if (!safeJoin(cfg.webRoot, path.substr(1), filePath)) return false;
        if (serveFile(res, filePath, true)) return true;
    }

    // Public files copied from frontend/public (favicon, logo, robots.txt, ...).
    if (path != "/" && path.find('.') != std::string::npos) {
        std::string filePath;
        if (!safeJoin(cfg.webRoot, path.substr(1), filePath)) return false;
        if (serveFile(res, filePath, false)) return true;
    }

    // Any other non-asset request is an SPA route: serve index.html.
    std::string index;
    if (!safeJoin(cfg.webRoot, "index.html", index)) return false;
    return serveFile(res, index, false);
}

namespace handlers {

void health(const Request&, Response& res) {
    bool db = Db::healthy();
    bool redis = Cache::healthy();
    std::map<std::string, JsonValue> obj;
    obj["ok"] = JsonValue(db && redis);
    obj["service"] = JsonValue("unisso");
    obj["version"] = JsonValue("2.0.0");
    obj["database"] = JsonValue(db ? "ok" : "degraded");
    obj["redis"] = JsonValue(redis ? "ok" : "degraded");
    http::json(res, (db && redis) ? 200 : 503, JsonValue(obj));
}

}  // namespace handlers

namespace services {

namespace {
std::mutex hbMutex;
std::condition_variable hbCv;
bool hbStop = false;
std::thread hbThread;
}  // namespace

void startHeartbeat() {
    auto& cfg = AppConfig::get();
    if (cfg.reportUrl.empty() || cfg.reportToken.empty() || cfg.deployId.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(hbMutex);
        hbStop = false;
    }
    // Joinable (not detached): stopHeartbeat() must be able to join this
    // thread before static destruction, otherwise it keeps running against
    // destroyed globals during exit().
    hbThread = std::thread([cfg]() {
        std::unique_lock<std::mutex> lock(hbMutex);
        if (hbCv.wait_for(lock, std::chrono::seconds(2), [] { return hbStop; })) return;
        while (true) {
            lock.unlock();
            CURL* curl = curl_easy_init();
            if (curl) {
                std::string body = "{\"deploy_id\":\"" + cfg.deployId +
                                   "\",\"state\":\"ready\",\"message\":\"unisso v2 heartbeat\",\"metrics\":\"\"}";
                struct curl_slist* headers = nullptr;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                std::string tokenHeader = "X-Report-Token: " + cfg.reportToken;
                headers = curl_slist_append(headers, tokenHeader.c_str());
                curl_easy_setopt(curl, CURLOPT_URL, cfg.reportUrl.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardResponse);
                curl_easy_perform(curl);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
            }
            lock.lock();
            if (hbCv.wait_for(lock, std::chrono::seconds(30), [] { return hbStop; })) return;
        }
    });
}

void stopHeartbeat() {
    {
        std::lock_guard<std::mutex> lock(hbMutex);
        hbStop = true;
    }
    hbCv.notify_all();
    if (hbThread.joinable()) hbThread.join();
}

}  // namespace services

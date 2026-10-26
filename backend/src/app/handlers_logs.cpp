#include "core.h"
#include "app/logger.h"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>

namespace {
std::string sanitize(std::string value, size_t maxLen) {
    for (char& c : value) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if (value.size() > maxLen) value.resize(maxLen);
    return value;
}
}  // namespace

namespace handlers {

void clientLog(const Request& req, Response& res) {
    std::string ip = http::clientIp(req).empty() ? "-" : http::clientIp(req);
    long long count = Cache::incr("client_log:" + ip, 60);
    if (count > 120) {
        http::error(res, 429, "rate_limited", "日志写入过于频繁");
        return;
    }
    std::string level = sanitize(util::trim(http::input(req, "level")), 32);
    std::string message = sanitize(http::input(req, "message"), 1000);
    std::string url = sanitize(http::input(req, "url"), 500);
    std::string stack = sanitize(http::input(req, "stack"), 2000);
    if (message.empty() && stack.empty()) {
        http::ok(res);
        return;
    }
    auto user = authx::currentUser(req);
    std::string who = user ? user->email : "-";
    applog::frontend("ip=" + ip + " user=" + sanitize(who, 128) +
                     " level=" + level + " url=" + url +
                     " message=" + message + " stack=" + stack);
    http::ok(res);
}

void adminLogsList(const Request& req, Response& res) {
    User admin;
    if (!authx::requireSuperAdmin(req, res, admin)) return;
    namespace fs = std::filesystem;
    std::vector<JsonValue> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(applog::dir(), ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".log") continue;
        std::map<std::string, JsonValue> item;
        item["name"] = JsonValue(entry.path().filename().string());
        item["size"] = jsonx::num(static_cast<long long>(fs::file_size(entry.path(), ec)));
        files.push_back(JsonValue(item));
    }
    std::map<std::string, JsonValue> obj;
    obj["files"] = JsonValue(files);
    http::json(res, 200, JsonValue(obj));
}

void adminLogsTail(const Request& req, Response& res) {
    User admin;
    if (!authx::requireSuperAdmin(req, res, admin)) return;
    std::string file = http::param(req, "file");
    if (file.empty()) file = http::query(req, "file");
    if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos ||
        file.find("..") != std::string::npos || file.size() < 4 ||
        file.substr(file.size() - 4) != ".log") {
        http::error(res, 400, "invalid_file", "文件名不合法");
        return;
    }
    int lines = 200;
    std::string linesParam = http::query(req, "lines");
    if (!linesParam.empty()) {
        try { lines = std::stoi(linesParam); } catch (...) {}
    }
    if (lines < 10) lines = 10;
    if (lines > 2000) lines = 2000;

    std::ifstream in(applog::dir() + "/" + file);
    if (!in.is_open()) {
        http::error(res, 404, "not_found", "日志文件不存在");
        return;
    }
    std::deque<std::string> tail;
    std::string line;
    while (std::getline(in, line)) {
        tail.push_back(line);
        if (static_cast<int>(tail.size()) > lines) tail.pop_front();
    }
    std::vector<JsonValue> out;
    for (const auto& item : tail) out.push_back(JsonValue(item));
    std::map<std::string, JsonValue> obj;
    obj["file"] = JsonValue(file);
    obj["lines"] = JsonValue(out);
    http::json(res, 200, JsonValue(obj));
}

}  // namespace handlers

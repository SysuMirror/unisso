#include "app/logger.h"
#include "core.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
struct Entry {
    std::string file;
    std::string line;
};

std::mutex queueMutex;
std::condition_variable queueCv;
std::deque<Entry> queue;
std::thread worker;
bool stopping = false;
std::filesystem::path logPath;
constexpr size_t kMaxQueue = 20000;

void enqueue(const std::string& file, const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (queue.size() >= kMaxQueue) return;
        queue.push_back(Entry{file, util::nowIso() + " " + line});
    }
    queueCv.notify_one();
}

void rotateIfNeeded(std::ofstream& out, const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    if (std::filesystem::file_size(path, ec) <= 8 * 1024 * 1024) return;
    out.close();
    auto rotated = path;
    rotated += ".1";
    std::filesystem::remove(rotated, ec);
    std::filesystem::rename(path, rotated, ec);
    out.open(path, std::ios::app);
}

void workerLoop() {
    std::ofstream backend;
    std::ofstream frontend;
    std::filesystem::create_directories(logPath);
    backend.open(logPath / "backend.log", std::ios::app);
    frontend.open(logPath / "frontend.log", std::ios::app);
    size_t writes = 0;

    while (true) {
        std::deque<Entry> batch;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [] { return stopping || !queue.empty(); });
            while (!queue.empty() && batch.size() < 256) {
                batch.push_back(std::move(queue.front()));
                queue.pop_front();
            }
            if (stopping && queue.empty()) break;
        }
        for (const auto& entry : batch) {
            if (entry.file == "frontend.log") {
                if (!frontend.is_open()) frontend.open(logPath / "frontend.log", std::ios::app);
                frontend << entry.line << "\n";
            } else {
                if (!backend.is_open()) backend.open(logPath / "backend.log", std::ios::app);
                backend << entry.line << "\n";
            }
            ++writes;
        }
        if (backend.is_open()) backend.flush();
        if (frontend.is_open()) frontend.flush();
        if (writes >= 2048) {
            writes = 0;
            if (backend.is_open()) rotateIfNeeded(backend, logPath / "backend.log");
            if (frontend.is_open()) rotateIfNeeded(frontend, logPath / "frontend.log");
        }
    }
}
}  // namespace

namespace applog {

void init() {
    logPath = util::env("UNISSO_LOG_DIR", "/home/cloud/deploy/logs");
    std::error_code ec;
    std::filesystem::create_directories(logPath, ec);
    worker = std::thread(workerLoop);
    enqueue("backend.log", "logger initialized dir=" + logPath.string());
}

std::string dir() { return logPath.empty() ? std::string("/home/cloud/deploy/logs") : logPath.string(); }

void access(const std::string& line) { enqueue("backend.log", line); }
void error(const std::string& line) { enqueue("backend.log", "[error] " + line); }
void frontend(const std::string& line) { enqueue("frontend.log", line); }

}  // namespace applog

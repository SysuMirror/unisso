#pragma once
#include <string>

namespace applog {
void init();
// Drain the queue and join the writer thread. MUST be called before main
// returns: the global std::thread would otherwise be destroyed while
// joinable, which calls std::terminate() during static destruction.
void shutdown();
std::string dir();
void access(const std::string& line);
void error(const std::string& line);
void frontend(const std::string& line);
}

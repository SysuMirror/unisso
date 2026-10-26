#pragma once
#include <string>

namespace applog {
void init();
std::string dir();
void access(const std::string& line);
void error(const std::string& line);
void frontend(const std::string& line);
}

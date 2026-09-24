#pragma once

#include <iostream>
#include <mutex>
#include <sstream>

namespace usc::log {

inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}

template <typename... Args>
void write(const char* level, const char* tag, Args&&... args) {
    std::ostringstream os;
    os << '[' << level << "][" << tag << "] ";
    (os << ... << args);
    std::lock_guard<std::mutex> lock(mutex());
    std::cerr << os.str() << std::endl;
}

} // namespace usc::log

#define LOG_INFO(tag, ...) ::usc::log::write("info", tag, __VA_ARGS__)
#define LOG_WARN(tag, ...) ::usc::log::write("warn", tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) ::usc::log::write("error", tag, __VA_ARGS__)

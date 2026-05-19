#pragma once

#include <string>

namespace j2k {

enum class LogLevel {
    Info = 0,
    Warn = 1,
    Error = 2,
};

void log(LogLevel level, const std::string& message);

}  // namespace j2k

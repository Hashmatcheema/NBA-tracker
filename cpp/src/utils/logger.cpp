#include "j2k/logger.hpp"

#include <iostream>

namespace j2k {

namespace {
const char* to_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        default:
            return "LOG";
    }
}
}  // namespace

void log(LogLevel level, const std::string& message) {
    std::ostream& os = (level == LogLevel::Error) ? std::cerr : std::cout;
    os << "[j2k][" << to_tag(level) << "] " << message << '\n';
}

}  // namespace j2k

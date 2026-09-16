#pragma once

#include <string>
#include <spdlog/spdlog.h>

// Centralized logging wrapper around spdlog.
// Business code should use the CA_LOG_* macros only; spdlog is the sole
// dependency for logging so it can be swapped later without touching call sites.

namespace ca {
namespace log {

inline void set_level(const std::string& level) {
    spdlog::level::level_enum lvl = spdlog::level::info;
    if (level == "trace")       lvl = spdlog::level::trace;
    else if (level == "debug")  lvl = spdlog::level::debug;
    else if (level == "warn")   lvl = spdlog::level::warn;
    else if (level == "error")  lvl = spdlog::level::err;
    else if (level == "info")   lvl = spdlog::level::info;
    spdlog::set_level(lvl);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
}

} // namespace log
} // namespace ca

#define CA_LOG_TRACE(...) spdlog::trace(__VA_ARGS__)
#define CA_LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#define CA_LOG_INFO(...)  spdlog::info(__VA_ARGS__)
#define CA_LOG_WARN(...)  spdlog::warn(__VA_ARGS__)
#define CA_LOG_ERROR(...) spdlog::error(__VA_ARGS__)

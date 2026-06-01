#pragma once

#include <pulp/platform/detect.hpp>
#include <string_view>
#include <string>
#include <cstdio>
#include <format>

#if defined(__APPLE__)
    #include <os/log.h>
#elif defined(_WIN32)
    #include <pulp/platform/win32_sane.hpp>
#endif

namespace pulp::runtime {

enum class LogLevel { Debug, Info, Warning, Error };

namespace detail {

inline void log_impl(LogLevel level, std::string_view message) {
    const char* prefix = "";
    switch (level) {
        case LogLevel::Debug:   prefix = "[pulp:debug] "; break;
        case LogLevel::Info:    prefix = "[pulp:info]  "; break;
        case LogLevel::Warning: prefix = "[pulp:warn]  "; break;
        case LogLevel::Error:   prefix = "[pulp:error] "; break;
    }

#if defined(__APPLE__)
    os_log_type_t type = OS_LOG_TYPE_DEFAULT;
    switch (level) {
        case LogLevel::Debug:   type = OS_LOG_TYPE_DEBUG; break;
        case LogLevel::Info:    type = OS_LOG_TYPE_INFO; break;
        case LogLevel::Warning: type = OS_LOG_TYPE_DEFAULT; break;
        case LogLevel::Error:   type = OS_LOG_TYPE_ERROR; break;
    }
    // Named subsystem so out-of-process logs (notably an AUv3 extension's)
    // are capturable via `log stream --predicate 'subsystem ==
    // "dev.pulp.runtime"'` and Console.app. OS_LOG_DEFAULT routes info/
    // debug records that are not persisted for extension processes, so
    // `simctl log show/stream` saw nothing during the #3217 cube
    // investigation. A function-local static initializes the logger once.
    static os_log_t pulp_log = os_log_create("dev.pulp.runtime", "runtime");
    os_log_with_type(pulp_log, type, "%{public}s%{public}s",
                     prefix, std::string(message).c_str());
#elif defined(_WIN32)
    auto full = std::string(prefix) + std::string(message) + "\n";
    OutputDebugStringA(full.c_str());
#endif
    // Always also write to stderr
    std::fprintf(stderr, "%s%.*s\n", prefix,
                 static_cast<int>(message.size()), message.data());
}

} // namespace detail

template<typename... Args>
void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    auto msg = std::format(fmt, std::forward<Args>(args)...);
    detail::log_impl(level, msg);
}

template<typename... Args>
void log_debug(std::format_string<Args...> fmt, Args&&... args) {
    if constexpr (pulp::platform::is_debug) {
        log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
    }
}

template<typename... Args>
void log_info(std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Info, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void log_warn(std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Warning, fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void log_error(std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::Error, fmt, std::forward<Args>(args)...);
}

} // namespace pulp::runtime

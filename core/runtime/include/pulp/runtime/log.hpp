#pragma once

// Logging.
//
// `log_info(...)` and friends write to the platform log (os_log, the debugger
// output window, stderr) and then hand the message to the registered sink, if
// any. Header-only: nothing here needs a runtime translation unit, so modules
// that include it without linking pulp-runtime keep working.
//
// Logging is NOT real-time safe. Formatting allocates, the platform writers and
// sinks take locks and perform I/O, and a sink is arbitrary user code. Never
// log from `Processor::process()` or any other audio-thread path. Hand the
// event to a non-realtime thread (an SPSC queue drained by a worker) and log
// there.
//
// Sinks: `set_log_sink` installs one process-wide callback that receives
// (level, category, message) for every record. It is the hook for file logs
// (`FileLogSink`), test capture (`CaptureLogSink`) and diagnostics surfaces; see
// <pulp/runtime/log_sink.hpp>. The registry is per loaded binary image: a
// plugin and its host each have their own, which keeps one plugin's sink from
// receiving another's records.

#include <pulp/platform/detect.hpp>
#include <pulp/runtime/exceptions.hpp>
#include <atomic>
#include <cstdio>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#if defined(__APPLE__)
    #include <os/log.h>
#elif defined(_WIN32)
    #include <pulp/platform/win32_sane.hpp>
#endif

namespace pulp::runtime {

enum class LogLevel { Debug, Info, Warning, Error };

/// Receives every record after the platform writers. Invoked on the logging
/// thread, possibly from several threads at once, so it must synchronize its own
/// state. It must not throw (a thrown exception is swallowed) and must not
/// assume it is realtime-safe or that it is the only consumer. Records logged
/// from inside a sink on the same thread are not re-delivered to it.
using LogSink =
    std::function<void(LogLevel level, std::string_view category, std::string_view message)>;

/// Category used by `log_info` and the other level helpers.
inline constexpr std::string_view kDefaultLogCategory = "pulp";

namespace detail {

struct LogSinkRegistry {
    std::mutex mutex;
    std::shared_ptr<const LogSink> sink;
    std::atomic<bool> active{false}; // relaxed fast path: skip the lock when no sink is set
};

// Intentionally leaked so logging during static destruction stays valid.
inline LogSinkRegistry& log_sink_registry() {
    static LogSinkRegistry* registry = new LogSinkRegistry;
    return *registry;
}

inline bool& in_log_sink_dispatch() {
    static thread_local bool flag = false;
    return flag;
}

inline void dispatch_log_sink(LogLevel level, std::string_view category, std::string_view message) {
    auto& registry = log_sink_registry();
    if (!registry.active.load(std::memory_order_relaxed))
        return;
    bool& busy = in_log_sink_dispatch();
    if (busy)
        return; // a sink that logs must not recurse into itself

    std::shared_ptr<const LogSink> sink;
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        sink = registry.sink;
    }
    if (!sink || !*sink)
        return;

    busy = true;
    PULP_TRY {
        (*sink)(level, category, message);
    }
    PULP_CATCH_ALL {
        // A misbehaving sink must never take the logging caller down.
    }
    busy = false;
}

inline void log_impl(LogLevel level, std::string_view category, std::string_view message) {
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

    dispatch_log_sink(level, category, message);
}

inline void log_impl(LogLevel level, std::string_view message) {
    log_impl(level, kDefaultLogCategory, message);
}

} // namespace detail

/// Install `sink` as the process-wide log sink and return the previous one
/// (empty if none). Passing an empty function removes the sink. The swap is
/// thread-safe: a record being delivered to the old sink finishes there, and
/// the old sink may still run briefly on other threads after this returns.
inline LogSink set_log_sink(LogSink sink) {
    auto& registry = detail::log_sink_registry();
    std::shared_ptr<const LogSink> incoming;
    if (sink)
        incoming = std::make_shared<const LogSink>(std::move(sink));

    std::shared_ptr<const LogSink> previous;
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        previous = std::move(registry.sink);
        registry.sink = std::move(incoming);
        registry.active.store(registry.sink != nullptr, std::memory_order_relaxed);
    }
    return previous ? *previous : LogSink{};
}

/// Log with an explicit category (a subsystem tag such as "audio" or "net").
template <typename... Args>
void log_with_category(std::string_view category, LogLevel level, std::format_string<Args...> fmt,
                       Args&&... args) {
    auto msg = std::format(fmt, std::forward<Args>(args)...);
    detail::log_impl(level, category, msg);
}

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

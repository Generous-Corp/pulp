// console_capture.hpp — JS console log interception for the inspector
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

/// Captures JS console output (console.log/warn/error) for inspector display.
/// Chains on an existing log callback — does not replace it.
class ConsoleCapture {
public:
    /// A captured log entry
    struct Entry {
        std::string level;    // "log", "warn", "error", "info", "debug"
        std::string message;
        std::chrono::steady_clock::time_point time;
    };

    using LogCallback = std::function<void(std::string_view level, std::string_view message)>;

    /// Install the capture. Chains with the previous callback.
    /// Call with the ScriptEngine's set_log_callback: engine.set_log_callback(capture.callback());
    LogCallback callback(LogCallback previous = {});

    /// Get all captured entries (ring buffer, last 200)
    std::vector<Entry> entries() const;

    /// Clear all entries
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    static constexpr size_t kMaxEntries = 200;
};

} // namespace pulp::inspect

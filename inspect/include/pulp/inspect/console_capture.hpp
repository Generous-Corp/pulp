// console_capture.hpp — JS console log interception for the inspector
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

/// Captures JS console output (console.log/warn/error) for inspector display.
/// Chains on an existing log callback — does not replace it.
///
/// Each entry carries a strictly increasing `seq`, so a device-log client can
/// poll only what is new since its last cursor (`entries_since`) instead of
/// re-reading the whole ring buffer. An optional sink fires synchronously on
/// each capture so a host that owns an InspectorServer can also push live
/// `Console.messageAdded` events.
class ConsoleCapture {
public:
    /// A captured log entry
    struct Entry {
        std::string level;    // "log", "warn", "error", "info", "debug"
        std::string message;
        std::chrono::steady_clock::time_point time;
        uint64_t seq = 0;     // strictly increasing capture sequence (>= 1)
    };

    using LogCallback = std::function<void(std::string_view level, std::string_view message)>;
    /// Fired on the capturing (engine/UI) thread for each new entry.
    using EntrySink = std::function<void(const Entry&)>;

    /// Install the capture. Chains with the previous callback.
    /// Call with the ScriptEngine's set_log_callback: engine.set_log_callback(capture.callback());
    LogCallback callback(LogCallback previous = {});

    /// Get all captured entries (ring buffer, last 200)
    std::vector<Entry> entries() const;

    /// Get only entries whose seq is strictly greater than `after_seq`, plus
    /// the highest seq observed so far (as the next cursor). A client passes
    /// back the previous `next_seq` to page forward without duplicates. Because
    /// the buffer is bounded, entries older than the retained window are
    /// dropped; the returned `next_seq` still advances past them.
    std::vector<Entry> entries_since(uint64_t after_seq, uint64_t& next_seq) const;

    /// The highest sequence assigned so far (0 when nothing captured).
    uint64_t latest_seq() const;

    /// Install a sink invoked synchronously for every newly captured entry.
    /// Pass {} to clear. Used for live event push; polling works without it.
    void set_entry_sink(EntrySink sink);

    /// Clear all entries (does not reset the sequence counter).
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    EntrySink sink_;
    uint64_t next_seq_ = 0;
    static constexpr size_t kMaxEntries = 200;
};

} // namespace pulp::inspect

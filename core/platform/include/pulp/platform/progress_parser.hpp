// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace pulp::platform {

/// A structured progress event parsed from a PROGRESS:TYPE:PAYLOAD line.
struct ProgressEvent {
    std::string type;     ///< e.g., "DOWNLOAD_START", "OVERALL", "ERROR"
    std::string payload;  ///< Everything after PROGRESS:TYPE:
};

/// Parses lines matching the PROGRESS:TYPE:PAYLOAD protocol and fires a
/// callback for each match. Lines that don't match are silently ignored.
///
/// Usage with ChildProcess:
/// @code
/// ProgressParser parser([&](const ProgressEvent& e) {
///     progress_queue.push(e);  // SpscQueue for UI delivery
/// });
/// ProcessOptions opts;
/// opts.on_stdout_line = [&](std::string_view line) { parser.feed_line(line); };
/// auto result = ChildProcess::run("python3", {"script.py"}, opts);
/// @endcode
class ProgressParser {
public:
    using Callback = std::function<void(const ProgressEvent&)>;

    explicit ProgressParser(Callback cb) : callback_(std::move(cb)) {}

    /// Feed a line of output. If it matches PROGRESS:TYPE:PAYLOAD, fires the
    /// callback. Otherwise does nothing.
    void feed_line(std::string_view line) {
        if (!line.starts_with("PROGRESS:")) return;
        auto rest = line.substr(9);  // after "PROGRESS:"
        auto colon = rest.find(':');
        if (colon == std::string_view::npos) {
            // PROGRESS:TYPE (no payload)
            if (callback_) callback_({std::string(rest), {}});
        } else {
            // PROGRESS:TYPE:PAYLOAD
            if (callback_) callback_({
                std::string(rest.substr(0, colon)),
                std::string(rest.substr(colon + 1))
            });
        }
    }

private:
    Callback callback_;
};

}  // namespace pulp::platform

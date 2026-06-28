// cmd_run_parse.cpp — pure flag parser for `pulp run`.
//
// Split out from cmd_run.cpp so test_cli_run_options can link the parser
// without dragging in cli_common (and its filesystem / colour / project
// resolution helpers). #914.

#include "cmd_run.hpp"

#include <charconv>
#include <algorithm>
#include <string>
#include <system_error>

namespace pulp_cli {

namespace {

bool parse_frame_count(const std::string& value, int& frames) {
    if (value.empty()) return false;
    if (value.front() == '+') return false;
    int parsed = 0;
    const char* first = value.data();
    const char* last = first + value.size();
    auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) return false;
    frames = parsed;
    return true;
}

bool parse_nonnegative_int(const std::string& value, int& out) {
    if (value.empty()) return false;
    if (value.front() == '+') return false;
    int parsed = 0;
    const char* first = value.data();
    const char* last = first + value.size();
    auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) return false;
    if (parsed < 0) return false;
    out = parsed;
    return true;
}

bool valid_scope_trigger(std::string value) {
    std::replace(value.begin(), value.end(), '_', '-');
    return value == "none" || value == "off" || value == "raw"
        || value == "rising-zero";
}

}  // namespace

ParseRunResult parse_run_options(const std::vector<std::string>& args) {
    ParseRunResult r;
    bool after_separator = false;
    bool audio_scope_acquisition_option_seen = false;
    bool audio_capture_frames_option_seen = false;
    bool audio_capture_rolling_frames_option_seen = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];

        if (a == "--help" || a == "-h") {
            r.help = true;
            return r;
        }

        if (a == "--") {
            after_separator = true;
            continue;
        }

        if (after_separator) {
            r.user_pass_through.push_back(a);
            continue;
        }

        if (a == "--headless") {
            r.headless = true;
            continue;
        }
        if (a == "--screenshot") {
            r.headless = true;  // --screenshot implies --headless
            if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != '-') {
                r.screenshot_path = args[++i];
            } else {
                r.error = "--screenshot requires a path argument";
                return r;
            }
            continue;
        }
        if (a.rfind("--screenshot=", 0) == 0) {
            r.headless = true;
            r.screenshot_path = a.substr(std::string("--screenshot=").size());
            if (r.screenshot_path.empty()) {
                r.error = "--screenshot= requires a non-empty path";
                return r;
            }
            continue;
        }
        if (a == "--frames") {
            if (i + 1 < args.size()) {
                int n = 0;
                if (!parse_frame_count(args[i + 1], n)) {
                    r.error = "--frames requires an integer argument";
                    return r;
                }
                if (n <= 0) { r.error = "--frames must be > 0"; return r; }
                r.frames = n;
                ++i;
                continue;
            }
            r.error = "--frames requires an integer argument";
            return r;
        }
        if (a.rfind("--frames=", 0) == 0) {
            int n = 0;
            if (!parse_frame_count(a.substr(std::string("--frames=").size()), n)) {
                r.error = "--frames= requires an integer";
                return r;
            }
            if (n <= 0) { r.error = "--frames must be > 0"; return r; }
            r.frames = n;
            continue;
        }
        if (a == "--watch") {
            r.watch = true;
            continue;
        }
        if (a == "--audio-inspector") {
            r.audio_inspector = true;
            continue;
        }
        if (a == "--audio-probe-json") {
            r.headless = true;  // implies --headless (one-shot dump + exit)
            if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != '-') {
                r.audio_probe_json_path = args[++i];
            } else {
                r.error = "--audio-probe-json requires a path argument";
                return r;
            }
            continue;
        }
        if (a.rfind("--audio-probe-json=", 0) == 0) {
            r.headless = true;
            r.audio_probe_json_path = a.substr(std::string("--audio-probe-json=").size());
            if (r.audio_probe_json_path.empty()) {
                r.error = "--audio-probe-json= requires a non-empty path";
                return r;
            }
            continue;
        }
        if (a == "--audio-scope-json") {
            r.headless = true;
            if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != '-') {
                r.audio_scope_json_path = args[++i];
            } else {
                r.error = "--audio-scope-json requires a path argument";
                return r;
            }
            continue;
        }
        if (a.rfind("--audio-scope-json=", 0) == 0) {
            r.headless = true;
            r.audio_scope_json_path = a.substr(std::string("--audio-scope-json=").size());
            if (r.audio_scope_json_path.empty()) {
                r.error = "--audio-scope-json= requires a non-empty path";
                return r;
            }
            continue;
        }
        if (a == "--audio-scope-window") {
            audio_scope_acquisition_option_seen = true;
            if (i + 1 >= args.size()
                || !parse_frame_count(args[i + 1], r.audio_scope_window)) {
                r.error = "--audio-scope-window requires an integer argument";
                return r;
            }
            if (r.audio_scope_window <= 0) {
                r.error = "--audio-scope-window must be > 0";
                return r;
            }
            ++i;
            continue;
        }
        if (a.rfind("--audio-scope-window=", 0) == 0) {
            audio_scope_acquisition_option_seen = true;
            if (!parse_frame_count(a.substr(std::string("--audio-scope-window=").size()),
                                   r.audio_scope_window)) {
                r.error = "--audio-scope-window= requires an integer";
                return r;
            }
            if (r.audio_scope_window <= 0) {
                r.error = "--audio-scope-window must be > 0";
                return r;
            }
            continue;
        }
        if (a == "--audio-scope-trigger") {
            audio_scope_acquisition_option_seen = true;
            if (i + 1 >= args.size() || args[i + 1].empty()
                || args[i + 1][0] == '-') {
                r.error = "--audio-scope-trigger requires a value";
                return r;
            }
            r.audio_scope_trigger = args[++i];
            if (!valid_scope_trigger(r.audio_scope_trigger)) {
                r.error = "--audio-scope-trigger must be one of none, raw, off, rising-zero";
                return r;
            }
            continue;
        }
        if (a.rfind("--audio-scope-trigger=", 0) == 0) {
            audio_scope_acquisition_option_seen = true;
            r.audio_scope_trigger = a.substr(std::string("--audio-scope-trigger=").size());
            if (r.audio_scope_trigger.empty()) {
                r.error = "--audio-scope-trigger= requires a non-empty value";
                return r;
            }
            if (!valid_scope_trigger(r.audio_scope_trigger)) {
                r.error = "--audio-scope-trigger must be one of none, raw, off, rising-zero";
                return r;
            }
            continue;
        }
        if (a == "--audio-scope-channel") {
            audio_scope_acquisition_option_seen = true;
            if (i + 1 >= args.size()
                || !parse_nonnegative_int(args[i + 1], r.audio_scope_channel)) {
                r.error = "--audio-scope-channel requires a non-negative integer";
                return r;
            }
            ++i;
            continue;
        }
        if (a.rfind("--audio-scope-channel=", 0) == 0) {
            audio_scope_acquisition_option_seen = true;
            if (!parse_nonnegative_int(a.substr(std::string("--audio-scope-channel=").size()),
                                       r.audio_scope_channel)) {
                r.error = "--audio-scope-channel requires a non-negative integer";
                return r;
            }
            continue;
        }

        if (a == "--audio-capture-wav") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                r.error = "--audio-capture-wav requires a path argument";
                return r;
            }
            r.audio_capture_wav_path = args[++i];
            r.headless = true;
            continue;
        }
        if (a.rfind("--audio-capture-wav=", 0) == 0) {
            r.audio_capture_wav_path = a.substr(std::string("--audio-capture-wav=").size());
            if (r.audio_capture_wav_path.empty()) {
                r.error = "--audio-capture-wav= requires a non-empty path";
                return r;
            }
            r.headless = true;
            continue;
        }
        if (a == "--audio-capture-frames") {
            audio_capture_frames_option_seen = true;
            if (i + 1 >= args.size() || !parse_frame_count(args[i + 1], r.audio_capture_frames)
                || r.audio_capture_frames <= 0) {
                r.error = "--audio-capture-frames requires a positive integer";
                return r;
            }
            ++i;
            continue;
        }
        if (a.rfind("--audio-capture-frames=", 0) == 0) {
            audio_capture_frames_option_seen = true;
            if (!parse_frame_count(a.substr(std::string("--audio-capture-frames=").size()),
                                   r.audio_capture_frames)
                || r.audio_capture_frames <= 0) {
                r.error = "--audio-capture-frames requires a positive integer";
                return r;
            }
            continue;
        }

        if (a == "--audio-capture-rolling") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                r.error = "--audio-capture-rolling requires a path argument";
                return r;
            }
            r.audio_capture_rolling_path = args[++i];
            r.headless = true;
            continue;
        }
        if (a.rfind("--audio-capture-rolling=", 0) == 0) {
            r.audio_capture_rolling_path =
                a.substr(std::string("--audio-capture-rolling=").size());
            if (r.audio_capture_rolling_path.empty()) {
                r.error = "--audio-capture-rolling= requires a non-empty path";
                return r;
            }
            r.headless = true;
            continue;
        }
        if (a == "--audio-capture-rolling-frames") {
            audio_capture_rolling_frames_option_seen = true;
            if (i + 1 >= args.size()
                || !parse_frame_count(args[i + 1], r.audio_capture_rolling_frames)
                || r.audio_capture_rolling_frames <= 0) {
                r.error = "--audio-capture-rolling-frames requires a positive integer";
                return r;
            }
            ++i;
            continue;
        }
        if (a.rfind("--audio-capture-rolling-frames=", 0) == 0) {
            audio_capture_rolling_frames_option_seen = true;
            if (!parse_frame_count(
                    a.substr(std::string("--audio-capture-rolling-frames=").size()),
                    r.audio_capture_rolling_frames)
                || r.audio_capture_rolling_frames <= 0) {
                r.error = "--audio-capture-rolling-frames requires a positive integer";
                return r;
            }
            continue;
        }

        if (r.target_name.empty() && !a.empty() && a[0] != '-') {
            r.target_name = a;
            continue;
        }

        r.user_pass_through.push_back(a);
    }

    if (!r.audio_scope_json_path.empty() && r.audio_inspector) {
        r.error = "--audio-scope-json cannot be combined with --audio-inspector; "
                  "both consume the live capture FIFO";
    }
    if (audio_scope_acquisition_option_seen && r.audio_scope_json_path.empty()) {
        r.error = "--audio-scope-window, --audio-scope-trigger, and "
                  "--audio-scope-channel require --audio-scope-json";
    }
    // --audio-capture-wav is the third single-consumer of the one probe FIFO.
    if (!r.audio_capture_wav_path.empty()
        && (r.audio_inspector || !r.audio_scope_json_path.empty())) {
        r.error = "--audio-capture-wav cannot be combined with --audio-inspector or "
                  "--audio-scope-json; they share the one live capture FIFO";
    }
    if (audio_capture_frames_option_seen && r.audio_capture_wav_path.empty()) {
        r.error = "--audio-capture-frames requires --audio-capture-wav";
    }
    // --audio-capture-rolling has its own buffer, but the standalone runs exactly
    // one capture one-shot per invocation, so it cannot be combined with the
    // other single-consumer capture modes.
    if (!r.audio_capture_rolling_path.empty()
        && (r.audio_inspector || !r.audio_scope_json_path.empty()
            || !r.audio_capture_wav_path.empty()
            || !r.audio_probe_json_path.empty())) {
        r.error = "--audio-capture-rolling cannot be combined with --audio-inspector, "
                  "--audio-probe-json, --audio-scope-json, or --audio-capture-wav; "
                  "the standalone runs one capture mode per invocation";
    }
    if (audio_capture_rolling_frames_option_seen && r.audio_capture_rolling_path.empty()) {
        r.error = "--audio-capture-rolling-frames requires --audio-capture-rolling";
    }

    return r;
}

std::vector<std::string> assemble_launch_args(const ParseRunResult& opts) {
    std::vector<std::string> out;
    if (opts.headless) {
        out.push_back("--headless");
    }
    if (!opts.screenshot_path.empty()) {
        out.push_back("--screenshot");
        out.push_back(opts.screenshot_path);
    }
    if (opts.frames != 1) {
        out.push_back("--frames");
        out.push_back(std::to_string(opts.frames));
    }
    if (opts.audio_inspector) {
        out.push_back("--audio-inspector");
    }
    if (!opts.audio_probe_json_path.empty()) {
        out.push_back("--audio-probe-json");
        out.push_back(opts.audio_probe_json_path);
    }
    if (!opts.audio_scope_json_path.empty()) {
        out.push_back("--audio-scope-json");
        out.push_back(opts.audio_scope_json_path);
        out.push_back("--audio-scope-window");
        out.push_back(std::to_string(opts.audio_scope_window));
        out.push_back("--audio-scope-trigger");
        out.push_back(opts.audio_scope_trigger);
        out.push_back("--audio-scope-channel");
        out.push_back(std::to_string(opts.audio_scope_channel));
    }
    if (!opts.audio_capture_wav_path.empty()) {
        out.push_back("--audio-capture-wav");
        out.push_back(opts.audio_capture_wav_path);
        if (opts.audio_capture_frames > 0) {
            out.push_back("--audio-capture-frames");
            out.push_back(std::to_string(opts.audio_capture_frames));
        }
    }
    if (!opts.audio_capture_rolling_path.empty()) {
        out.push_back("--audio-capture-rolling");
        out.push_back(opts.audio_capture_rolling_path);
        if (opts.audio_capture_rolling_frames > 0) {
            out.push_back("--audio-capture-rolling-frames");
            out.push_back(std::to_string(opts.audio_capture_rolling_frames));
        }
    }
    for (const auto& a : opts.user_pass_through) {
        out.push_back(a);
    }
    return out;
}

}  // namespace pulp_cli

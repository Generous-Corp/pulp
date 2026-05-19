// cmd_run_parse.cpp — pure flag parser for `pulp run`.
//
// Split out from cmd_run.cpp so test_cli_run_options can link the parser
// without dragging in cli_common (and its filesystem / colour / project
// resolution helpers). #914.

#include "cmd_run.hpp"

#include <charconv>
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

}  // namespace

ParseRunResult parse_run_options(const std::vector<std::string>& args) {
    ParseRunResult r;
    bool after_separator = false;

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

        if (r.target_name.empty() && !a.empty() && a[0] != '-') {
            r.target_name = a;
            continue;
        }

        r.user_pass_through.push_back(a);
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
    for (const auto& a : opts.user_pass_through) {
        out.push_back(a);
    }
    return out;
}

}  // namespace pulp_cli

#pragma once

// Per-stage import timing for the design-import CLI.
//
// A successful import ends with one stdout line summarizing where the time
// went. Stage boundaries are steady_clock timestamps captured in the CLI's
// pipeline; a stage that does not run for the active mode (e.g. render
// without --validate) stays unset — absent, not zero.

#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace pulp::import_design {

struct StageTimings {
    using Clock = std::chrono::steady_clock;
    Clock::time_point load_start{};                  // start of source loading
    std::optional<Clock::time_point> content_ready;  // envelope content in memory
    std::optional<Clock::time_point> ir_ready;       // parsed to DesignIR
    std::optional<Clock::time_point> js_ready;       // generated Pulp JS
    std::optional<Clock::time_point> render_start;   // --validate headless render begins
    std::optional<Clock::time_point> render_done;    // --validate headless render finished
};

// Sub-second durations print as integer milliseconds ("52ms"); one second
// and up as seconds with two decimals ("4.59s").
inline std::string format_duration(StageTimings::Clock::duration d) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    if (ms < 1000) return std::to_string(ms) + "ms";
    std::ostringstream out;
    out << std::fixed << std::setprecision(2)
        << (static_cast<double>(ms) / 1000.0) << "s";
    return out.str();
}

// One-line breakdown, e.g.:
//   ✓ imported "ui" (1730 nodes) in 4.59s  — decode 3.81s · parse 52ms · codegen 310ms · render 402ms
// "decode" covers everything that produces the parseable envelope content
// (for `--from fig` that includes the Node decode subprocess — the honest
// cost the user waits for). Total is measured to the moment of printing, so
// it also absorbs writes/reports and can exceed the sum of the stages.
inline std::string format_import_timing_line(const StageTimings& t,
                                             const std::string& design_label,
                                             size_t node_count) {
    const auto now = StageTimings::Clock::now();
    std::ostringstream out;
    out << "✓ imported \"" << design_label << "\"";
    if (node_count > 0)
        out << " (" << node_count << " node" << (node_count == 1 ? "" : "s") << ")";
    out << " in " << format_duration(now - t.load_start) << "  — ";

    struct Segment {
        const char* name;
        std::optional<StageTimings::Clock::duration> duration;
    };
    const Segment segments[] = {
        {"decode", t.content_ready
                       ? std::optional(*t.content_ready - t.load_start)
                       : std::nullopt},
        {"parse", t.ir_ready && t.content_ready
                      ? std::optional(*t.ir_ready - *t.content_ready)
                      : std::nullopt},
        {"codegen", t.js_ready && t.ir_ready
                        ? std::optional(*t.js_ready - *t.ir_ready)
                        : std::nullopt},
        {"render", t.render_done && t.render_start
                       ? std::optional(*t.render_done - *t.render_start)
                       : std::nullopt},
    };
    bool first = true;
    for (const auto& seg : segments) {
        if (!seg.duration) continue;
        if (!first) out << " · ";
        first = false;
        out << seg.name << " " << format_duration(*seg.duration);
    }
    return out.str();
}

}  // namespace pulp::import_design

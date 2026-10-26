#pragma once

// Wall-clock stages of the browser-backed HTML import lane.
//
// The clock is a chain of laps: each lap attributes the time since the
// previous one to a named stage, so the stages always sum to the lane's
// elapsed time and nothing between two stage boundaries goes unattributed.
// A stage may carry parts measured by the code that ran it (the capture
// script's own phases, the validation render and diff); parts describe their
// stage and are never added to the total a second time.

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace pulp::import_design {

struct BrowserLaneStage {
    std::string name;
    double elapsed_ms = 0.0;
    std::vector<BrowserLaneStage> parts;
};

class BrowserLaneClock {
  public:
    using Clock = std::chrono::steady_clock;

    BrowserLaneClock() : last_(Clock::now()) {}

    // Attributes everything since the previous lap, up to `until`, to `name`.
    // A boundary recorded by a callee lands its time on the right side of the
    // split instead of on whichever stage the caller happens to close next.
    BrowserLaneStage& lap(std::string name, Clock::time_point until) {
        if (until < last_)
            until = last_;
        stages_.push_back({std::move(name), milliseconds(until - last_), {}});
        last_ = until;
        return stages_.back();
    }

    BrowserLaneStage& lap(std::string name) {
        return lap(std::move(name), Clock::now());
    }

    [[nodiscard]] const std::vector<BrowserLaneStage>& stages() const noexcept {
        return stages_;
    }

    [[nodiscard]] std::vector<BrowserLaneStage> take_stages() {
        return std::move(stages_);
    }

    static double milliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

  private:
    Clock::time_point last_;
    std::vector<BrowserLaneStage> stages_;
};

[[nodiscard]] inline double browser_lane_total_ms(const std::vector<BrowserLaneStage>& stages) {
    double total = 0.0;
    for (const auto& stage : stages)
        total += stage.elapsed_ms;
    return total;
}

// Sub-second durations print as integer milliseconds, longer ones as seconds
// with two decimals, matching the CLI's import summary line.
[[nodiscard]] inline std::string format_browser_lane_ms(double ms) {
    char buffer[32];
    if (ms < 1000.0)
        std::snprintf(buffer, sizeof(buffer), "%.0fms", ms);
    else
        std::snprintf(buffer, sizeof(buffer), "%.2fs", ms / 1000.0);
    return buffer;
}

// One line, stages in pipeline order, e.g.
//   Browser lane timing: 27.40s  — html-intake 12ms · browser-discovery 1.92s · ...
[[nodiscard]] inline std::string
format_browser_lane_timing_line(const std::vector<BrowserLaneStage>& stages) {
    std::string line =
        "Browser lane timing: " + format_browser_lane_ms(browser_lane_total_ms(stages)) + "  — ";
    bool first = true;
    for (const auto& stage : stages) {
        if (!first)
            line += " · ";
        first = false;
        line += stage.name + " " + format_browser_lane_ms(stage.elapsed_ms);
    }
    return line;
}

} // namespace pulp::import_design

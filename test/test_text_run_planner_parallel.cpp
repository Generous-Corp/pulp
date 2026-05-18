// test_text_run_planner_parallel.cpp — Pulp #2163, font v2 Slice 3.7.
//
// Parallel shaping: shape_batch fans out N inputs across futures.
// Asserts (a) per-input output matches the serial path, (b) empty
// batch + single-input batch behave correctly, (c) under thread
// contention the planner stays thread-safe (no crashes / data races
// caught by repeated stress over many iterations).

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/text_run_planner.hpp>
#include <pulp/canvas/font_options.hpp>

#ifdef PULP_HAS_SKIA
#include "include/core/SkTypeface.h"
#endif

#include <string>
#include <utility>
#include <vector>

using namespace pulp::canvas;

namespace {

FontOptions make_opts(const char* family, float size) {
    FontOptions opts;
    opts.family_stack.push_back(family);
    opts.size = size;
    return opts;
}

}  // namespace

TEST_CASE("shape_batch: empty input returns empty vector",
          "[font][parallel][issue-2163]") {
    auto out = TextRunPlanner::instance().shape_batch({});
    REQUIRE(out.empty());
}

TEST_CASE("shape_batch: single input matches serial shape()",
          "[font][parallel][issue-2163]") {
    auto serial = TextRunPlanner::instance().shape("Hello world", make_opts("Inter", 14.0f));
    auto batch  = TextRunPlanner::instance().shape_batch({
        {"Hello world", make_opts("Inter", 14.0f)},
    });
    REQUIRE(batch.size() == 1u);
    REQUIRE(batch.front().text == serial.text);
    REQUIRE(batch.front().total_width == serial.total_width);
    REQUIRE(batch.front().runs.size() == serial.runs.size());
}

TEST_CASE("shape_batch: returns results in input order",
          "[font][parallel][issue-2163]") {
    std::vector<std::pair<std::string, FontOptions>> inputs;
    for (int i = 0; i < 16; ++i) {
        inputs.emplace_back(std::string("label-") + std::to_string(i),
                            make_opts("Inter", 10.0f + static_cast<float>(i)));
    }
    auto out = TextRunPlanner::instance().shape_batch(inputs);
    REQUIRE(out.size() == inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        REQUIRE(out[i].text == inputs[i].first);
    }
}

TEST_CASE("shape_batch: 50 inputs all complete without crash",
          "[font][parallel][issue-2163]") {
    std::vector<std::pair<std::string, FontOptions>> inputs;
    inputs.reserve(50);
    const char* families[] = {"Inter", "JetBrains Mono", "system"};
    for (int i = 0; i < 50; ++i) {
        inputs.emplace_back(
            std::string("widget-") + std::to_string(i) + " label",
            make_opts(families[i % 3], 7.0f + static_cast<float>(i % 12)));
    }
    auto out = TextRunPlanner::instance().shape_batch(inputs);
    REQUIRE(out.size() == 50u);
    for (const auto& r : out) {
        REQUIRE(!r.text.empty());
    }
}

TEST_CASE("shape_batch: parallel + serial paths produce equivalent output",
          "[font][parallel][issue-2163]") {
    std::vector<std::pair<std::string, FontOptions>> inputs = {
        {"OSC", make_opts("Inter", 9.0f)},
        {"polywave generator", make_opts("Inter", 8.0f)},
        {"ENV", make_opts("Inter", 9.0f)},
        {"ADSR shaper", make_opts("Inter", 8.0f)},
    };
    auto batch = TextRunPlanner::instance().shape_batch(inputs);
    REQUIRE(batch.size() == inputs.size());

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto serial = TextRunPlanner::instance().shape(
            inputs[i].first, inputs[i].second);
        REQUIRE(batch[i].text == serial.text);
        REQUIRE(batch[i].total_width == serial.total_width);
    }
}

TEST_CASE("shape_batch: repeated stress run stays thread-safe",
          "[font][parallel][issue-2163]") {
    std::vector<std::pair<std::string, FontOptions>> inputs;
    for (int i = 0; i < 32; ++i) {
        inputs.emplace_back(std::string("stress-") + std::to_string(i),
                            make_opts("Inter", 12.0f));
    }
    for (int trial = 0; trial < 5; ++trial) {
        auto out = TextRunPlanner::instance().shape_batch(inputs);
        REQUIRE(out.size() == inputs.size());
    }
}

// Unified UX+DSP live-swap transaction (live-swap plan item 1.8). Verifies the
// all-or-nothing coordination with fake stages: a failure in a later stage rolls
// back the earlier applied stages in reverse, and an earlier failure never
// applies the later stages — so a content pack can't land a new UI on old DSP.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/live_swap_transaction.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace pulp::format::reload;

namespace {
// Build a stage that records apply/rollback into a shared log and applies with a
// fixed result.
SwapStage recording_stage(std::vector<std::string>& log, std::string name,
                          bool apply_result) {
    SwapStage s;
    s.name = name;
    s.apply = [&log, name, apply_result] {
        log.push_back("apply:" + name);
        return apply_result;
    };
    s.rollback = [&log, name] { log.push_back("rollback:" + name); };
    return s;
}
}  // namespace

TEST_CASE("live-swap transaction commits when every stage succeeds", "[reload][transaction][1.8]") {
    std::vector<std::string> log;
    std::vector<SwapStage> stages{
        recording_stage(log, "ux", true),
        recording_stage(log, "dsp", true),
    };
    auto r = apply_live_swap(stages);
    REQUIRE(r.ok);
    REQUIRE(r.failed_stage.empty());
    // Both applied, nothing rolled back.
    REQUIRE(log == std::vector<std::string>{"apply:ux", "apply:dsp"});
}

TEST_CASE("a failing later stage rolls back the earlier applied stages", "[reload][transaction][1.8]") {
    // UX applies, DSP rejects → UX must be rolled back (no new-UI-on-old-DSP).
    std::vector<std::string> log;
    std::vector<SwapStage> stages{
        recording_stage(log, "ux", true),
        recording_stage(log, "dsp", false),
    };
    auto r = apply_live_swap(stages);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.failed_stage == "dsp");
    REQUIRE(r.rollback_clean);
    // UX applied then rolled back; DSP applied (returned false) but is NOT rolled
    // back (it left no partial state — that is the atomicity contract).
    REQUIRE(log == std::vector<std::string>{"apply:ux", "apply:dsp", "rollback:ux"});
}

TEST_CASE("an early failure never applies the later stages", "[reload][transaction][1.8]") {
    // UX rejects → DSP must never be touched; nothing to roll back.
    std::vector<std::string> log;
    std::vector<SwapStage> stages{
        recording_stage(log, "ux", false),
        recording_stage(log, "dsp", true),
    };
    auto r = apply_live_swap(stages);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.failed_stage == "ux");
    REQUIRE(log == std::vector<std::string>{"apply:ux"});   // DSP never applied
}

TEST_CASE("rollback runs in reverse order across three stages", "[reload][transaction][1.8]") {
    std::vector<std::string> log;
    std::vector<SwapStage> stages{
        recording_stage(log, "a", true),
        recording_stage(log, "b", true),
        recording_stage(log, "c", false),
    };
    auto r = apply_live_swap(stages);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.failed_stage == "c");
    REQUIRE(log == std::vector<std::string>{
        "apply:a", "apply:b", "apply:c", "rollback:b", "rollback:a"});
}

TEST_CASE("a throwing stage is treated as failure and unwinds", "[reload][transaction][1.8]") {
    std::vector<std::string> log;
    SwapStage ux = recording_stage(log, "ux", true);
    SwapStage dsp;
    dsp.name = "dsp";
    dsp.apply = [&log] { log.push_back("apply:dsp"); throw std::runtime_error("boom"); return true; };
    std::vector<SwapStage> stages{ux, dsp};
    auto r = apply_live_swap(stages);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.failed_stage == "dsp");
    REQUIRE(r.detail.find("boom") != std::string::npos);
    REQUIRE(log == std::vector<std::string>{"apply:ux", "apply:dsp", "rollback:ux"});
}

TEST_CASE("a throwing rollback is best-effort and flagged, others still run", "[reload][transaction][1.8]") {
    std::vector<std::string> log;
    SwapStage a = recording_stage(log, "a", true);
    SwapStage b;   // b's rollback throws
    b.name = "b";
    b.apply = [&log] { log.push_back("apply:b"); return true; };
    b.rollback = [&log] { log.push_back("rollback:b"); throw std::runtime_error("rb"); };
    std::vector<SwapStage> stages{a, b, recording_stage(log, "c", false)};
    auto r = apply_live_swap(stages);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.failed_stage == "c");
    REQUIRE_FALSE(r.rollback_clean);   // b's rollback threw
    // The unwind still reached a after b threw.
    REQUIRE(log == std::vector<std::string>{
        "apply:a", "apply:b", "apply:c", "rollback:b", "rollback:a"});
}

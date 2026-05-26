// VST3 SMPTE → pulp::format::FrameRate mapping.
//
// Extracted from vst3_adapter.cpp into a Steinberg-SDK-free helper so
// the table is unit-testable without the VST3 SDK. Pins the
// regression for #2963 (Codex comment 3305434120): a `framesPerSecond
// == 60` mapping that also matched 59.94 (= 60 + pulldown).

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/vst3_frame_rate.hpp>
#include <pulp/format/processor.hpp>

using pulp::format::detail::vst3_frame_rate;
using FR = pulp::format::FrameRate;

TEST_CASE("vst3_frame_rate — integer SMPTE rates (no pulldown / no drop)",
          "[format][vst3][playhead]") {
    REQUIRE(vst3_frame_rate(24, /*pulldown=*/false, /*drop=*/false) == FR::fps_24);
    REQUIRE(vst3_frame_rate(25, false, false) == FR::fps_25);
    REQUIRE(vst3_frame_rate(30, false, false) == FR::fps_30);
    REQUIRE(vst3_frame_rate(60, false, false) == FR::fps_60);
}

TEST_CASE("vst3_frame_rate — 23.976 / 29.97 / 29.97-drop / 30-drop",
          "[format][vst3][playhead]") {
    // 23.976 = 24 + pulldown — Pulp's enum has no 23.976 entry, so we
    // collapse to fps_24 (documented in the helper header).
    REQUIRE(vst3_frame_rate(24, /*pulldown=*/true, /*drop=*/false) == FR::fps_24);
    // 29.97 = 30 + pulldown.
    REQUIRE(vst3_frame_rate(30, true, false) == FR::fps_29_97);
    // 29.97 drop = 30 + pulldown + drop.
    REQUIRE(vst3_frame_rate(30, true, true) == FR::fps_29_97_drop);
    // 30 drop = 30 + drop.
    REQUIRE(vst3_frame_rate(30, false, true) == FR::fps_30_drop);
}

// Regression: #2963 / Codex comment 3305434120 — the VST3 mapper used
// `else if (fps == 60)`, which also matched 59.94 (= 60 + pulldown).
// 59.94 sessions became indistinguishable from true 60fps and broke
// SMPTE / timecode math in plug-ins that trust ctx.frame_rate. The fix
// gates the 60→fps_60 case on `!pulldown`, leaving 59.94 to fall through
// to FrameRate::unknown (Pulp's enum has no 59.94 entry).
TEST_CASE("vst3_frame_rate — 59.94 must NOT map to fps_60 (#2963)",
          "[format][vst3][playhead][issue-2963]") {
    // 59.94 == 60 + pulldown.
    REQUIRE(vst3_frame_rate(60, /*pulldown=*/true, /*drop=*/false) != FR::fps_60);
    REQUIRE(vst3_frame_rate(60, /*pulldown=*/true, /*drop=*/false) == FR::unknown);
    // True 60 (no pulldown) still maps to fps_60.
    REQUIRE(vst3_frame_rate(60, /*pulldown=*/false, /*drop=*/false) == FR::fps_60);
}

TEST_CASE("vst3_frame_rate — unsupported integer rates fall through to unknown",
          "[format][vst3][playhead]") {
    // 50 has no Pulp enum entry.
    REQUIRE(vst3_frame_rate(50, false, false) == FR::unknown);
    // 23 / 0 / negative / huge — all unknown.
    REQUIRE(vst3_frame_rate(23, false, false) == FR::unknown);
    REQUIRE(vst3_frame_rate(0, false, false) == FR::unknown);
    REQUIRE(vst3_frame_rate(120, false, false) == FR::unknown);
}

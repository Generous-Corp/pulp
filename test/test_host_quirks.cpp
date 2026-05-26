#include <catch2/catch_test_macros.hpp>
#include <pulp/format/host_quirks.hpp>

using namespace pulp::format;

TEST_CASE("HostQuirks default-construct enables cheap defenses",
          "[format][host-quirks]") {
    HostQuirks q;
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
    // Expensive defenses default off.
    REQUIRE(q.fl_studio_setactive_process_mutex == false);
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.reaper_process_while_bypassed == false);
    REQUIRE(q.logic_au_channel_probe_cap == 64);
}

TEST_CASE("make_quirks_for unknown host returns cheap-defense defaults",
          "[format][host-quirks]") {
    auto q = make_quirks_for(HostType::Unknown, {});
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.logic_au_channel_probe_cap == 64);
    REQUIRE(q.cubase10_async_view_resize_queue == false);
}

TEST_CASE("make_quirks_for Cubase 10 flips Cubase-10 flags",
          "[format][host-quirks]") {
    auto q = make_quirks_for(HostType::Cubase, HostVersion{10, 0});
    REQUIRE(q.cubase10_async_view_resize_queue == true);
    REQUIRE(q.cubase10_param_gesture_ordering == true);
    REQUIRE(q.cubase10_fractional_scale_correction == true);
    // Cubase 9 row should NOT fire on Cubase 10.
    REQUIRE(q.cubase9_state_blob_size_validation == false);
}

TEST_CASE("make_quirks_for Cubase 9 fires the 9-only state-blob quirk",
          "[format][host-quirks]") {
    auto q = make_quirks_for(HostType::Cubase, HostVersion{9, 5});
    REQUIRE(q.cubase9_state_blob_size_validation == true);
    REQUIRE(q.cubase10_async_view_resize_queue == false);
}

TEST_CASE("make_quirks_for Logic caps channel probe at 8",
          "[format][host-quirks]") {
    auto q = make_quirks_for(HostType::LogicPro, HostVersion{11, 0});
    REQUIRE(q.logic_au_channel_probe_cap == 8);
    REQUIRE(q.logic_au_tail_time_conversion == true);
    // GarageBand shares the same Logic quirks.
    auto gb = make_quirks_for(HostType::GarageBand, HostVersion{});
    REQUIRE(gb.logic_au_channel_probe_cap == 8);
}

TEST_CASE("make_quirks_for Reaper flips all 6 Reaper flags",
          "[format][host-quirks]") {
    auto q = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(q.reaper_vst3_gesture_ordering == true);
    REQUIRE(q.reaper_process_while_bypassed == true);
    REQUIRE(q.reaper_keyboard_passthrough == true);
    REQUIRE(q.reaper_permissive_bus_arrangements == true);
    REQUIRE(q.reaper_anticipative_fx_buffer_variability == true);
    REQUIRE(q.reaper_midsession_setstate == true);
}

TEST_CASE("make_quirks_for Bitwig < 6 enables setBusArrangements-while-active workaround",
          "[format][host-quirks]") {
    auto old = make_quirks_for(HostType::Bitwig, HostVersion{5, 2});
    REQUIRE(old.bitwig_vst3_setbusarrangements_while_active == true);
    auto modern = make_quirks_for(HostType::Bitwig, HostVersion{6, 0});
    REQUIRE(modern.bitwig_vst3_setbusarrangements_while_active == false);
    // Linux-repaint always-on (no-op off Linux at adapter call site).
    REQUIRE(old.bitwig_vst3_linux_repaint_after_resize == true);
}

TEST_CASE("make_quirks_for Pro Tools flips all 3 AAX flags",
          "[format][host-quirks]") {
    auto q = make_quirks_for(HostType::ProTools, HostVersion{2024, 6});
    REQUIRE(q.pro_tools_aax_sidechain_negotiation == true);
    REQUIRE(q.pro_tools_aax_latency_callback_push == true);
    REQUIRE(q.pro_tools_aax_mono_second_bus == true);
}

TEST_CASE("make_quirks_for Ableton Live enables canResize workaround",
          "[format][host-quirks]") {
    auto q = make_quirks_for(HostType::AbletonLive, HostVersion{12, 0});
    REQUIRE(q.live_vst3_canresize_ignore == true);
    REQUIRE(q.live_vst3_windows_dpi_defer == true);
}

TEST_CASE("Nuendo treats as Cubase for quirk purposes",
          "[format][host-quirks]") {
    auto q = make_quirks_for(HostType::Nuendo, HostVersion{13, 0});
    REQUIRE(q.cubase10_async_view_resize_queue == true);
}

// ── macOS plan items 5.2 / 5.3 — Cubase header extraction. The Cubase
//    factory lives at `core/format/include/pulp/format/host_quirks/cubase.hpp`;
//    these isolation tests pin that Cubase doesn't fire other hosts'
//    flags, and that the version bands behave as documented. ──

TEST_CASE("make_quirks_for Cubase 10 does not fire Live / Wavelab / Bitwig flags",
          "[format][host-quirks][isolation]") {
    auto q = make_quirks_for(HostType::Cubase, HostVersion{12, 0});
    REQUIRE(q.cubase10_async_view_resize_queue == true);
    // Live / Wavelab / Bitwig flags must stay default-false.
    REQUIRE(q.live_vst3_canresize_ignore == false);
    REQUIRE(q.live_vst3_windows_dpi_defer == false);
    REQUIRE(q.wavelab_vst3_defer_activation == false);
    REQUIRE(q.wavelab_state_blob_fallback == false);
    REQUIRE(q.bitwig_vst3_linux_repaint_after_resize == false);
}

TEST_CASE("make_quirks_for Cubase 9 leaves Cubase 10 flags off",
          "[format][host-quirks][isolation]") {
    auto q = make_quirks_for(HostType::Cubase, HostVersion{9, 0});
    REQUIRE(q.cubase9_state_blob_size_validation == true);
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.cubase10_param_gesture_ordering == false);
    REQUIRE(q.cubase10_fractional_scale_correction == false);
}

TEST_CASE("make_quirks_for Cubase with unknown version stays defensive-default",
          "[format][host-quirks][isolation]") {
    // Unknown HostVersion (all zeros) — no version-keyed quirk should fire.
    auto q = make_quirks_for(HostType::Cubase, HostVersion{});
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.cubase9_state_blob_size_validation == false);
    // Cheap defenses still on.
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
}

// ── macOS plan item 5.4 — Ableton Live header extraction. Factory at
//    `core/format/include/pulp/format/host_quirks/ableton_live.hpp`. ──

TEST_CASE("make_quirks_for Ableton Live leaves Cubase / Wavelab flags off",
          "[format][host-quirks][isolation]") {
    auto q = make_quirks_for(HostType::AbletonLive, HostVersion{11, 0});
    REQUIRE(q.live_vst3_canresize_ignore == true);
    REQUIRE(q.live_vst3_windows_dpi_defer == true);
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.cubase9_state_blob_size_validation == false);
    REQUIRE(q.wavelab_vst3_defer_activation == false);
    REQUIRE(q.wavelab_state_blob_fallback == false);
    REQUIRE(q.reaper_process_while_bypassed == false);
}

// ── macOS plan item 5.6 — Wavelab dispatch (DAW-quirks rows 10 + 11).
//    New `HostType::Wavelab` + per-host header at
//    `core/format/include/pulp/format/host_quirks/wavelab.hpp`. ──

TEST_CASE("make_quirks_for Wavelab 11.1 fires both Wavelab flags",
          "[format][host-quirks][wavelab]") {
    auto q = make_quirks_for(HostType::Wavelab, HostVersion{11, 1});
    REQUIRE(q.wavelab_vst3_defer_activation == true);
    REQUIRE(q.wavelab_state_blob_fallback == true);
    // No cross-host bleed.
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.live_vst3_canresize_ignore == false);
}

TEST_CASE("make_quirks_for Wavelab 12 keeps both flags on (regression coverage)",
          "[format][host-quirks][wavelab]") {
    auto q = make_quirks_for(HostType::Wavelab, HostVersion{12, 0});
    REQUIRE(q.wavelab_vst3_defer_activation == true);
    REQUIRE(q.wavelab_state_blob_fallback == true);
}

TEST_CASE("make_quirks_for Wavelab 10 keeps activation-deferral OFF but state-blob fallback ON",
          "[format][host-quirks][wavelab]") {
    // Row 10 is documented as Wavelab 11+; row 11 is version-invariant.
    auto q = make_quirks_for(HostType::Wavelab, HostVersion{10, 5});
    REQUIRE(q.wavelab_vst3_defer_activation == false);
    REQUIRE(q.wavelab_state_blob_fallback == true);
}

TEST_CASE("make_quirks_for Wavelab unknown version keeps state-blob fallback on, defer off",
          "[format][host-quirks][wavelab]") {
    // Version-invariant state-blob row fires even when version is
    // undetected; activation-deferral stays off without 11+ evidence.
    auto q = make_quirks_for(HostType::Wavelab, HostVersion{});
    REQUIRE(q.wavelab_state_blob_fallback == true);
    REQUIRE(q.wavelab_vst3_defer_activation == false);
}

// ── macOS plan item 5.5 — Bitwig dispatch (DAW-quirks rows 8 + 9).
//    Factory extracted to `core/format/include/pulp/format/host_quirks/bitwig.hpp`. ──

TEST_CASE("make_quirks_for Bitwig unknown version treats as legacy (workaround on)",
          "[format][host-quirks][bitwig]") {
    // HostVersion{} defaults to {0,0,0}, which is_before(6,0) → true,
    // so the conservative workaround stays on for a misdetected older
    // Bitwig. Linux-repaint flag is always on (no-op off Linux).
    auto q = make_quirks_for(HostType::Bitwig, HostVersion{});
    REQUIRE(q.bitwig_vst3_linux_repaint_after_resize == true);
    REQUIRE(q.bitwig_vst3_setbusarrangements_while_active == true);
}

TEST_CASE("make_quirks_for Bitwig 6+ drops setBusArrangements workaround",
          "[format][host-quirks][bitwig]") {
    auto q = make_quirks_for(HostType::Bitwig, HostVersion{6, 5});
    REQUIRE(q.bitwig_vst3_setbusarrangements_while_active == false);
    REQUIRE(q.bitwig_vst3_linux_repaint_after_resize == true);
}

TEST_CASE("make_quirks_for Bitwig leaves Cubase / Live / FL flags off",
          "[format][host-quirks][bitwig][isolation]") {
    auto q = make_quirks_for(HostType::Bitwig, HostVersion{5, 2});
    REQUIRE(q.bitwig_vst3_setbusarrangements_while_active == true);
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.live_vst3_canresize_ignore == false);
    REQUIRE(q.fl_studio_setactive_process_mutex == false);
    REQUIRE(q.logic_au_channel_probe_cap == 64);
}

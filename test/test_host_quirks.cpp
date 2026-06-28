#include <catch2/catch_test_macros.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_quirks/cubase.hpp>
#include <pulp/format/host_quirks/digital_performer.hpp>
#include <pulp/format/host_quirks/pro_tools.hpp>
#include <pulp/format/host_quirks/reaper.hpp>
#include <pulp/format/host_quirks/studio_one.hpp>
#include <pulp/format/host_type.hpp>

#include <array>
#include <utility>

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

// ── Cubase host-quirk dispatch ──────────────────────────────────────────
//
// The Cubase factory lives at
// `core/format/include/pulp/format/host_quirks/cubase.hpp`; these
// isolation tests pin that Cubase doesn't fire other hosts' flags, and
// that the version bands behave as documented.

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

// ── Ableton Live host-quirk dispatch ────────────────────────────────────
//
// Factory: `core/format/include/pulp/format/host_quirks/ableton_live.hpp`.

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

// ── Wavelab host-quirk dispatch ─────────────────────────────────────────
//
// `HostType::Wavelab` routes through
// `core/format/include/pulp/format/host_quirks/wavelab.hpp`.

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

// ── Bitwig host-quirk dispatch ──────────────────────────────────────────
//
// Factory: `core/format/include/pulp/format/host_quirks/bitwig.hpp`.

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

// ── FL Studio host-quirk dispatch ───────────────────────────────────────
//
// Factory: `core/format/include/pulp/format/host_quirks/fl_studio.hpp`.

TEST_CASE("make_quirks_for FL Studio fires mutex + state-reader-skip flags",
          "[format][host-quirks][fl-studio]") {
    auto q = make_quirks_for(HostType::FLStudio, HostVersion{21, 0});
    REQUIRE(q.fl_studio_setactive_process_mutex == true);
    REQUIRE(q.fl_studio_state_reader_skip == true);
}

TEST_CASE("make_quirks_for FL Studio unknown version still fires both flags",
          "[format][host-quirks][fl-studio]") {
    // Both rows are version-invariant.
    auto q = make_quirks_for(HostType::FLStudio, HostVersion{});
    REQUIRE(q.fl_studio_setactive_process_mutex == true);
    REQUIRE(q.fl_studio_state_reader_skip == true);
}

TEST_CASE("make_quirks_for FL Studio leaves other hosts' flags off",
          "[format][host-quirks][fl-studio][isolation]") {
    auto q = make_quirks_for(HostType::FLStudio, HostVersion{20, 9});
    REQUIRE(q.fl_studio_setactive_process_mutex == true);
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.live_vst3_canresize_ignore == false);
    REQUIRE(q.wavelab_state_blob_fallback == false);
    REQUIRE(q.bitwig_vst3_linux_repaint_after_resize == false);
    REQUIRE(q.logic_au_channel_probe_cap == 64);
    REQUIRE(q.au_v3_bypass_dual_tracking == false);
}

// ── Logic Pro AU host-quirk dispatch ────────────────────────────────────
//
// Factory: `core/format/include/pulp/format/host_quirks/logic_pro.hpp`.

TEST_CASE("make_quirks_for Logic Pro keeps channel cap + tail-time conversion on",
          "[format][host-quirks][logic]") {
    // Pre-existing "Logic caps channel probe at 8" already covers the
    // happy path; this case pins that the extracted header keeps the
    // same flags on across the modern Logic releases.
    auto q = make_quirks_for(HostType::LogicPro, HostVersion{11, 0});
    REQUIRE(q.logic_au_channel_probe_cap == 8);
    REQUIRE(q.logic_au_tail_time_conversion == true);
    auto modern = make_quirks_for(HostType::LogicPro, HostVersion{12, 0});
    REQUIRE(modern.logic_au_channel_probe_cap == 8);
    REQUIRE(modern.logic_au_tail_time_conversion == true);
}

TEST_CASE("make_quirks_for GarageBand inherits Logic AU quirks via shared header",
          "[format][host-quirks][logic]") {
    auto q = make_quirks_for(HostType::GarageBand, HostVersion{10, 4});
    REQUIRE(q.logic_au_channel_probe_cap == 8);
    REQUIRE(q.logic_au_tail_time_conversion == true);
}

TEST_CASE("make_quirks_for Logic Pro leaves other hosts' flags off",
          "[format][host-quirks][logic][isolation]") {
    auto q = make_quirks_for(HostType::LogicPro, HostVersion{11, 0});
    REQUIRE(q.logic_au_channel_probe_cap == 8);
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.live_vst3_canresize_ignore == false);
    REQUIRE(q.wavelab_state_blob_fallback == false);
    REQUIRE(q.bitwig_vst3_linux_repaint_after_resize == false);
    REQUIRE(q.fl_studio_setactive_process_mutex == false);
    REQUIRE(q.reaper_process_while_bypassed == false);
}

// ── AU v3 cross-host dispatch ───────────────────────────────────────────
//
// Helper: `core/format/include/pulp/format/host_quirks/auv3_cross_host.hpp`,
// layered on Logic + GarageBand which expose an AU v3 surface.

TEST_CASE("make_quirks_for Logic Pro layers AU v3 cross-host flags on top",
          "[format][host-quirks][auv3]") {
    auto q = make_quirks_for(HostType::LogicPro, HostVersion{11, 0});
    REQUIRE(q.au_v3_bypass_dual_tracking == true);
    REQUIRE(q.au_v3_host_id_from_wrapper == true);
    // Logic AU quirks still on (layering doesn't clobber the per-host
    // flags).
    REQUIRE(q.logic_au_channel_probe_cap == 8);
    REQUIRE(q.logic_au_tail_time_conversion == true);
}

TEST_CASE("make_quirks_for GarageBand layers AU v3 cross-host flags on top",
          "[format][host-quirks][auv3]") {
    auto q = make_quirks_for(HostType::GarageBand, HostVersion{10, 4});
    REQUIRE(q.au_v3_bypass_dual_tracking == true);
    REQUIRE(q.au_v3_host_id_from_wrapper == true);
}

TEST_CASE("AU v3 cross-host flags stay off for non-AU-v3 hosts",
          "[format][host-quirks][auv3][isolation]") {
    auto cubase = make_quirks_for(HostType::Cubase, HostVersion{12, 0});
    REQUIRE(cubase.au_v3_bypass_dual_tracking == false);
    REQUIRE(cubase.au_v3_host_id_from_wrapper == false);

    auto live = make_quirks_for(HostType::AbletonLive, HostVersion{12, 0});
    REQUIRE(live.au_v3_bypass_dual_tracking == false);
    REQUIRE(live.au_v3_host_id_from_wrapper == false);

    auto bitwig = make_quirks_for(HostType::Bitwig, HostVersion{5, 0});
    REQUIRE(bitwig.au_v3_bypass_dual_tracking == false);
    REQUIRE(bitwig.au_v3_host_id_from_wrapper == false);

    auto reaper = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(reaper.au_v3_bypass_dual_tracking == false);
    REQUIRE(reaper.au_v3_host_id_from_wrapper == false);

    auto fl = make_quirks_for(HostType::FLStudio, HostVersion{21, 0});
    REQUIRE(fl.au_v3_bypass_dual_tracking == false);
    REQUIRE(fl.au_v3_host_id_from_wrapper == false);
}

// ── Cross-format defensive defaults ─────────────────────────────────────
//
// The cheap always-on defenses are seeded by the default-constructed
// `HostQuirks`; this group of tests pins that every host dispatch path
// (including the unknown / default lane) leaves them on, and that
// `detect_quirks()` preserves them too.

TEST_CASE("cross-format defensive defaults are on for every host",
          "[format][host-quirks][defaults]") {
    // Sample one host per category — Cubase / Nuendo (version-keyed),
    // Live + Wavelab (version-invariant), Bitwig (legacy with
    // workaround), FL Studio (just-shipped lane), Logic + GarageBand
    // (Apple AU lane with AU v3 layering), Reaper (workaround-heavy),
    // Pro Tools (AAX lane), and Unknown / Standalone / Other (default
    // lane). Every entry must keep the cheap defenses on.
    const auto cases = std::array{
        std::pair{HostType::Cubase,      HostVersion{12, 0}},
        std::pair{HostType::Nuendo,      HostVersion{13, 0}},
        std::pair{HostType::AbletonLive, HostVersion{11, 0}},
        std::pair{HostType::Wavelab,     HostVersion{11, 1}},
        std::pair{HostType::Bitwig,      HostVersion{5, 2}},
        std::pair{HostType::FLStudio,    HostVersion{21, 0}},
        std::pair{HostType::LogicPro,    HostVersion{11, 0}},
        std::pair{HostType::GarageBand,  HostVersion{10, 4}},
        std::pair{HostType::Reaper,      HostVersion{7, 20}},
        std::pair{HostType::ProTools,    HostVersion{2024, 6}},
        std::pair{HostType::Unknown,     HostVersion{}},
        std::pair{HostType::Standalone,  HostVersion{}},
        std::pair{HostType::Other,       HostVersion{}},
    };
    for (auto [host, version] : cases) {
        auto q = make_quirks_for(host, version);
        INFO("host=" << host_type_name(host)
             << " version=" << version.major << "." << version.minor);
        // Row 23 — bypass synthesis.
        REQUIRE(q.synthesize_bypass_parameter == true);
        // Row 24 — latency clamp to non-negative.
        REQUIRE(q.clamp_latency_to_nonneg == true);
        // Rows 25-26 — silence unsupported bus arrangements.
        REQUIRE(q.silence_unsupported_bus_arrangements == true);
    }
}

TEST_CASE("cross-format defensive defaults survive detect_quirks()",
          "[format][host-quirks][defaults]") {
    // detect_quirks() shells through detect_host_info() → the same
    // make_quirks_for() pipeline. Cheap defenses must be on no matter
    // which host (or no host) the runtime detects. NOTE: this invariant
    // assumes the default PULP_HOST_QUIRKS_DEFAULT_POLICY=all; the
    // separate "detect_quirks respects PULP_HOST_QUIRKS_DEFAULT_POLICY"
    // case below covers the validated_only / off policies explicitly.
#if !defined(PULP_HOST_QUIRKS_DEFAULT_POLICY_OFF)
    auto q = detect_quirks();
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
#endif
}

TEST_CASE("cross-format defensive defaults are the only flags on for Unknown host",
          "[format][host-quirks][defaults]") {
    // The default lane must not flip any host-gated flag — only the
    // cheap defenses should be on.
    auto q = make_quirks_for(HostType::Unknown, HostVersion{});
    // Cheap defenses on.
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
    // Every host-gated flag stays off.
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.cubase10_param_gesture_ordering == false);
    REQUIRE(q.cubase10_fractional_scale_correction == false);
    REQUIRE(q.cubase9_state_blob_size_validation == false);
    REQUIRE(q.live_vst3_canresize_ignore == false);
    REQUIRE(q.live_vst3_windows_dpi_defer == false);
    REQUIRE(q.bitwig_vst3_linux_repaint_after_resize == false);
    REQUIRE(q.bitwig_vst3_setbusarrangements_while_active == false);
    REQUIRE(q.wavelab_vst3_defer_activation == false);
    REQUIRE(q.wavelab_state_blob_fallback == false);
    REQUIRE(q.fl_studio_setactive_process_mutex == false);
    REQUIRE(q.fl_studio_state_reader_skip == false);
    REQUIRE(q.reaper_vst3_gesture_ordering == false);
    REQUIRE(q.reaper_process_while_bypassed == false);
    REQUIRE(q.reaper_keyboard_passthrough == false);
    REQUIRE(q.reaper_permissive_bus_arrangements == false);
    REQUIRE(q.reaper_anticipative_fx_buffer_variability == false);
    REQUIRE(q.reaper_midsession_setstate == false);
    REQUIRE(q.pro_tools_aax_sidechain_negotiation == false);
    REQUIRE(q.pro_tools_aax_latency_callback_push == false);
    REQUIRE(q.pro_tools_aax_mono_second_bus == false);
    REQUIRE(q.logic_au_channel_probe_cap == 64); // Default cap, not Logic's 8.
    REQUIRE(q.logic_au_tail_time_conversion == false);
    REQUIRE(q.au_v3_bypass_dual_tracking == false);
    REQUIRE(q.au_v3_host_id_from_wrapper == false);
    // iPlug2-audit flags also stay off on the Unknown lane.
    REQUIRE(q.skip_bus_arrangement_call == false);
    REQUIRE(q.tolerate_state_read_nontrue_status == false);
    REQUIRE(q.double_string_buffer_for_live_10_1_13 == false);
    REQUIRE(q.reaper_keyboard_only_space == false);
    REQUIRE(q.aax_vendor_version_unknown == false);
}

// ── iPlug2-quirks-audit-2026-05-25 lessons — new flags layered on top
//    of the existing host catalog. Each test pins exactly one quirk
//    behavior so a regression localizes immediately. ──

TEST_CASE("HostQuirks default-construct leaves iplug2-audit flags off",
          "[format][host-quirks]") {
    HostQuirks q;
    REQUIRE(q.skip_bus_arrangement_call == false);
    REQUIRE(q.tolerate_state_read_nontrue_status == false);
    REQUIRE(q.double_string_buffer_for_live_10_1_13 == false);
    REQUIRE(q.reaper_keyboard_only_space == false);
    REQUIRE(q.aax_vendor_version_unknown == false);
}

TEST_CASE("make_quirks_for Ardour skips setBusArrangements",
          "[format][host-quirks][ardour]") {
    auto q = make_quirks_for(HostType::Ardour, HostVersion{8, 0});
    REQUIRE(q.skip_bus_arrangement_call == true);
    // No cross-host bleed.
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.live_vst3_canresize_ignore == false);
    REQUIRE(q.wavelab_state_blob_fallback == false);
}

TEST_CASE("make_quirks_for Mixbus32C skips setBusArrangements",
          "[format][host-quirks][mixbus32c]") {
    auto q = make_quirks_for(HostType::Mixbus32C, HostVersion{10, 0});
    REQUIRE(q.skip_bus_arrangement_call == true);
    // Mixbus32C is its own host, not Ardour — should not pick up
    // unrelated flags from anywhere else either.
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.reaper_keyboard_only_space == false);
}

TEST_CASE("make_quirks_for Ardour with unknown version still skips bus arrangement",
          "[format][host-quirks][ardour]") {
    // Version-invariant: Ardour's setBusArrangements bug is documented
    // across every vintage and the fix is host-agnostic.
    auto q = make_quirks_for(HostType::Ardour, HostVersion{});
    REQUIRE(q.skip_bus_arrangement_call == true);
}

TEST_CASE("make_quirks_for Wavelab enables state-read tolerance flag",
          "[format][host-quirks][wavelab]") {
    auto q = make_quirks_for(HostType::Wavelab, HostVersion{11, 1});
    REQUIRE(q.tolerate_state_read_nontrue_status == true);
    // Other hosts should not pick up the Wavelab-specific tolerance.
    auto cubase = make_quirks_for(HostType::Cubase, HostVersion{12, 0});
    REQUIRE(cubase.tolerate_state_read_nontrue_status == false);
    auto live = make_quirks_for(HostType::AbletonLive, HostVersion{12, 0});
    REQUIRE(live.tolerate_state_read_nontrue_status == false);
}

TEST_CASE("make_quirks_for Live 10.1.13 doubles getString buffer",
          "[format][host-quirks][live]") {
    auto q = make_quirks_for(HostType::AbletonLive, HostVersion{10, 1, 13});
    REQUIRE(q.double_string_buffer_for_live_10_1_13 == true);
    // Cheap row-5/6 defenses still on.
    REQUIRE(q.live_vst3_canresize_ignore == true);
}

TEST_CASE("make_quirks_for Live 10.1.12 leaves buffer-doubling off",
          "[format][host-quirks][live]") {
    // Exact version gate — 10.1.12 does NOT trigger.
    auto q = make_quirks_for(HostType::AbletonLive, HostVersion{10, 1, 12});
    REQUIRE(q.double_string_buffer_for_live_10_1_13 == false);
    REQUIRE(q.live_vst3_canresize_ignore == true);
}

TEST_CASE("make_quirks_for Live 10.1.14 leaves buffer-doubling off",
          "[format][host-quirks][live]") {
    // Build after the broken one — fix landed, no doubling needed.
    auto q = make_quirks_for(HostType::AbletonLive, HostVersion{10, 1, 14});
    REQUIRE(q.double_string_buffer_for_live_10_1_13 == false);
}

TEST_CASE("make_quirks_for Live 11.x and 12.x leave buffer-doubling off",
          "[format][host-quirks][live]") {
    auto l11 = make_quirks_for(HostType::AbletonLive, HostVersion{11, 3, 25});
    REQUIRE(l11.double_string_buffer_for_live_10_1_13 == false);
    auto l12 = make_quirks_for(HostType::AbletonLive, HostVersion{12, 0});
    REQUIRE(l12.double_string_buffer_for_live_10_1_13 == false);
}

TEST_CASE("make_quirks_for Reaper enables keyboard-only-space flag",
          "[format][host-quirks][reaper]") {
    auto q = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(q.reaper_keyboard_only_space == true);
    // Existing Reaper flags still fire.
    REQUIRE(q.reaper_vst3_gesture_ordering == true);
    REQUIRE(q.reaper_keyboard_passthrough == true);
    // Non-Reaper host stays clean.
    auto cubase = make_quirks_for(HostType::Cubase, HostVersion{12, 0});
    REQUIRE(cubase.reaper_keyboard_only_space == false);
}

TEST_CASE("make_quirks_for Pro Tools enables aax-vendor-version-unknown flag",
          "[format][host-quirks][protools]") {
    auto q = make_quirks_for(HostType::ProTools, HostVersion{2024, 6});
    REQUIRE(q.aax_vendor_version_unknown == true);
    // Existing Pro Tools AAX flags still fire.
    REQUIRE(q.pro_tools_aax_sidechain_negotiation == true);
    REQUIRE(q.pro_tools_aax_latency_callback_push == true);
    // Non-Pro-Tools host stays clean.
    auto reaper = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(reaper.aax_vendor_version_unknown == false);
}

TEST_CASE("make_quirks_for Pro Tools with unknown version still sets the version-unknown flag",
          "[format][host-quirks][protools]") {
    // The whole point of aax_vendor_version_unknown is that callers
    // should NOT branch on HostVersion for Pro Tools. Verify it fires
    // regardless of what version was (mis)reported.
    auto q = make_quirks_for(HostType::ProTools, HostVersion{});
    REQUIRE(q.aax_vendor_version_unknown == true);
}

// ── REAPER per-host dispatch ────────────────────────────────────────────
//
// Main dispatch lives in
// `core/format/include/pulp/format/host_quirks/reaper.hpp::apply_reaper`,
// with the iPlug2-audit `apply_reaper_keyboard` factory layered on top.
// These isolation tests pin each symptom row to its own assertion so a
// regression localizes to the exact REAPER quirk that broke.

TEST_CASE("apply_reaper fires row 15 (VST3 gesture ordering) standalone",
          "[format][host-quirks][reaper][isolation]") {
    HostQuirks q;
    host_quirks::apply_reaper(q, HostVersion{7, 20});
    REQUIRE(q.reaper_vst3_gesture_ordering == true);
}

TEST_CASE("apply_reaper fires row R1 (process while bypassed) standalone",
          "[format][host-quirks][reaper][isolation]") {
    HostQuirks q;
    host_quirks::apply_reaper(q, HostVersion{7, 20});
    REQUIRE(q.reaper_process_while_bypassed == true);
}

TEST_CASE("apply_reaper fires row R2 (keyboard passthrough) standalone",
          "[format][host-quirks][reaper][isolation]") {
    HostQuirks q;
    host_quirks::apply_reaper(q, HostVersion{7, 20});
    REQUIRE(q.reaper_keyboard_passthrough == true);
}

TEST_CASE("apply_reaper fires row R3 (permissive bus arrangements) standalone",
          "[format][host-quirks][reaper][isolation]") {
    HostQuirks q;
    host_quirks::apply_reaper(q, HostVersion{7, 20});
    REQUIRE(q.reaper_permissive_bus_arrangements == true);
}

TEST_CASE("apply_reaper fires row R4 (anticipative-FX buffer variability) standalone",
          "[format][host-quirks][reaper][isolation]") {
    HostQuirks q;
    host_quirks::apply_reaper(q, HostVersion{7, 20});
    REQUIRE(q.reaper_anticipative_fx_buffer_variability == true);
}

TEST_CASE("apply_reaper fires row R6 (mid-session setState) standalone",
          "[format][host-quirks][reaper][isolation]") {
    HostQuirks q;
    host_quirks::apply_reaper(q, HostVersion{7, 20});
    REQUIRE(q.reaper_midsession_setstate == true);
}

TEST_CASE("apply_reaper does not flip the keyboard-only-space iPlug2 lesson",
          "[format][host-quirks][reaper][isolation]") {
    // The two REAPER factories must stay independently composable so
    // their validation tiers can evolve separately (Speculative for the
    // main 6 rows vs LessonOnly for the iPlug2-audit lesson). The main
    // apply_reaper(...) must not silently include the keyboard lesson.
    HostQuirks q;
    host_quirks::apply_reaper(q, HostVersion{7, 20});
    REQUIRE(q.reaper_keyboard_only_space == false);

    // The keyboard factory called alone must not flip the main 6 rows.
    HostQuirks kb;
    host_quirks::apply_reaper_keyboard(kb, HostVersion{7, 20});
    REQUIRE(kb.reaper_keyboard_only_space == true);
    REQUIRE(kb.reaper_process_while_bypassed == false);
    REQUIRE(kb.reaper_vst3_gesture_ordering == false);
}

TEST_CASE("apply_reaper leaves other hosts' flags off",
          "[format][host-quirks][reaper][isolation]") {
    HostQuirks q;
    host_quirks::apply_reaper(q, HostVersion{7, 20});
    REQUIRE(q.reaper_vst3_gesture_ordering == true);
    // No cross-host bleed — every non-REAPER row must stay default.
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.cubase9_state_blob_size_validation == false);
    REQUIRE(q.live_vst3_canresize_ignore == false);
    REQUIRE(q.wavelab_state_blob_fallback == false);
    REQUIRE(q.bitwig_vst3_linux_repaint_after_resize == false);
    REQUIRE(q.fl_studio_setactive_process_mutex == false);
    REQUIRE(q.logic_au_channel_probe_cap == 64);
    REQUIRE(q.au_v3_bypass_dual_tracking == false);
    REQUIRE(q.pro_tools_aax_sidechain_negotiation == false);
    REQUIRE(q.pro_tools_aax_latency_callback_push == false);
    REQUIRE(q.pro_tools_aax_mono_second_bus == false);
    REQUIRE(q.aax_vendor_version_unknown == false);
    REQUIRE(q.skip_bus_arrangement_call == false);
}

TEST_CASE("make_quirks_for Reaper routes through the extracted header",
          "[format][host-quirks][reaper][isolation]") {
    // Sanity: the dispatch table still produces the same struct after
    // the extraction — every Reaper main row + the keyboard lesson + the
    // cheap defenses must all be on. Mirrors the pre-extraction
    // behavior pinned by the "flips all 6 Reaper flags" case above.
    auto dispatched = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    HostQuirks direct;
    host_quirks::apply_reaper(direct, HostVersion{7, 20});
    host_quirks::apply_reaper_keyboard(direct, HostVersion{7, 20});

    REQUIRE(dispatched.reaper_vst3_gesture_ordering
            == direct.reaper_vst3_gesture_ordering);
    REQUIRE(dispatched.reaper_process_while_bypassed
            == direct.reaper_process_while_bypassed);
    REQUIRE(dispatched.reaper_keyboard_passthrough
            == direct.reaper_keyboard_passthrough);
    REQUIRE(dispatched.reaper_permissive_bus_arrangements
            == direct.reaper_permissive_bus_arrangements);
    REQUIRE(dispatched.reaper_anticipative_fx_buffer_variability
            == direct.reaper_anticipative_fx_buffer_variability);
    REQUIRE(dispatched.reaper_midsession_setstate
            == direct.reaper_midsession_setstate);
    REQUIRE(dispatched.reaper_keyboard_only_space
            == direct.reaper_keyboard_only_space);
    // Cheap defenses present in the dispatched struct but not on the
    // hand-composed `direct` one (which started from default-ctor) —
    // verify the dispatch keeps them on independently.
    REQUIRE(dispatched.synthesize_bypass_parameter == true);
}

// ── Pro Tools / AAX per-host dispatch ───────────────────────────────────
//
// Main dispatch lives in
// `core/format/include/pulp/format/host_quirks/pro_tools.hpp::apply_pro_tools`
// with the iPlug2-audit `apply_pro_tools_aax_vendor_version_unknown`
// factory layered on top. Per-symptom isolation pins each row.

TEST_CASE("apply_pro_tools fires row 16 (sidechain negotiation) standalone",
          "[format][host-quirks][protools][isolation]") {
    HostQuirks q;
    host_quirks::apply_pro_tools(q, HostVersion{2024, 6});
    REQUIRE(q.pro_tools_aax_sidechain_negotiation == true);
}

TEST_CASE("apply_pro_tools fires row 17 (latency callback push) standalone",
          "[format][host-quirks][protools][isolation]") {
    HostQuirks q;
    host_quirks::apply_pro_tools(q, HostVersion{2024, 6});
    REQUIRE(q.pro_tools_aax_latency_callback_push == true);
}

TEST_CASE("apply_pro_tools fires row 18 (mono second bus) standalone",
          "[format][host-quirks][protools][isolation]") {
    HostQuirks q;
    host_quirks::apply_pro_tools(q, HostVersion{2024, 6});
    REQUIRE(q.pro_tools_aax_mono_second_bus == true);
}

TEST_CASE("apply_pro_tools does not flip the aax-vendor-version-unknown iPlug2 lesson",
          "[format][host-quirks][protools][isolation]") {
    // The two Pro Tools factories must stay independently composable so
    // their validation tiers can evolve separately (Speculative for the
    // main 3 AAX rows vs LessonOnly for the iPlug2-audit lesson).
    HostQuirks q;
    host_quirks::apply_pro_tools(q, HostVersion{2024, 6});
    REQUIRE(q.aax_vendor_version_unknown == false);

    HostQuirks ver;
    host_quirks::apply_pro_tools_aax_vendor_version_unknown(ver, HostVersion{2024, 6});
    REQUIRE(ver.aax_vendor_version_unknown == true);
    REQUIRE(ver.pro_tools_aax_sidechain_negotiation == false);
    REQUIRE(ver.pro_tools_aax_latency_callback_push == false);
    REQUIRE(ver.pro_tools_aax_mono_second_bus == false);
}

TEST_CASE("apply_pro_tools leaves other hosts' flags off",
          "[format][host-quirks][protools][isolation]") {
    HostQuirks q;
    host_quirks::apply_pro_tools(q, HostVersion{2024, 6});
    REQUIRE(q.pro_tools_aax_sidechain_negotiation == true);
    // No cross-host bleed.
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.cubase9_state_blob_size_validation == false);
    REQUIRE(q.live_vst3_canresize_ignore == false);
    REQUIRE(q.wavelab_state_blob_fallback == false);
    REQUIRE(q.bitwig_vst3_linux_repaint_after_resize == false);
    REQUIRE(q.fl_studio_setactive_process_mutex == false);
    REQUIRE(q.reaper_process_while_bypassed == false);
    REQUIRE(q.reaper_keyboard_only_space == false);
    REQUIRE(q.logic_au_channel_probe_cap == 64);
    REQUIRE(q.au_v3_bypass_dual_tracking == false);
    REQUIRE(q.skip_bus_arrangement_call == false);
}

TEST_CASE("make_quirks_for Pro Tools routes through the extracted header",
          "[format][host-quirks][protools][isolation]") {
    // Sanity: the dispatch table still produces the same struct after
    // the extraction — every AAX main row + the version-unknown lesson
    // + cheap defenses must all be on.
    auto dispatched = make_quirks_for(HostType::ProTools, HostVersion{2024, 6});
    HostQuirks direct;
    host_quirks::apply_pro_tools(direct, HostVersion{2024, 6});
    host_quirks::apply_pro_tools_aax_vendor_version_unknown(direct, HostVersion{2024, 6});

    REQUIRE(dispatched.pro_tools_aax_sidechain_negotiation
            == direct.pro_tools_aax_sidechain_negotiation);
    REQUIRE(dispatched.pro_tools_aax_latency_callback_push
            == direct.pro_tools_aax_latency_callback_push);
    REQUIRE(dispatched.pro_tools_aax_mono_second_bus
            == direct.pro_tools_aax_mono_second_bus);
    REQUIRE(dispatched.aax_vendor_version_unknown
            == direct.aax_vendor_version_unknown);
    REQUIRE(dispatched.synthesize_bypass_parameter == true);
}

TEST_CASE("make_quirks_for_validated_only on Pro Tools leaves only cheap defenses",
          "[format][host-quirks][protools][tiers]") {
    // Every Pro Tools row is currently Speculative (AAX 16-18) or
    // LessonOnly (vendor-version-unknown), so the validated-only filter
    // should zero all of them and leave cheap defenses on.
    auto q = make_quirks_for_validated_only(HostType::ProTools, HostVersion{2024, 6});
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
    REQUIRE(q.pro_tools_aax_sidechain_negotiation == false);
    REQUIRE(q.pro_tools_aax_latency_callback_push == false);
    REQUIRE(q.pro_tools_aax_mono_second_bus == false);
    REQUIRE(q.aax_vendor_version_unknown == false);
}

// ── Validation-tier API ─────────────────────────────────────────────────
//
// Per-quirk validation tiers + the filter + the validated-only factory
// + the default-policy compile-time switch. The intent is that plugin
// authors can dial in exactly the accommodations they trust, without
// having to hand-zero individual fields.

TEST_CASE("kHostQuirksMeta tags cheap defenses as Validated",
          "[format][host-quirks][tiers]") {
    // Cheap defenses are covered by the "cross-format defensive
    // defaults are on for every host" case above.
    STATIC_REQUIRE(kHostQuirksMeta.synthesize_bypass_parameter
                   == QuirkStatus::Validated);
    STATIC_REQUIRE(kHostQuirksMeta.clamp_latency_to_nonneg
                   == QuirkStatus::Validated);
    STATIC_REQUIRE(kHostQuirksMeta.silence_unsupported_bus_arrangements
                   == QuirkStatus::Validated);
}

TEST_CASE("kHostQuirksMeta tags every host-gated row as Speculative",
          "[format][host-quirks][tiers]") {
    // Every per-host row currently has dispatch-table coverage + an
    // optional per-host header, but none has been bench-confirmed
    // against the real DAW. They start at Speculative; promote to
    // Validated as bench rows ship.
    STATIC_REQUIRE(kHostQuirksMeta.cubase10_async_view_resize_queue
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.cubase9_state_blob_size_validation
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.live_vst3_canresize_ignore
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.wavelab_state_blob_fallback
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.bitwig_vst3_setbusarrangements_while_active
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.fl_studio_setactive_process_mutex
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.logic_au_channel_probe_cap
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.logic_au_tail_time_conversion
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.au_v3_bypass_dual_tracking
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.au_v3_host_id_from_wrapper
                   == QuirkStatus::Speculative);
}

TEST_CASE("kHostQuirksMeta tags Reaper bench-proven rows as Validated",
          "[format][host-quirks][tiers]") {
    STATIC_REQUIRE(kHostQuirksMeta.reaper_process_while_bypassed
                   == QuirkStatus::Validated);
    STATIC_REQUIRE(kHostQuirksMeta.reaper_vst3_gesture_ordering
                   == QuirkStatus::Validated);
    STATIC_REQUIRE(kHostQuirksMeta.reaper_keyboard_passthrough
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.reaper_permissive_bus_arrangements
                   == QuirkStatus::Validated);
    STATIC_REQUIRE(kHostQuirksMeta.reaper_anticipative_fx_buffer_variability
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.reaper_midsession_setstate
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.pro_tools_aax_sidechain_negotiation
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.pro_tools_aax_latency_callback_push
                   == QuirkStatus::Speculative);
    STATIC_REQUIRE(kHostQuirksMeta.pro_tools_aax_mono_second_bus
                   == QuirkStatus::Speculative);
}

TEST_CASE("kHostQuirksMeta tags iPlug2-audit 2026-05-25 rows as LessonOnly",
          "[format][host-quirks][tiers]") {
    // New 2026-05-25 lessons — documented from vendor docs +
    // reproducer notes, no in-tree bench yet. Promote to Speculative
    // (or Validated) when the bench evidence ships.
    STATIC_REQUIRE(kHostQuirksMeta.skip_bus_arrangement_call
                   == QuirkStatus::LessonOnly);
    STATIC_REQUIRE(kHostQuirksMeta.tolerate_state_read_nontrue_status
                   == QuirkStatus::LessonOnly);
    STATIC_REQUIRE(kHostQuirksMeta.double_string_buffer_for_live_10_1_13
                   == QuirkStatus::LessonOnly);
    STATIC_REQUIRE(kHostQuirksMeta.reaper_keyboard_only_space
                   == QuirkStatus::LessonOnly);
    STATIC_REQUIRE(kHostQuirksMeta.aax_vendor_version_unknown
                   == QuirkStatus::LessonOnly);
}

TEST_CASE("apply_filter validated-only strips iPlug2-audit lessons",
          "[format][host-quirks][tiers]") {
    // All 5 new flags are LessonOnly — the validated-only filter must
    // zero every one of them on the affected hosts.
    auto ardour = make_quirks_for(HostType::Ardour, HostVersion{8, 0});
    REQUIRE(ardour.skip_bus_arrangement_call == true);
    apply_filter(ardour, kQuirkFilterValidatedOnly);
    REQUIRE(ardour.skip_bus_arrangement_call == false);

    auto wavelab = make_quirks_for(HostType::Wavelab, HostVersion{11, 1});
    REQUIRE(wavelab.tolerate_state_read_nontrue_status == true);
    apply_filter(wavelab, kQuirkFilterValidatedOnly);
    REQUIRE(wavelab.tolerate_state_read_nontrue_status == false);

    auto live = make_quirks_for(HostType::AbletonLive, HostVersion{10, 1, 13});
    REQUIRE(live.double_string_buffer_for_live_10_1_13 == true);
    apply_filter(live, kQuirkFilterValidatedOnly);
    REQUIRE(live.double_string_buffer_for_live_10_1_13 == false);

    auto reaper = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(reaper.reaper_keyboard_only_space == true);
    apply_filter(reaper, kQuirkFilterValidatedOnly);
    REQUIRE(reaper.reaper_keyboard_only_space == false);

    auto pt = make_quirks_for(HostType::ProTools, HostVersion{2024, 6});
    REQUIRE(pt.aax_vendor_version_unknown == true);
    apply_filter(pt, kQuirkFilterValidatedOnly);
    REQUIRE(pt.aax_vendor_version_unknown == false);
}

TEST_CASE("apply_filter validated-only strips Speculative + LessonOnly flags",
          "[format][host-quirks][tiers]") {
    // Build a Cubase 10 quirks bundle — all 3 Cubase 10 flags are
    // Speculative, the cheap defenses are Validated.
    auto q = make_quirks_for(HostType::Cubase, HostVersion{12, 0});
    REQUIRE(q.cubase10_async_view_resize_queue == true);
    REQUIRE(q.synthesize_bypass_parameter == true);

    apply_filter(q, kQuirkFilterValidatedOnly);

    // Cheap defenses survive (Validated).
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
    // Cubase 10 speculative flags zeroed.
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.cubase10_param_gesture_ordering == false);
    REQUIRE(q.cubase10_fractional_scale_correction == false);
}

TEST_CASE("apply_filter validated-only resets logic_au_channel_probe_cap",
          "[format][host-quirks][tiers]") {
    // Numeric field (Speculative) should reset to the cross-host
    // default cap (64) when filtered out, not stay at the
    // Logic-specific 8.
    auto q = make_quirks_for(HostType::LogicPro, HostVersion{11, 0});
    REQUIRE(q.logic_au_channel_probe_cap == 8);

    apply_filter(q, kQuirkFilterValidatedOnly);
    REQUIRE(q.logic_au_channel_probe_cap == 64);
    REQUIRE(q.logic_au_tail_time_conversion == false);
}

TEST_CASE("apply_filter kQuirkFilterOff zeros every field including cheap defenses",
          "[format][host-quirks][tiers]") {
    auto q = make_quirks_for(HostType::Cubase, HostVersion{12, 0});
    apply_filter(q, kQuirkFilterOff);

    REQUIRE(q.synthesize_bypass_parameter == false);
    REQUIRE(q.clamp_latency_to_nonneg == false);
    REQUIRE(q.silence_unsupported_bus_arrangements == false);
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.logic_au_channel_probe_cap == 64); // numeric default
}

TEST_CASE("apply_filter is idempotent",
          "[format][host-quirks][tiers]") {
    auto q = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    apply_filter(q, kQuirkFilterValidatedOnly);
    auto snapshot = q;
    apply_filter(q, kQuirkFilterValidatedOnly);
    // Same struct after a second filter call — no oscillation.
    REQUIRE(q.synthesize_bypass_parameter == snapshot.synthesize_bypass_parameter);
    REQUIRE(q.reaper_process_while_bypassed == snapshot.reaper_process_while_bypassed);
    REQUIRE(q.logic_au_channel_probe_cap == snapshot.logic_au_channel_probe_cap);
}

TEST_CASE("apply_filter custom: allow only LessonOnly keeps iPlug2-audit lessons",
          "[format][host-quirks][tiers]") {
    // LessonOnly-only filter: lets the iPlug2-audit catalog lessons
    // (currently the only LessonOnly tier rows on these two hosts)
    // ride while suppressing the validated cheap defenses and the
    // speculative per-host rows that items 5.8 + 5.9 promoted.
    auto reaper = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    QuirkFilter only_lesson{
        .allow_validated = false,
        .allow_speculative = false,
        .allow_lesson_only = true,
    };
    apply_filter(reaper, only_lesson);
    // LessonOnly REAPER row survives.
    REQUIRE(reaper.reaper_keyboard_only_space == true);
    // Speculative REAPER rows (post-5.8) zeroed.
    REQUIRE(reaper.reaper_process_while_bypassed == false);
    REQUIRE(reaper.reaper_midsession_setstate == false);
    REQUIRE(reaper.reaper_vst3_gesture_ordering == false);
    // Validated cheap defenses zeroed.
    REQUIRE(reaper.synthesize_bypass_parameter == false);

    auto pt = make_quirks_for(HostType::ProTools, HostVersion{2024, 6});
    apply_filter(pt, only_lesson);
    // LessonOnly Pro Tools row survives.
    REQUIRE(pt.aax_vendor_version_unknown == true);
    // Speculative Pro Tools AAX rows (post-5.9) zeroed.
    REQUIRE(pt.pro_tools_aax_sidechain_negotiation == false);
    REQUIRE(pt.pro_tools_aax_latency_callback_push == false);
    REQUIRE(pt.pro_tools_aax_mono_second_bus == false);
    REQUIRE(pt.synthesize_bypass_parameter == false);
}

TEST_CASE("make_quirks_for_validated_only matches make_quirks_for + filter",
          "[format][host-quirks][tiers]") {
    const auto v = HostVersion{12, 0};
    auto direct = make_quirks_for(HostType::Cubase, v);
    apply_filter(direct, kQuirkFilterValidatedOnly);
    auto factory = make_quirks_for_validated_only(HostType::Cubase, v);

    // Field-for-field equality on the subset we care about.
    REQUIRE(direct.synthesize_bypass_parameter == factory.synthesize_bypass_parameter);
    REQUIRE(direct.clamp_latency_to_nonneg == factory.clamp_latency_to_nonneg);
    REQUIRE(direct.cubase10_async_view_resize_queue
            == factory.cubase10_async_view_resize_queue);
    REQUIRE(direct.cubase10_param_gesture_ordering
            == factory.cubase10_param_gesture_ordering);
    REQUIRE(direct.cubase10_fractional_scale_correction
            == factory.cubase10_fractional_scale_correction);
    REQUIRE(direct.logic_au_channel_probe_cap == factory.logic_au_channel_probe_cap);
}

TEST_CASE("make_quirks_for_validated_only on Reaper keeps DAW-bench validated rows",
          "[format][host-quirks][tiers]") {
    auto q = make_quirks_for_validated_only(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
    REQUIRE(q.reaper_process_while_bypassed == true);
    REQUIRE(q.reaper_vst3_gesture_ordering == true);
    REQUIRE(q.reaper_keyboard_passthrough == false);
    REQUIRE(q.reaper_permissive_bus_arrangements == true);
    REQUIRE(q.reaper_anticipative_fx_buffer_variability == false);
    REQUIRE(q.reaper_midsession_setstate == false);
}

TEST_CASE("detect_quirks respects PULP_HOST_QUIRKS_DEFAULT_POLICY compile-time policy",
          "[format][host-quirks][tiers]") {
    // The runtime-detected `detect_quirks()` always returns at least
    // a valid struct; what we can assert here without knowing the
    // calling DAW is that the build-time policy invariant holds:
    //
    //   policy=off            -> cheap defenses are OFF on the returned struct
    //   policy=validated_only -> cheap defenses are ON, but no LessonOnly fires
    //   policy=all (default)  -> cheap defenses are ON; whatever else fires fires
    //
    // We can only check the policy that this binary was built with —
    // the macros are mutually exclusive and define-or-undefined.
    auto q = detect_quirks();
#if defined(PULP_HOST_QUIRKS_DEFAULT_POLICY_OFF)
    REQUIRE(q.synthesize_bypass_parameter == false);
    REQUIRE(q.clamp_latency_to_nonneg == false);
    REQUIRE(q.silence_unsupported_bus_arrangements == false);
    REQUIRE(q.logic_au_channel_probe_cap == 64);
#elif defined(PULP_HOST_QUIRKS_DEFAULT_POLICY_VALIDATED_ONLY)
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
    // Every currently-LessonOnly field MUST be at its default.
    REQUIRE(q.reaper_process_while_bypassed == false);
    REQUIRE(q.pro_tools_aax_sidechain_negotiation == false);
#else
    // Default ("all") — cheap defenses survive; speculative/lesson rows
    // depend on the detected host so we can only assert the invariants
    // that must always hold.
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
#endif
}

// ── 2026-05-26 iPlug2-audit batch — 4 additional clean-room lessons
//    sourced from each host's public vendor documentation + a Pulp
//    reproducer issue. Each row is wired through a dedicated per-host
//    factory (or layered on top of an existing one) so its validation
//    tier can evolve independently. ──

// REAPER AU v3 in-process preferredContentSize lesson (Pulp #3044).

TEST_CASE("apply_reaper_auv3_in_process fires the preferredContentSize-sync lesson",
          "[format][host-quirks][reaper][isolation][issue-3044]") {
    HostQuirks q;
    host_quirks::apply_reaper_auv3_in_process(q, HostVersion{7, 20});
    REQUIRE(q.reaper_auv3_in_process_preferred_size_sync == true);
    // Standalone factory must not touch the main 6 REAPER rows.
    REQUIRE(q.reaper_vst3_gesture_ordering == false);
    REQUIRE(q.reaper_keyboard_only_space == false);
}

TEST_CASE("apply_reaper alone does not flip the AU v3 in-process lesson",
          "[format][host-quirks][reaper][isolation][issue-3044]") {
    // Tier isolation: the main 6 REAPER rows (Speculative) must stay
    // composable independently of the iPlug2-audit lesson (LessonOnly).
    HostQuirks q;
    host_quirks::apply_reaper(q, HostVersion{7, 20});
    REQUIRE(q.reaper_auv3_in_process_preferred_size_sync == false);
}

TEST_CASE("make_quirks_for Reaper layers the AU v3 in-process lesson on top",
          "[format][host-quirks][reaper][issue-3044]") {
    auto q = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(q.reaper_auv3_in_process_preferred_size_sync == true);
    // Cross-host bleed: non-REAPER hosts must stay clean.
    auto cubase = make_quirks_for(HostType::Cubase, HostVersion{12, 0});
    REQUIRE(cubase.reaper_auv3_in_process_preferred_size_sync == false);
    auto logic = make_quirks_for(HostType::LogicPro, HostVersion{11, 0});
    REQUIRE(logic.reaper_auv3_in_process_preferred_size_sync == false);
}

// Studio One restart-component UI-thread lesson (Pulp #3045).

TEST_CASE("apply_studio_one fires the restart-component UI-thread lesson",
          "[format][host-quirks][studio-one][isolation][issue-3045]") {
    HostQuirks q;
    host_quirks::apply_studio_one(q, HostVersion{6, 5});
    REQUIRE(q.studio_one_restart_component_ui_thread == true);
    // No cross-host bleed.
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.live_vst3_canresize_ignore == false);
    REQUIRE(q.wavelab_state_blob_fallback == false);
}

TEST_CASE("apply_studio_one fires the lesson regardless of version",
          "[format][host-quirks][studio-one][issue-3045]") {
    // Version-invariant: the threading contract is the same across 5.x
    // and 6.x.
    HostQuirks q;
    host_quirks::apply_studio_one(q, HostVersion{});
    REQUIRE(q.studio_one_restart_component_ui_thread == true);
    HostQuirks q5;
    host_quirks::apply_studio_one(q5, HostVersion{5, 5});
    REQUIRE(q5.studio_one_restart_component_ui_thread == true);
}

TEST_CASE("make_quirks_for Studio One routes through the per-host header",
          "[format][host-quirks][studio-one][issue-3045]") {
    auto q = make_quirks_for(HostType::StudioOne, HostVersion{6, 5});
    REQUIRE(q.studio_one_restart_component_ui_thread == true);
    // Cheap defenses still on, no cross-host bleed.
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.reaper_keyboard_only_space == false);
    REQUIRE(q.fl_studio_setactive_process_mutex == false);
    // Other hosts must NOT pick up the Studio One flag.
    auto cubase = make_quirks_for(HostType::Cubase, HostVersion{12, 0});
    REQUIRE(cubase.studio_one_restart_component_ui_thread == false);
    auto reaper = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(reaper.studio_one_restart_component_ui_thread == false);
}

// Digital Performer param-list reload lesson (Pulp #3046).

TEST_CASE("apply_digital_performer fires the param-list reload lesson",
          "[format][host-quirks][digital-performer][isolation][issue-3046]") {
    HostQuirks q;
    host_quirks::apply_digital_performer(q, HostVersion{11, 0});
    REQUIRE(q.digital_performer_param_list_reload == true);
    // No cross-host bleed.
    REQUIRE(q.cubase10_async_view_resize_queue == false);
    REQUIRE(q.studio_one_restart_component_ui_thread == false);
}

TEST_CASE("make_quirks_for DigitalPerformer routes through the per-host header",
          "[format][host-quirks][digital-performer][issue-3046]") {
    auto q = make_quirks_for(HostType::DigitalPerformer, HostVersion{11, 0});
    REQUIRE(q.digital_performer_param_list_reload == true);
    // Cheap defenses still on, no cross-host bleed.
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.reaper_keyboard_only_space == false);
    REQUIRE(q.studio_one_restart_component_ui_thread == false);
    // Other hosts must NOT pick up the DP flag.
    auto cubase = make_quirks_for(HostType::Cubase, HostVersion{12, 0});
    REQUIRE(cubase.digital_performer_param_list_reload == false);
    auto reaper = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(reaper.digital_performer_param_list_reload == false);
}

TEST_CASE("apply_digital_performer is version-invariant",
          "[format][host-quirks][digital-performer][issue-3046]") {
    HostQuirks q;
    host_quirks::apply_digital_performer(q, HostVersion{});
    REQUIRE(q.digital_performer_param_list_reload == true);
    HostQuirks q12;
    host_quirks::apply_digital_performer(q12, HostVersion{12, 0});
    REQUIRE(q12.digital_performer_param_list_reload == true);
}

// Cubase 13+ MIDI CC parameter ID stability (Pulp #3047).

TEST_CASE("apply_cubase fires the Cubase 13+ MIDI CC stability flag on 13+",
          "[format][host-quirks][cubase][issue-3047]") {
    HostQuirks q;
    host_quirks::apply_cubase(q, HostVersion{13, 0});
    REQUIRE(q.cubase13_midi_cc_param_id_stable == true);
    // Existing Cubase 10+ flags still fire (layering doesn't clobber).
    REQUIRE(q.cubase10_async_view_resize_queue == true);
}

TEST_CASE("apply_cubase keeps the Cubase 13+ MIDI CC stability flag off on 12.x",
          "[format][host-quirks][cubase][issue-3047]") {
    HostQuirks q;
    host_quirks::apply_cubase(q, HostVersion{12, 0});
    REQUIRE(q.cubase13_midi_cc_param_id_stable == false);
    // Cubase 12 still gets all of the 10+ flags.
    REQUIRE(q.cubase10_async_view_resize_queue == true);
}

TEST_CASE("apply_cubase keeps the Cubase 13+ MIDI CC stability flag off on 11.x and 10.x",
          "[format][host-quirks][cubase][issue-3047]") {
    HostQuirks q10;
    host_quirks::apply_cubase(q10, HostVersion{10, 5});
    REQUIRE(q10.cubase13_midi_cc_param_id_stable == false);
    HostQuirks q11;
    host_quirks::apply_cubase(q11, HostVersion{11, 0});
    REQUIRE(q11.cubase13_midi_cc_param_id_stable == false);
}

TEST_CASE("make_quirks_for Nuendo 13 inherits the Cubase 13+ MIDI CC stability flag",
          "[format][host-quirks][cubase][issue-3047]") {
    // Nuendo dispatches through apply_cubase, so the Nuendo 13+ line
    // inherits row 3047 automatically.
    auto q = make_quirks_for(HostType::Nuendo, HostVersion{13, 0});
    REQUIRE(q.cubase13_midi_cc_param_id_stable == true);
}

TEST_CASE("Cubase 13+ MIDI CC stability flag stays off for non-Cubase hosts",
          "[format][host-quirks][cubase][isolation][issue-3047]") {
    auto reaper = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(reaper.cubase13_midi_cc_param_id_stable == false);
    auto live = make_quirks_for(HostType::AbletonLive, HostVersion{12, 0});
    REQUIRE(live.cubase13_midi_cc_param_id_stable == false);
    auto studio_one = make_quirks_for(HostType::StudioOne, HostVersion{6, 5});
    REQUIRE(studio_one.cubase13_midi_cc_param_id_stable == false);
    auto dp = make_quirks_for(HostType::DigitalPerformer, HostVersion{11, 0});
    REQUIRE(dp.cubase13_midi_cc_param_id_stable == false);
}

// Tier-filter coverage for the new flags.

TEST_CASE("validated-only filter zeroes the 4 new iPlug2-audit lessons",
          "[format][host-quirks][tiers][issue-3044][issue-3045][issue-3046][issue-3047]") {
    // All 4 land at LessonOnly/Speculative — none should survive a
    // validated-only filter.
    auto reaper = make_quirks_for_validated_only(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(reaper.reaper_auv3_in_process_preferred_size_sync == false);
    auto studio_one = make_quirks_for_validated_only(HostType::StudioOne, HostVersion{6, 5});
    REQUIRE(studio_one.studio_one_restart_component_ui_thread == false);
    auto dp = make_quirks_for_validated_only(HostType::DigitalPerformer, HostVersion{11, 0});
    REQUIRE(dp.digital_performer_param_list_reload == false);
    auto cubase = make_quirks_for_validated_only(HostType::Cubase, HostVersion{13, 0});
    REQUIRE(cubase.cubase13_midi_cc_param_id_stable == false);
}

TEST_CASE("kHostQuirksMeta tags the 4 new iPlug2-audit rows correctly",
          "[format][host-quirks][tiers][issue-3044][issue-3045][issue-3046][issue-3047]") {
    STATIC_REQUIRE(kHostQuirksMeta.reaper_auv3_in_process_preferred_size_sync
                   == QuirkStatus::LessonOnly);
    STATIC_REQUIRE(kHostQuirksMeta.studio_one_restart_component_ui_thread
                   == QuirkStatus::LessonOnly);
    STATIC_REQUIRE(kHostQuirksMeta.digital_performer_param_list_reload
                   == QuirkStatus::LessonOnly);
    // Cubase 13+ MIDI CC ID stability lands as Speculative because it
    // has a per-host header + per-symptom isolation tests, mirroring
    // the rest of the Cubase rows.
    STATIC_REQUIRE(kHostQuirksMeta.cubase13_midi_cc_param_id_stable
                   == QuirkStatus::Speculative);
}

// Header includes for per-host modules kept inline with the tests that
// exercise them.


// ─────────────────────────────────────────────────────────────────────
// Runtime policy gate, per-quirk override, and field enumeration.
// ─────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <optional>
#include <string_view>

namespace {
// Find one enumerated field by name; returns nullptr if absent.
const pulp::format::QuirkFieldStatus* find_field(
    const std::vector<pulp::format::QuirkFieldStatus>& fields,
    std::string_view name) {
    auto it = std::find_if(fields.begin(), fields.end(),
                           [&](const auto& f) { return f.name == name; });
    return it == fields.end() ? nullptr : &*it;
}

// RAII reset so a test never leaks runtime-policy state into the next.
struct QuirkPolicyGuard {
    QuirkPolicyGuard() { reset(); }
    ~QuirkPolicyGuard() { reset(); }
    static void reset() {
        pulp::format::set_host_quirk_policy(std::nullopt);
        pulp::format::clear_quirk_overrides();
    }
};
}  // namespace

TEST_CASE("resolve_quirk_policy: API policy wins and reports source=Api",
          "[format][host-quirks][runtime-policy][p2]") {
    QuirkPolicyGuard guard;
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterValidatedOnly);
    auto resolved = pulp::format::resolve_quirk_policy();
    REQUIRE(resolved.source == pulp::format::QuirkPolicySource::Api);
    REQUIRE(resolved.filter.allow_validated == true);
    REQUIRE(resolved.filter.allow_speculative == false);
    REQUIRE(resolved.filter.allow_lesson_only == false);
}

TEST_CASE("resolved_quirks validated-only keeps cheap defenses, drops speculative",
          "[format][host-quirks][runtime-policy][p2]") {
    QuirkPolicyGuard guard;
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterValidatedOnly);
    auto q = pulp::format::resolved_quirks(HostType::Reaper, HostVersion{7, 20});
    // Cheap defenses are Validated → survive.
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
    // Bench-validated REAPER rows survive; still-speculative rows remain
    // filtered out, even though make_quirks_for(Reaper) would have set them.
    REQUIRE(make_quirks_for(HostType::Reaper, HostVersion{7, 20})
                .reaper_vst3_gesture_ordering == true);
    REQUIRE(q.reaper_vst3_gesture_ordering == true);
    REQUIRE(q.reaper_process_while_bypassed == true);
    REQUIRE(q.reaper_permissive_bus_arrangements == true);
    REQUIRE(q.reaper_keyboard_passthrough == false);
}

TEST_CASE("resolved_quirks off-policy zeroes everything including cheap defenses",
          "[format][host-quirks][runtime-policy][p2]") {
    QuirkPolicyGuard guard;
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);
    auto q = pulp::format::resolved_quirks(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(q.synthesize_bypass_parameter == false);
    REQUIRE(q.clamp_latency_to_nonneg == false);
    REQUIRE(q.silence_unsupported_bus_arrangements == false);
    REQUIRE(q.reaper_vst3_gesture_ordering == false);
    // The int field reverts to its cross-host default.
    auto logic = pulp::format::resolved_quirks(HostType::LogicPro, HostVersion{11, 0});
    REQUIRE(logic.logic_au_channel_probe_cap == 64);
}

TEST_CASE("per-quirk override force-on exempts a tier-filtered flag",
          "[format][host-quirks][runtime-policy][override][p2]") {
    QuirkPolicyGuard guard;
    // Base policy drops every speculative REAPER row...
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterValidatedOnly);
    // ...but the author trusts this one specifically.
    pulp::format::set_quirk_override("reaper_keyboard_passthrough", true);
    auto q = pulp::format::resolved_quirks(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(q.reaper_keyboard_passthrough == true);    // forced on
    REQUIRE(q.reaper_midsession_setstate == false);    // still filtered
}

TEST_CASE("per-quirk override force-off beats an allowed flag (incl. int field)",
          "[format][host-quirks][runtime-policy][override][p2]") {
    QuirkPolicyGuard guard;
    pulp::format::set_host_quirk_policy(pulp::format::QuirkFilter{});  // all tiers
    pulp::format::set_quirk_override("synthesize_bypass_parameter", false);
    pulp::format::set_quirk_override("logic_au_channel_probe_cap", false);
    auto q = pulp::format::resolved_quirks(HostType::LogicPro, HostVersion{11, 0});
    REQUIRE(q.synthesize_bypass_parameter == false);     // forced off
    REQUIRE(q.clamp_latency_to_nonneg == true);          // untouched
    REQUIRE(q.logic_au_channel_probe_cap == 64);         // forced to default
}

TEST_CASE("set_quirk_override ignores unknown flag names",
          "[format][host-quirks][runtime-policy][override][p2]") {
    QuirkPolicyGuard guard;
    pulp::format::set_quirk_override("not_a_real_quirk_flag", true);
    // Resolution proceeds normally; the bogus name is a no-op.
    auto q = pulp::format::resolved_quirks(HostType::Unknown, HostVersion{});
    REQUIRE(q.synthesize_bypass_parameter == true);
}

TEST_CASE("enumerate_quirk_fields covers every field with tier + enforced",
          "[format][host-quirks][runtime-policy][doctor][p2]") {
    auto q = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    auto fields = pulp::format::enumerate_quirk_fields(q);
    // One row per HostQuirks field (kept in lock-step with the X-macro
    // count static_assert in host_quirks.cpp).
    REQUIRE(fields.size() == 37);

    const auto* bypass = find_field(fields, "synthesize_bypass_parameter");
    REQUIRE(bypass != nullptr);
    REQUIRE(bypass->tier == QuirkStatus::Validated);
    REQUIRE(bypass->enforced == true);  // cheap defense on by default

    const auto* reaper = find_field(fields, "reaper_vst3_gesture_ordering");
    REQUIRE(reaper != nullptr);
    REQUIRE(reaper->enforced == true);  // set for REAPER

    const auto* cubase = find_field(fields, "cubase10_async_view_resize_queue");
    REQUIRE(cubase != nullptr);
    REQUIRE(cubase->enforced == false);  // not a REAPER flag
}

TEST_CASE("enumerate_quirk_fields enforced reflects the int channel-probe cap",
          "[format][host-quirks][runtime-policy][doctor][p2]") {
    auto logic = enumerate_quirk_fields(make_quirks_for(HostType::LogicPro, HostVersion{11, 0}));
    const auto* cap = find_field(logic, "logic_au_channel_probe_cap");
    REQUIRE(cap != nullptr);
    REQUIRE(cap->enforced == true);  // Logic caps at 8 (!= default 64)

    auto unknown = enumerate_quirk_fields(make_quirks_for(HostType::Unknown, HostVersion{}));
    const auto* cap2 = find_field(unknown, "logic_au_channel_probe_cap");
    REQUIRE(cap2 != nullptr);
    REQUIRE(cap2->enforced == false);  // default 64 → not enforced
}

// ─────────────────────────────────────────────────────────────────────
// clamp_latency_to_nonneg enforcement — the accommodation helper that
// the VST3 / CLAP / AU adapters consume.
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("reported_latency_samples clamps negatives only when the quirk is on",
          "[format][host-quirks][p3][latency]") {
    HostQuirks on;  // default-constructed: clamp_latency_to_nonneg == true
    REQUIRE(on.clamp_latency_to_nonneg == true);
    REQUIRE(pulp::format::reported_latency_samples(-5, on) == 0);
    REQUIRE(pulp::format::reported_latency_samples(0, on) == 0);
    REQUIRE(pulp::format::reported_latency_samples(128, on) == 128);

    HostQuirks off = on;
    off.clamp_latency_to_nonneg = false;
    REQUIRE(pulp::format::reported_latency_samples(-5, off) == -5);  // raw through
    REQUIRE(pulp::format::reported_latency_samples(128, off) == 128);
}

TEST_CASE("latency clamp follows the runtime policy via resolved_quirks",
          "[format][host-quirks][p3][latency][runtime-policy]") {
    QuirkPolicyGuard guard;
    // Enforced under the 'all' policy (clamp_latency is Validated)...
    pulp::format::set_host_quirk_policy(pulp::format::QuirkFilter{});
    auto on = pulp::format::resolved_quirks(HostType::Reaper, HostVersion{7, 0});
    REQUIRE(on.clamp_latency_to_nonneg == true);
    REQUIRE(pulp::format::reported_latency_samples(-7, on) == 0);

    // ...and disabled when the user opts out via PULP_HOST_QUIRKS=off.
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);
    auto off = pulp::format::resolved_quirks(HostType::Reaper, HostVersion{7, 0});
    REQUIRE(off.clamp_latency_to_nonneg == false);
    REQUIRE(pulp::format::reported_latency_samples(-7, off) == -7);  // raw reported
}

// ─────────────────────────────────────────────────────────────────────
// maybe_synthesize_bypass helper (synthesize_bypass_parameter).
// ─────────────────────────────────────────────────────────────────────

#include <pulp/format/quirk_apply.hpp>
#include <pulp/state/store.hpp>

TEST_CASE("maybe_synthesize_bypass injects a Bypass param only when warranted",
          "[format][host-quirks][p3][bypass]") {
    using pulp::format::maybe_synthesize_bypass;
    using pulp::format::kSynthesizedBypassParamId;

    SECTION("synthesizes when enforced and no Bypass exists") {
        pulp::state::StateStore store;
        store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 24, 0, 0.1f}});
        HostQuirks q;  // synthesize_bypass_parameter defaults true
        REQUIRE(maybe_synthesize_bypass(store, q) == true);
        REQUIRE(store.param_count() == 2);
        // The synthesized one carries the reserved ID + boolean range.
        bool found = false;
        for (const auto& p : store.all_params()) {
            if (p.id == kSynthesizedBypassParamId) {
                found = true;
                REQUIRE(p.name == "Bypass");
                REQUIRE(p.range.step >= 1.0f);
                REQUIRE(p.range.min == 0.0f);
                REQUIRE(p.range.max == 1.0f);
            }
        }
        REQUIRE(found);
    }

    SECTION("no-op when the quirk is filtered out") {
        pulp::state::StateStore store;
        store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 24, 0, 0.1f}});
        HostQuirks q;
        q.synthesize_bypass_parameter = false;
        REQUIRE(maybe_synthesize_bypass(store, q) == false);
        REQUIRE(store.param_count() == 1);
    }

    SECTION("no-op when the plugin already declares a Bypass param") {
        pulp::state::StateStore store;
        store.add_parameter({.id = 5, .name = "Bypass", .range = {0, 1, 0, 1}});
        HostQuirks q;
        REQUIRE(maybe_synthesize_bypass(store, q) == false);
        REQUIRE(store.param_count() == 1);
    }

    SECTION("no-op when a param declares the Bypass designation under any name") {
        // A declared Bypass designation suppresses synthesis even though the
        // param is not named "Bypass" and is not in the legacy boolean shape —
        // the shared is_bypass_param contract recognizes it.
        pulp::state::StateStore store;
        pulp::state::ParamInfo declared;
        declared.id = 7;
        declared.name = "Active";
        declared.range = {0.0f, 1.0f, 1.0f};  // default ON, no step
        declared.designation = pulp::state::ParamDesignation::Bypass;
        store.add_parameter(declared);
        HostQuirks q;
        REQUIRE(maybe_synthesize_bypass(store, q) == false);
        REQUIRE(store.param_count() == 1);
    }
}

// The bypass-detection contract every format adapter (VST3 / AU v2 / AU v3 /
// CLAP) now shares via pulp::state::is_bypass_param. Proving the contract here
// proves all four adapters: each one resolves its bypass param through this
// single function rather than a re-implemented name/range check.
TEST_CASE("adapter bypass detection uses the shared designation-first contract",
          "[format][host-quirks][bypass][param-designation]") {
    using pulp::state::is_bypass_param;
    using pulp::state::ParamDesignation;
    using pulp::state::ParamInfo;

    SECTION("a declared Bypass designation is detected without relying on name") {
        ParamInfo p;
        p.id = 1;
        p.name = "Engine On";  // deliberately NOT "Bypass"
        p.range = {0.0f, 1.0f, 1.0f};
        p.designation = ParamDesignation::Bypass;
        REQUIRE(is_bypass_param(p));
    }

    SECTION("a legacy name-only boolean Bypass is still detected") {
        ParamInfo p;
        p.id = 2;
        p.name = "Bypass";
        p.range = {0.0f, 1.0f, 0.0f, 1.0f};  // boolean shape, no designation
        REQUIRE(p.designation == ParamDesignation::None);
        REQUIRE(is_bypass_param(p));
    }

    SECTION("a non-bypass param is never detected") {
        ParamInfo p;
        p.id = 3;
        p.name = "Gain";
        p.range = {-60.0f, 12.0f, 0.0f};
        REQUIRE_FALSE(is_bypass_param(p));
    }
}

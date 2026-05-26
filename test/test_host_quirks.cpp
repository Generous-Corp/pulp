#include <catch2/catch_test_macros.hpp>
#include <pulp/format/host_quirks.hpp>
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

// ── macOS plan item 5.7 — FL Studio dispatch (DAW-quirks rows 13 + 14).
//    Factory at `core/format/include/pulp/format/host_quirks/fl_studio.hpp`. ──

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

// ── macOS plan item 5.10 — Logic Pro AU dispatch (DAW-quirks rows
//    19 + 20). Factory extracted to
//    `core/format/include/pulp/format/host_quirks/logic_pro.hpp`. ──

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

// ── macOS plan item 5.11 — AU v3 cross-host dispatch (DAW-quirks rows
//    21 + 22). Helper at
//    `core/format/include/pulp/format/host_quirks/auv3_cross_host.hpp`,
//    layered on Logic + GarageBand which expose an AU v3 surface. ──

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

// ── macOS plan item 5.12 — Cross-format defensive defaults (DAW-quirks
//    rows 23-30). The cheap always-on defenses are seeded by the
//    default-constructed `HostQuirks`; this group of tests pins that
//    every host dispatch path (including the unknown / default lane)
//    leaves them on, and that `detect_quirks()` preserves them too. ──

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

// ── Validation-tier API (2026-05-25, item 5.15) ──
//
// Per-quirk validation tiers + the filter + the validated-only factory
// + the default-policy compile-time switch. The intent is that plugin
// authors can dial in exactly the accommodations they trust, without
// having to hand-zero individual fields.

TEST_CASE("kHostQuirksMeta tags cheap defenses as Validated",
          "[format][host-quirks][tiers]") {
    // Cheap defenses (rows 23–28) have been on the test bench since
    // item 5.1 — they're the founding assumption + are covered by the
    // "cross-format defensive defaults are on for every host" case
    // above.
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

TEST_CASE("kHostQuirksMeta tags Reaper / Pro Tools rows as LessonOnly",
          "[format][host-quirks][tiers]") {
    // Reaper + Pro Tools live in the dispatch table but have no
    // per-host header yet (see host_quirks.cpp). They're catalog
    // entries Pulp uses as lessons until the headers + bench evidence
    // ship.
    STATIC_REQUIRE(kHostQuirksMeta.reaper_process_while_bypassed
                   == QuirkStatus::LessonOnly);
    STATIC_REQUIRE(kHostQuirksMeta.reaper_vst3_gesture_ordering
                   == QuirkStatus::LessonOnly);
    STATIC_REQUIRE(kHostQuirksMeta.pro_tools_aax_sidechain_negotiation
                   == QuirkStatus::LessonOnly);
    STATIC_REQUIRE(kHostQuirksMeta.pro_tools_aax_mono_second_bus
                   == QuirkStatus::LessonOnly);
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

TEST_CASE("apply_filter custom: allow only LessonOnly keeps Reaper / Pro Tools flags",
          "[format][host-quirks][tiers]") {
    // LessonOnly-only filter: lets the (currently un-bench-validated)
    // Reaper rows ride while suppressing the validated cheap defenses
    // and the speculative per-host rows.
    auto q = make_quirks_for(HostType::Reaper, HostVersion{7, 20});
    QuirkFilter only_lesson{
        .allow_validated = false,
        .allow_speculative = false,
        .allow_lesson_only = true,
    };
    apply_filter(q, only_lesson);
    REQUIRE(q.reaper_process_while_bypassed == true);
    REQUIRE(q.reaper_midsession_setstate == true);
    // Validated cheap defenses zeroed.
    REQUIRE(q.synthesize_bypass_parameter == false);
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

TEST_CASE("make_quirks_for_validated_only on Reaper leaves only cheap defenses",
          "[format][host-quirks][tiers]") {
    auto q = make_quirks_for_validated_only(HostType::Reaper, HostVersion{7, 20});
    REQUIRE(q.synthesize_bypass_parameter == true);
    REQUIRE(q.clamp_latency_to_nonneg == true);
    REQUIRE(q.silence_unsupported_bus_arrangements == true);
    // Every Reaper row (currently LessonOnly) zeroed.
    REQUIRE(q.reaper_process_while_bypassed == false);
    REQUIRE(q.reaper_vst3_gesture_ordering == false);
    REQUIRE(q.reaper_keyboard_passthrough == false);
    REQUIRE(q.reaper_permissive_bus_arrangements == false);
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

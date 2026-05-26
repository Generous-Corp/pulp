#include <pulp/format/host_quirks.hpp>

#include <pulp/format/host_quirks/ableton_live.hpp>
#include <pulp/format/host_quirks/ardour.hpp>
#include <pulp/format/host_quirks/auv3_cross_host.hpp>
#include <pulp/format/host_quirks/bitwig.hpp>
#include <pulp/format/host_quirks/cubase.hpp>
#include <pulp/format/host_quirks/fl_studio.hpp>
#include <pulp/format/host_quirks/logic_pro.hpp>
#include <pulp/format/host_quirks/pro_tools.hpp>
#include <pulp/format/host_quirks/reaper.hpp>
#include <pulp/format/host_quirks/wavelab.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format {

namespace {

// Per-host quirk-population helpers. Extracted per-host modules under
// `core/format/include/pulp/format/host_quirks/<host>.hpp` expose an
// inline `host_quirks::apply_<host>(HostQuirks&, HostVersion)` that
// the dispatch table below routes to. Hosts without a dedicated header
// still live here as private helpers until the next batch extracts
// them. License-hygiene: every flag flipped here (or in a per-host
// header) must be backed by a host vendor doc + a reproducer Pulp
// issue. See the catalog at
// `planning/2026-05-24-daw-host-quirks-inheritance.md`.

void apply_reaper_quirks(HostQuirks& q, HostVersion v) {
    // Main 6 REAPER rows (15 + R1-R4 + R6) — extracted to the per-host
    // header so REAPER-specific lessons live in one place. Adapter
    // surface unchanged; this is a pure header refactor for item 5.8.
    host_quirks::apply_reaper(q, v);
    // Layer the iPlug2-audit keyboard-only-space lesson on top via the
    // separate factory so its LessonOnly tier can evolve independently
    // of the rest of the REAPER dispatch (Speculative).
    host_quirks::apply_reaper_keyboard(q, v);
}

void apply_pro_tools_quirks(HostQuirks& q, HostVersion v) {
    // Main 3 AAX rows (16-18) — extracted to the per-host header for
    // item 5.9 (opt-in lane gated on Avid SDK at the adapter level;
    // the quirk dispatch itself is unconditional so filtering behaves
    // identically across builds).
    host_quirks::apply_pro_tools(q, v);
    // Pro Tools' AAX wrapper does not surface a reliable vendor version
    // through the AAX spec, so version-gated decisions for this host
    // must be skipped entirely. Layered via the separate factory so its
    // LessonOnly tier can evolve independently of rows 16-18.
    host_quirks::apply_pro_tools_aax_vendor_version_unknown(q, v);
}

// Tier filtering. A single helper reduces a (status-tag, current value)
// pair to either the current value or the field's "off" value,
// depending on `filter`. Bool fields go to `false` when filtered out
// (so `kQuirkFilterOff` zeroes everything including cheap defenses).
// Numeric fields fall back to their default-constructed value (so the
// Logic channel-probe cap reverts to the cross-host 64, not to 0).
constexpr bool keep(QuirkStatus status, QuirkFilter filter) noexcept {
    switch (status) {
        case QuirkStatus::Validated:   return filter.allow_validated;
        case QuirkStatus::Speculative: return filter.allow_speculative;
        case QuirkStatus::LessonOnly:  return filter.allow_lesson_only;
    }
    return false;
}

constexpr void reset_if_filtered(bool& field, bool /*default_value*/,
                                 QuirkStatus status, QuirkFilter filter) noexcept {
    if (!keep(status, filter)) field = false;
}

constexpr void reset_if_filtered(int& field, int default_value,
                                 QuirkStatus status, QuirkFilter filter) noexcept {
    if (!keep(status, filter)) field = default_value;
}

// Compile-time default sentinel — used by reset_if_filtered to restore
// numeric fields to the value `HostQuirks{}` would yield (e.g. the
// Logic channel-probe cap to its cross-host default of 64). Keeping
// it as a constexpr instance (rather than building a fresh one per
// call) makes apply_filter() noexcept + cheap.
constexpr HostQuirks kDefaultHostQuirks{};

// Compile-time policy selection for `detect_quirks()`. The build-time
// option `PULP_HOST_QUIRKS_DEFAULT_POLICY` toggles `_VALIDATED_ONLY`
// or `_OFF`; default is "all quirks fire" (matches pre-tier behavior).
constexpr QuirkFilter default_policy_filter() noexcept {
#if defined(PULP_HOST_QUIRKS_DEFAULT_POLICY_OFF)
    return kQuirkFilterOff;
#elif defined(PULP_HOST_QUIRKS_DEFAULT_POLICY_VALIDATED_ONLY)
    return kQuirkFilterValidatedOnly;
#else
    return QuirkFilter{}; // all tiers allowed
#endif
}

} // namespace

void apply_filter(HostQuirks& q, QuirkFilter filter) {
    constexpr auto& m = kHostQuirksMeta;
    constexpr auto& d = kDefaultHostQuirks;

#define PULP_QUIRK_FILTER_FIELD(name) \
    reset_if_filtered(q.name, d.name, m.name, filter)

    // Cheap defenses (Validated)
    PULP_QUIRK_FILTER_FIELD(synthesize_bypass_parameter);
    PULP_QUIRK_FILTER_FIELD(clamp_latency_to_nonneg);
    PULP_QUIRK_FILTER_FIELD(silence_unsupported_bus_arrangements);

    // Cubase
    PULP_QUIRK_FILTER_FIELD(cubase10_async_view_resize_queue);
    PULP_QUIRK_FILTER_FIELD(cubase10_param_gesture_ordering);
    PULP_QUIRK_FILTER_FIELD(cubase10_fractional_scale_correction);
    PULP_QUIRK_FILTER_FIELD(cubase9_state_blob_size_validation);

    // Ableton Live
    PULP_QUIRK_FILTER_FIELD(live_vst3_canresize_ignore);
    PULP_QUIRK_FILTER_FIELD(live_vst3_windows_dpi_defer);
    PULP_QUIRK_FILTER_FIELD(double_string_buffer_for_live_10_1_13);

    // Bitwig
    PULP_QUIRK_FILTER_FIELD(bitwig_vst3_linux_repaint_after_resize);
    PULP_QUIRK_FILTER_FIELD(bitwig_vst3_setbusarrangements_while_active);

    // Ardour family (Ardour + Mixbus 32C)
    PULP_QUIRK_FILTER_FIELD(skip_bus_arrangement_call);

    // Wavelab
    PULP_QUIRK_FILTER_FIELD(wavelab_vst3_defer_activation);
    PULP_QUIRK_FILTER_FIELD(wavelab_state_blob_fallback);
    PULP_QUIRK_FILTER_FIELD(tolerate_state_read_nontrue_status);

    // FL Studio
    PULP_QUIRK_FILTER_FIELD(fl_studio_setactive_process_mutex);
    PULP_QUIRK_FILTER_FIELD(fl_studio_state_reader_skip);

    // Reaper
    PULP_QUIRK_FILTER_FIELD(reaper_vst3_gesture_ordering);
    PULP_QUIRK_FILTER_FIELD(reaper_process_while_bypassed);
    PULP_QUIRK_FILTER_FIELD(reaper_keyboard_passthrough);
    PULP_QUIRK_FILTER_FIELD(reaper_permissive_bus_arrangements);
    PULP_QUIRK_FILTER_FIELD(reaper_anticipative_fx_buffer_variability);
    PULP_QUIRK_FILTER_FIELD(reaper_midsession_setstate);
    PULP_QUIRK_FILTER_FIELD(reaper_keyboard_only_space);

    // Pro Tools
    PULP_QUIRK_FILTER_FIELD(pro_tools_aax_sidechain_negotiation);
    PULP_QUIRK_FILTER_FIELD(pro_tools_aax_latency_callback_push);
    PULP_QUIRK_FILTER_FIELD(pro_tools_aax_mono_second_bus);
    PULP_QUIRK_FILTER_FIELD(aax_vendor_version_unknown);

    // Logic Pro AU (one int field — same machinery)
    PULP_QUIRK_FILTER_FIELD(logic_au_channel_probe_cap);
    PULP_QUIRK_FILTER_FIELD(logic_au_tail_time_conversion);

    // AU v3 cross-host
    PULP_QUIRK_FILTER_FIELD(au_v3_bypass_dual_tracking);
    PULP_QUIRK_FILTER_FIELD(au_v3_host_id_from_wrapper);

#undef PULP_QUIRK_FILTER_FIELD
}

HostQuirks make_quirks_for(HostType type, HostVersion version) {
    HostQuirks q; // cheap defenses on by default
    switch (type) {
        case HostType::Cubase:
        case HostType::Nuendo:        host_quirks::apply_cubase(q, version); break;
        case HostType::AbletonLive:   host_quirks::apply_ableton_live(q, version); break;
        case HostType::Wavelab:       host_quirks::apply_wavelab(q, version); break;
        case HostType::Bitwig:        host_quirks::apply_bitwig(q, version); break;
        case HostType::FLStudio:      host_quirks::apply_fl_studio(q, version); break;
        case HostType::Reaper:        apply_reaper_quirks(q, version); break;
        case HostType::LogicPro:
        case HostType::GarageBand:
            // Logic + GarageBand share the AU v2 host stack (rows 19,
            // 20) and also expose an AU v3 surface (rows 21, 22). Apply
            // the per-host Logic helper first, then layer the AU v3
            // cross-host flags on top so adapters consulting either
            // surface see consistent state.
            host_quirks::apply_logic_pro(q, version);
            host_quirks::apply_auv3_cross_host(q, version);
            break;
        case HostType::ProTools:      apply_pro_tools_quirks(q, version); break;
        case HostType::Ardour:        host_quirks::apply_ardour(q, version); break;
        case HostType::Mixbus32C:     host_quirks::apply_mixbus32c(q, version); break;
        // StudioOne / DigitalPerformer / etc. land their flags here
        // when the per-host fixes ship in later batches.
        default: break;
    }
    return q;
}

HostQuirks make_quirks_for_validated_only(HostType type, HostVersion version) {
    auto q = make_quirks_for(type, version);
    apply_filter(q, kQuirkFilterValidatedOnly);
    return q;
}

HostQuirks detect_quirks() {
    const auto info = detect_host_info();
    auto q = make_quirks_for(info.type, info.version);
    apply_filter(q, default_policy_filter());
    return q;
}

}  // namespace pulp::format

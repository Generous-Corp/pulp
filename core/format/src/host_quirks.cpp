#include <pulp/format/host_quirks.hpp>

#include <pulp/format/host_quirks/cubase.hpp>
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

void apply_ableton_live_quirks(HostQuirks& q, HostVersion /*v*/) {
    q.live_vst3_canresize_ignore = true;
    q.live_vst3_windows_dpi_defer = true; // No-op on macOS/Linux.
}

void apply_bitwig_quirks(HostQuirks& q, HostVersion v) {
    q.bitwig_vst3_linux_repaint_after_resize = true; // No-op off Linux.
    if (v.is_before(6, 0)) {
        q.bitwig_vst3_setbusarrangements_while_active = true;
    }
}

void apply_reaper_quirks(HostQuirks& q, HostVersion /*v*/) {
    q.reaper_vst3_gesture_ordering = true;
    q.reaper_process_while_bypassed = true;
    q.reaper_keyboard_passthrough = true;
    q.reaper_permissive_bus_arrangements = true;
    q.reaper_anticipative_fx_buffer_variability = true;
    q.reaper_midsession_setstate = true;
}

void apply_logic_pro_quirks(HostQuirks& q, HostVersion /*v*/) {
    q.logic_au_channel_probe_cap = 8; // row 19 — Logic hangs above 8.
    q.logic_au_tail_time_conversion = true;
}

void apply_pro_tools_quirks(HostQuirks& q, HostVersion /*v*/) {
    q.pro_tools_aax_sidechain_negotiation = true;
    q.pro_tools_aax_latency_callback_push = true;
    q.pro_tools_aax_mono_second_bus = true;
}

} // namespace

HostQuirks make_quirks_for(HostType type, HostVersion version) {
    HostQuirks q; // cheap defenses on by default
    switch (type) {
        case HostType::Cubase:
        case HostType::Nuendo:        host_quirks::apply_cubase(q, version); break;
        case HostType::AbletonLive:   apply_ableton_live_quirks(q, version); break;
        case HostType::Bitwig:        apply_bitwig_quirks(q, version); break;
        case HostType::Reaper:        apply_reaper_quirks(q, version); break;
        case HostType::LogicPro:
        case HostType::GarageBand:    apply_logic_pro_quirks(q, version); break;
        case HostType::ProTools:      apply_pro_tools_quirks(q, version); break;
        // Wavelab / FL Studio / StudioOne / DigitalPerformer / etc. land
        // their flags here when the per-host fixes ship in batches L/N.
        default: break;
    }
    return q;
}

HostQuirks detect_quirks() {
    const auto info = detect_host_info();
    return make_quirks_for(info.type, info.version);
}

}  // namespace pulp::format

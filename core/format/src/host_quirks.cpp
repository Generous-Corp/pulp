#include <pulp/format/host_quirks.hpp>

#include <pulp/format/host_quirks/ableton_live.hpp>
#include <pulp/format/host_quirks/auv3_cross_host.hpp>
#include <pulp/format/host_quirks/bitwig.hpp>
#include <pulp/format/host_quirks/cubase.hpp>
#include <pulp/format/host_quirks/fl_studio.hpp>
#include <pulp/format/host_quirks/logic_pro.hpp>
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

void apply_reaper_quirks(HostQuirks& q, HostVersion /*v*/) {
    q.reaper_vst3_gesture_ordering = true;
    q.reaper_process_while_bypassed = true;
    q.reaper_keyboard_passthrough = true;
    q.reaper_permissive_bus_arrangements = true;
    q.reaper_anticipative_fx_buffer_variability = true;
    q.reaper_midsession_setstate = true;
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
        // StudioOne / DigitalPerformer / etc. land their flags here
        // when the per-host fixes ship in later batches.
        default: break;
    }
    return q;
}

HostQuirks detect_quirks() {
    const auto info = detect_host_info();
    return make_quirks_for(info.type, info.version);
}

}  // namespace pulp::format

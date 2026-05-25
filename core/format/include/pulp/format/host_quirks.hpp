#pragma once

/// @file host_quirks.hpp
/// Per-host accommodation flags consumed by format adapters.
///
/// Pulp's format adapters (VST3, AU v2, AU v3, CLAP, AAX) consult a
/// `HostQuirks` struct at init time to switch between defensive defaults
/// (always-on) and host-gated behaviors (only when a specific DAW /
/// version is detected). The full catalog of accommodations lives in
/// `planning/2026-05-24-daw-host-quirks-inheritance.md`.
///
/// Reviewer decision (2026-05-25): cheap defenses are always-on, expensive
/// defenses are host-gated. Always-on defaults are seeded by the
/// default-constructed `HostQuirks`; host-gated flags are turned on by
/// the per-host factory headers under `host_quirks/<host>.hpp`.
///
/// **License-hygiene contract**: every fix that flips a host-gated flag
/// must cite a host vendor doc + a reproducer Pulp issue. The commit
/// trailer `Reference-Lineage: cleanroom reproducer=#NNNN docs=<url>`
/// is required (advisory pre-push warning hint will catch missing
/// trailers on commits touching `core/format/host_quirks/`).

#include <pulp/format/host_type.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format {

/// Always-on / host-gated defensive behaviors that the adapters consult.
///
/// Cheap defenses (default-true) apply to every host — they cover spec
/// compliance and obvious safety nets. Expensive defenses (default-false)
/// only fire when a specific host is detected; the per-host module
/// factories under `host_quirks/<host>.hpp` populate the relevant flags.
struct HostQuirks {
    // ── Cheap defenses (always-on by default; rows 23–28 of the
    //    DAW-quirks catalog cross-format defaults) ──
    /// Synthesize a bypass parameter when the plugin doesn't declare one.
    bool synthesize_bypass_parameter = true;
    /// Clamp `Processor::latency_samples()` to non-negative when
    /// reporting to the host (VST3 requires unsigned).
    bool clamp_latency_to_nonneg = true;
    /// When a host-requested bus arrangement isn't supported, accept it
    /// and emit silence on the extra channels rather than failing.
    bool silence_unsupported_bus_arrangements = true;

    // ── Host-gated cheap-ish (default off, single host) ──

    // Cubase 10
    bool cubase10_async_view_resize_queue = false;     ///< row 1
    bool cubase10_param_gesture_ordering = false;      ///< row 2
    bool cubase10_fractional_scale_correction = false; ///< row 3

    // Cubase 9
    bool cubase9_state_blob_size_validation = false;   ///< row 4

    // Ableton Live
    bool live_vst3_canresize_ignore = false;           ///< row 5
    bool live_vst3_windows_dpi_defer = false;          ///< row 6 (Windows-only)

    // Bitwig
    bool bitwig_vst3_linux_repaint_after_resize = false; ///< row 8 (Linux-only)
    bool bitwig_vst3_setbusarrangements_while_active = false; ///< row 9

    // Wavelab
    bool wavelab_vst3_defer_activation = false;        ///< row 10
    bool wavelab_state_blob_fallback = false;          ///< row 11

    // FL Studio
    bool fl_studio_setactive_process_mutex = false;    ///< row 13
    bool fl_studio_state_reader_skip = false;          ///< row 14

    // Reaper (rows 15 + R1–R7)
    bool reaper_vst3_gesture_ordering = false;         ///< row 15
    bool reaper_process_while_bypassed = false;        ///< row R1
    bool reaper_keyboard_passthrough = false;          ///< row R2
    bool reaper_permissive_bus_arrangements = false;   ///< row R3
    bool reaper_anticipative_fx_buffer_variability = false; ///< row R4
    bool reaper_midsession_setstate = false;           ///< row R6

    // Pro Tools (AAX, opt-in)
    bool pro_tools_aax_sidechain_negotiation = false;  ///< row 16
    bool pro_tools_aax_latency_callback_push = false;  ///< row 17
    bool pro_tools_aax_mono_second_bus = false;        ///< row 18

    // Logic Pro AU
    int logic_au_channel_probe_cap = 64;               ///< row 19 (Logic = 8)
    bool logic_au_tail_time_conversion = false;        ///< row 20

    // AU v3 cross-host
    bool au_v3_bypass_dual_tracking = false;           ///< row 21
    bool au_v3_host_id_from_wrapper = false;           ///< row 22
};

/// Build a `HostQuirks` populated for the given host + version.
///
/// Default-constructed `HostQuirks` already turns on the cheap
/// always-on defenses. This factory layers the host-gated flags on top
/// based on the detected host (and version where the quirk is
/// version-keyed).
HostQuirks make_quirks_for(HostType type, HostVersion version);

/// Convenience: detect host + version, return the corresponding quirks.
HostQuirks detect_quirks();

}  // namespace pulp::format

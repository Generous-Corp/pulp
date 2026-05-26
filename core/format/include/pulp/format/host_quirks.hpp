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
///
/// ## Per-quirk validation tiers (2026-05-25)
///
/// Not every quirk has been bench-validated against a real DAW. We tag
/// each flag with one of three `QuirkStatus` tiers so plugin authors
/// can dial in exactly the accommodations they trust:
///
///   * `Validated` — exercised against the named DAW under a Pulp
///     regression test that observed the symptom + the fix.
///   * `Speculative` — implemented per host docs + reproducer report
///     but not yet confirmed in-DAW from a Pulp test bench. Default
///     ON when the host is detected, but a plugin author can opt out
///     via `QuirkFilter{ .allow_speculative = false }`.
///   * `LessonOnly` — the quirk row exists in the catalog as a
///     historical lesson; no Pulp fix is wired yet. Carried so the
///     enum + meta stay consistent with the catalog (the field's
///     default in `HostQuirks` is the safe value).
///
/// See `docs/reference/host-quirks-policy.md` for the full opt-in /
/// opt-out story (plugin-author surface) and
/// `planning/host-quirks-log.md` for the discovery log.

#include <pulp/format/host_type.hpp>
#include <pulp/format/host_version.hpp>

namespace pulp::format {

/// Validation tier for an individual host-quirk flag. See file header.
enum class QuirkStatus : unsigned char {
    Validated,   ///< Exercised by a Pulp regression test under the named DAW.
    Speculative, ///< Implemented per docs + reproducer; not yet bench-confirmed.
    LessonOnly,  ///< Catalog entry only; no Pulp fix wired (kept for record).
};

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
    /// Ableton Live 10.1.13 has a string-length parsing bug where the
    /// `IInfoListener::getString` callbacks for channel name, channel
    /// UID, and index-namespace queries write past the spec-required
    /// buffer length, corrupting adjacent memory. Adapters must allocate
    /// those buffers at twice the spec size when this flag is on. Only
    /// the 10.1.13 build is affected; the flag stays off on every other
    /// Live version.
    bool double_string_buffer_for_live_10_1_13 = false;

    // Bitwig
    bool bitwig_vst3_linux_repaint_after_resize = false; ///< row 8 (Linux-only)
    bool bitwig_vst3_setbusarrangements_while_active = false; ///< row 9

    // Ardour family (Ardour + Harrison Mixbus 32C)
    /// Ardour and its Mixbus 32C derivative ship a `setBusArrangements`
    /// implementation that returns success without applying the request,
    /// and may corrupt subsequent layout state if the plugin calls it.
    /// VST3 adapters must skip the call on these hosts and accept the
    /// host-published default arrangement instead.
    bool skip_bus_arrangement_call = false;

    // Wavelab
    bool wavelab_vst3_defer_activation = false;        ///< row 10
    bool wavelab_state_blob_fallback = false;          ///< row 11
    /// Wavelab's `IBStream::read` may return a non-`kResultTrue` status
    /// at end-of-stream while still having populated the supplied buffer.
    /// Strict callers reject the load and lose the parameter set; the
    /// VST3 adapter must accept the populated buffer when the read count
    /// matches the requested byte count even if the status is not
    /// `kResultTrue`.
    bool tolerate_state_read_nontrue_status = false;

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
    /// REAPER's VST2/VST3 keyboard pipeline only delivers a well-formed
    /// `keyMsg` payload for the Space key (VKEY_SPACE); other keys arrive
    /// with malformed key state and cannot be parsed reliably. Editors
    /// that route keyboard input must reject non-Space keys when this
    /// flag is set rather than acting on garbage data.
    bool reaper_keyboard_only_space = false;

    // Pro Tools (AAX, opt-in)
    bool pro_tools_aax_sidechain_negotiation = false;  ///< row 16
    bool pro_tools_aax_latency_callback_push = false;  ///< row 17
    bool pro_tools_aax_mono_second_bus = false;        ///< row 18
    /// Pro Tools' AAX wrapper does not reliably surface its host vendor
    /// version through the AAX specification — querying it can return
    /// zero or stale data. Adapters and quirks must treat the Pro Tools
    /// version as unknown and avoid version-gated branching for this
    /// host. (Always-true on Pro Tools, off elsewhere.)
    bool aax_vendor_version_unknown = false;

    // Logic Pro AU
    int logic_au_channel_probe_cap = 64;               ///< row 19 (Logic = 8)
    bool logic_au_tail_time_conversion = false;        ///< row 20

    // AU v3 cross-host
    bool au_v3_bypass_dual_tracking = false;           ///< row 21
    bool au_v3_host_id_from_wrapper = false;           ///< row 22
};

/// Per-quirk validation status, parallel to `HostQuirks`.
///
/// Field names match `HostQuirks` exactly. The constexpr
/// `kHostQuirksMeta` instance documents the current tier of every
/// flag; update the meta + the field's row in
/// `planning/host-quirks-log.md` together when a quirk graduates from
/// `Speculative` to `Validated` (or down to `LessonOnly`).
///
/// Cheap-defense fields (`synthesize_bypass_parameter`,
/// `clamp_latency_to_nonneg`, `silence_unsupported_bus_arrangements`)
/// are tagged `Validated` — they were the founding assumptions and
/// have ridden the test bench since item 5.1.
///
/// The Logic channel-probe cap is a numeric field (not a bool) but
/// carries a status tag like everything else; the filter helper resets
/// it back to the default cap (64) when its tier is filtered out.
struct HostQuirksMeta {
    QuirkStatus synthesize_bypass_parameter = QuirkStatus::Validated;
    QuirkStatus clamp_latency_to_nonneg = QuirkStatus::Validated;
    QuirkStatus silence_unsupported_bus_arrangements = QuirkStatus::Validated;

    QuirkStatus cubase10_async_view_resize_queue = QuirkStatus::Speculative;
    QuirkStatus cubase10_param_gesture_ordering = QuirkStatus::Speculative;
    QuirkStatus cubase10_fractional_scale_correction = QuirkStatus::Speculative;

    QuirkStatus cubase9_state_blob_size_validation = QuirkStatus::Speculative;

    QuirkStatus live_vst3_canresize_ignore = QuirkStatus::Speculative;
    QuirkStatus live_vst3_windows_dpi_defer = QuirkStatus::Speculative;
    // iPlug2-audit lesson (2026-05-25): exact-version Live 10.1.13
    // getString buffer-doubling — documented from release notes +
    // reproducer notes, no in-tree bench yet → LessonOnly.
    QuirkStatus double_string_buffer_for_live_10_1_13 = QuirkStatus::LessonOnly;

    // Bitwig + FL Studio + Logic + AU v3 cross-host all have per-host
    // headers under `host_quirks/<host>.hpp` (items 5.5 / 5.7 / 5.10 /
    // 5.11), reproducer docs in the catalog, and isolation tests.
    // Bench evidence under the real DAW is still pending → Speculative.
    QuirkStatus bitwig_vst3_linux_repaint_after_resize = QuirkStatus::Speculative;
    QuirkStatus bitwig_vst3_setbusarrangements_while_active = QuirkStatus::Speculative;

    // iPlug2-audit lesson (2026-05-25): Ardour + Mixbus 32C
    // setBusArrangements bypass — documented from Ardour docs +
    // reproducer notes, no in-tree bench yet → LessonOnly.
    QuirkStatus skip_bus_arrangement_call = QuirkStatus::LessonOnly;

    QuirkStatus wavelab_vst3_defer_activation = QuirkStatus::Speculative;
    QuirkStatus wavelab_state_blob_fallback = QuirkStatus::Speculative;
    // iPlug2-audit lessons (2026-05-25): documented from Steinberg
    // spec + reproducer notes, no in-tree bench yet → LessonOnly.
    QuirkStatus tolerate_state_read_nontrue_status = QuirkStatus::LessonOnly;

    QuirkStatus fl_studio_setactive_process_mutex = QuirkStatus::Speculative;
    QuirkStatus fl_studio_state_reader_skip = QuirkStatus::Speculative;

    // Reaper rows live in the dispatch table (host_quirks.cpp) but
    // have NO per-host header yet and no isolation tests. LessonOnly
    // until item 5.8 lands the header + per-symptom regression tests.
    QuirkStatus reaper_vst3_gesture_ordering = QuirkStatus::LessonOnly;
    QuirkStatus reaper_process_while_bypassed = QuirkStatus::LessonOnly;
    QuirkStatus reaper_keyboard_passthrough = QuirkStatus::LessonOnly;
    QuirkStatus reaper_permissive_bus_arrangements = QuirkStatus::LessonOnly;
    QuirkStatus reaper_anticipative_fx_buffer_variability = QuirkStatus::LessonOnly;
    QuirkStatus reaper_midsession_setstate = QuirkStatus::LessonOnly;
    // iPlug2-audit lesson (2026-05-25): REAPER keyboard only-Space —
    // documented across 5.x–7.x line, no in-tree bench yet → LessonOnly.
    QuirkStatus reaper_keyboard_only_space = QuirkStatus::LessonOnly;

    // Pro Tools AAX rows are gated on the developer-supplied Avid SDK
    // (item 5.9). Catalog entries until the AAX lane lands a header.
    QuirkStatus pro_tools_aax_sidechain_negotiation = QuirkStatus::LessonOnly;
    QuirkStatus pro_tools_aax_latency_callback_push = QuirkStatus::LessonOnly;
    QuirkStatus pro_tools_aax_mono_second_bus = QuirkStatus::LessonOnly;
    // iPlug2-audit lesson (2026-05-25): Pro Tools AAX vendor-version
    // unreliability — documented via Avid AAX docs, no bench → LessonOnly.
    QuirkStatus aax_vendor_version_unknown = QuirkStatus::LessonOnly;

    QuirkStatus logic_au_channel_probe_cap = QuirkStatus::Speculative;
    QuirkStatus logic_au_tail_time_conversion = QuirkStatus::Speculative;

    QuirkStatus au_v3_bypass_dual_tracking = QuirkStatus::Speculative;
    QuirkStatus au_v3_host_id_from_wrapper = QuirkStatus::Speculative;
};

/// Authoritative meta for the in-tree `HostQuirks`. Plugin authors and
/// tests should consult this rather than re-deriving the tier from
/// the field name. Edited in lock-step with the file header summary
/// and the row in `planning/host-quirks-log.md` for each promotion.
inline constexpr HostQuirksMeta kHostQuirksMeta{};

/// Selection of validation tiers a `HostQuirks` instance may keep.
///
/// Plugin authors who want to opt out of speculative accommodations
/// pass a filter to `apply_filter()` (or use the
/// `make_quirks_for_validated_only()` shortcut). Anything not allowed
/// is reset to its default-constructed value.
struct QuirkFilter {
    bool allow_validated = true;
    bool allow_speculative = true;
    bool allow_lesson_only = true;
};

/// Filter constant: keep only `Validated` tier accommodations.
inline constexpr QuirkFilter kQuirkFilterValidatedOnly{
    .allow_validated = true,
    .allow_speculative = false,
    .allow_lesson_only = false,
};

/// Filter constant: drop every accommodation (including cheap
/// defenses). Intended for diagnostic / fully-spec-compliant builds
/// — use with care.
inline constexpr QuirkFilter kQuirkFilterOff{
    .allow_validated = false,
    .allow_speculative = false,
    .allow_lesson_only = false,
};

/// In-place: zero out (reset to default) every field in `q` whose
/// meta tier is not allowed by `filter`. Numeric fields are reset to
/// their default-constructed value (`logic_au_channel_probe_cap` →
/// 64). Cheap defenses are not special-cased here — when their
/// `Validated` tier is filtered out, they too are reset.
///
/// Idempotent + safe to call multiple times.
void apply_filter(HostQuirks& q, QuirkFilter filter);

/// Build a `HostQuirks` populated for the given host + version.
///
/// Default-constructed `HostQuirks` already turns on the cheap
/// always-on defenses. This factory layers the host-gated flags on top
/// based on the detected host (and version where the quirk is
/// version-keyed).
HostQuirks make_quirks_for(HostType type, HostVersion version);

/// Same as `make_quirks_for(...)` followed by
/// `apply_filter(..., kQuirkFilterValidatedOnly)`. Convenience for
/// plugin authors who want to opt out of every accommodation that
/// hasn't been bench-validated yet — they get cheap defenses
/// (currently `Validated`) and any future `Validated` host quirks,
/// but no `Speculative` or `LessonOnly` rows.
HostQuirks make_quirks_for_validated_only(HostType type, HostVersion version);

/// Convenience: detect host + version, return the corresponding quirks.
///
/// Applies the default policy chosen at build time (CMake option
/// `PULP_HOST_QUIRKS_DEFAULT_POLICY`):
///
///   * `all` (default) — every detected quirk fires regardless of tier.
///   * `validated_only` — only `Validated` accommodations fire.
///   * `off` — no accommodations fire (returns a filtered-empty struct).
///
/// Plugin authors can still override at runtime by ignoring this
/// helper and constructing their own `HostQuirks` via
/// `make_quirks_for(...)` + `apply_filter(...)`.
HostQuirks detect_quirks();

}  // namespace pulp::format

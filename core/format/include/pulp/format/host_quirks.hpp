#pragma once

/// @file host_quirks.hpp
/// Per-host accommodation flags consumed by format adapters.
///
/// Pulp's format adapters (VST3, AU v2, AU v3, CLAP, AAX) consult a
/// `HostQuirks` struct at init time to switch between defensive defaults
/// (always-on) and host-gated behaviors (only when a specific DAW /
/// version is detected). The full catalog of accommodations lives in
/// `core/format/host-quirks.json`.
///
/// Cheap defenses are always-on, and expensive defenses are host-gated.
/// Always-on defaults are seeded by the
/// default-constructed `HostQuirks`; host-gated flags are turned on by
/// the per-host factory headers under `host_quirks/<host>.hpp`.
///
/// **License-hygiene contract**: every fix that flips a host-gated flag
/// must cite a host vendor doc + a reproducer Pulp issue. The commit
/// trailer `Reference-Lineage: cleanroom reproducer=#NNNN docs=<url>`
/// is required (advisory pre-push warning hint will catch missing
/// trailers on commits touching `core/format/host_quirks/`).
///
/// ## Per-quirk validation tiers
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
/// `core/format/host-quirks.json` for the current catalog.

#include <pulp/format/host_type.hpp>
#include <pulp/format/host_version.hpp>

#include <optional>
#include <string_view>
#include <vector>

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
    /// Logic AU v2 has no formal plugin-to-host resize callback, but its Cocoa
    /// host accepts an exact resize of the returned view's immediate container.
    bool logic_au_v2_container_resize = false;
    // NOTE (no runtime flag — handled in the AU v3 adapter, recorded here so the
    // DAW-quirks ledger captures it): Logic Pro AU v3 editor SIZING has two
    // confirmed behaviors a fixed-design GPU editor must accommodate:
    //   1. Logic locks the editor window to its selected view-config aspect
    //      (it offers only oversized ~4:3 configs), so a wide fixed design
    //      letterboxes. Fix: do NOT implement supportedViewConfigurations: /
    //      selectViewConfiguration: → Logic free-resizes to the design aspect.
    //   2. Logic (out-of-process) does NOT deliver the editor view's restored
    //      window size on first open, so a GPU surface created at the design
    //      size clips its first paint. Fix: defer GPU-host creation until the
    //      view reports a real settled size.
    // Both live in core/format/src/{au_adapter,au_view_controller_mac}.mm and
    // .agents/skills/auv3/SKILL.md. Verified live in Logic 2026-05-29.

    // AU v3 cross-host
    bool au_v3_bypass_dual_tracking = false;           ///< row 21
    bool au_v3_host_id_from_wrapper = false;           ///< row 22

    /// Reaper's AU v3 host is **in-process** (it creates the
    /// `AUAudioUnit` on the main thread via
    /// `createAudioUnitWithComponentDescription`), unlike Logic /
    /// GarageBand which run AU v3 plug-ins out-of-process. Setting the
    /// view controller's `preferredContentSize` only from `viewDidLoad`
    /// is too late on the in-process path — the editor opens at the
    /// extension's default size and snaps to the requested size one
    /// paint cycle later. AU v3 adapters must additionally set
    /// `preferredContentSize` synchronously in `audioUnitInitialized`
    /// (the in-process entry point); setting it twice is harmless on
    /// the OOP path. Layered on top of the REAPER VST3 dispatch via
    /// `apply_reaper_auv3_in_process`.
    bool reaper_auv3_in_process_preferred_size_sync = false;

    /// Studio One serializes `IComponentHandler::restartComponent`
    /// notifications through a non-thread-safe lane: audio-thread calls
    /// can deadlock the host or be dropped silently. The VST3 adapter
    /// must marshal restart-component calls (especially
    /// `kLatencyChanged`, which is the most common audio-thread
    /// emitter) to the UI thread when this flag is set. Cubase /
    /// Nuendo / Reaper accept the call from any thread.
    bool studio_one_restart_component_ui_thread = false;

    /// Digital Performer (MOTU) revives the edit controller on
    /// preset-load / project-import paths without re-querying the
    /// previously published parameter list. Plug-ins that mutate the
    /// parameter set via `restartComponent(kParamValuesChanged |
    /// kParamTitlesChanged)` end up with stale labels in DP's
    /// automation lane. The VST3 adapter must layer an additional
    /// `kReloadComponent` notification onto the standard restart bundle
    /// when the detected host is Digital Performer; Cubase / Logic /
    /// Reaper do not require this.
    bool digital_performer_param_list_reload = false;

    /// Cubase 13+ / Nuendo 13+ bind project automation lanes to the
    /// VST3 MIDI CC parameter IDs at save-time and refuse to re-map
    /// them on reload. Plug-ins that synthesize CC parameter IDs from
    /// dynamic state (e.g. preset-driven parameter counts) will see
    /// automation lanes orphaned on the 13.x line. The VST3 adapter
    /// must derive CC parameter IDs from a stable hash (not the
    /// runtime parameter index) when this flag is set. Cubase 12 was
    /// forgiving; the flag stays off on 12 and earlier.
    bool cubase13_midi_cc_param_id_stable = false;
};

/// Per-quirk validation status, parallel to `HostQuirks`.
///
/// Field names match `HostQuirks` exactly. The constexpr
/// `kHostQuirksMeta` instance documents the current tier of every
/// flag; update the meta + the field's row in
/// `core/format/host-quirks.json` together when a quirk graduates from
/// `Speculative` to `Validated` (or down to `LessonOnly`).
///
/// Cheap-defense fields (`synthesize_bypass_parameter`,
/// `clamp_latency_to_nonneg`, `silence_unsupported_bus_arrangements`)
/// are tagged `Validated` — they were the founding assumptions and
/// have ridden the test bench from the first cataloged defaults.
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
    // Exact-version Live 10.1.13 getString buffer-doubling —
    // documented from release notes + reproducer notes, no in-tree
    // bench yet → LessonOnly.
    QuirkStatus double_string_buffer_for_live_10_1_13 = QuirkStatus::LessonOnly;

    // Bitwig + FL Studio + Logic + AU v3 cross-host all have per-host
    // headers under `host_quirks/<host>.hpp`, reproducer docs in the
    // catalog, and isolation tests.
    // Bench evidence under the real DAW is still pending → Speculative.
    QuirkStatus bitwig_vst3_linux_repaint_after_resize = QuirkStatus::Speculative;
    QuirkStatus bitwig_vst3_setbusarrangements_while_active = QuirkStatus::Speculative;

    // Ardour + Mixbus 32C setBusArrangements bypass — documented from
    // Ardour docs + reproducer notes, no in-tree bench yet → LessonOnly.
    QuirkStatus skip_bus_arrangement_call = QuirkStatus::LessonOnly;

    QuirkStatus wavelab_vst3_defer_activation = QuirkStatus::Speculative;
    QuirkStatus wavelab_state_blob_fallback = QuirkStatus::Speculative;
    // Documented from Steinberg spec + reproducer notes, no in-tree
    // bench yet → LessonOnly.
    QuirkStatus tolerate_state_read_nontrue_status = QuirkStatus::LessonOnly;

    QuirkStatus fl_studio_setactive_process_mutex = QuirkStatus::Speculative;
    QuirkStatus fl_studio_state_reader_skip = QuirkStatus::Speculative;

    // Reaper rows have a per-host header (`host_quirks/reaper.hpp`) +
    // per-symptom isolation tests pinning the dispatch. Rows backed by
    // checked-in REAPER 7.x DAW-bench evidence are Validated; the rest
    // stay Speculative.
    QuirkStatus reaper_vst3_gesture_ordering = QuirkStatus::Validated;
    QuirkStatus reaper_process_while_bypassed = QuirkStatus::Validated;
    QuirkStatus reaper_keyboard_passthrough = QuirkStatus::Speculative;
    QuirkStatus reaper_permissive_bus_arrangements = QuirkStatus::Validated;
    QuirkStatus reaper_anticipative_fx_buffer_variability = QuirkStatus::Speculative;
    QuirkStatus reaper_midsession_setstate = QuirkStatus::Speculative;
    // REAPER keyboard only-Space — documented across 5.x–7.x line, no
    // in-tree bench yet → LessonOnly.
    QuirkStatus reaper_keyboard_only_space = QuirkStatus::LessonOnly;

    // Pro Tools AAX rows have a per-host header
    // (`host_quirks/pro_tools.hpp`) + per-symptom isolation tests.
    // AAX adapter use is gated on the developer-supplied Avid
    // SDK (`PULP_AAX_SDK`); the quirk dispatch itself is unconditional
    // so filter behavior matches across builds. In-DAW bench evidence
    // requires the SDK → Speculative until those rows ship.
    QuirkStatus pro_tools_aax_sidechain_negotiation = QuirkStatus::Speculative;
    QuirkStatus pro_tools_aax_latency_callback_push = QuirkStatus::Speculative;
    QuirkStatus pro_tools_aax_mono_second_bus = QuirkStatus::Speculative;
    // Pro Tools AAX vendor-version unreliability — documented via Avid
    // AAX docs, no bench → LessonOnly.
    QuirkStatus aax_vendor_version_unknown = QuirkStatus::LessonOnly;

    QuirkStatus logic_au_channel_probe_cap = QuirkStatus::Speculative;
    QuirkStatus logic_au_tail_time_conversion = QuirkStatus::Speculative;
    QuirkStatus logic_au_v2_container_resize = QuirkStatus::Validated;

    QuirkStatus au_v3_bypass_dual_tracking = QuirkStatus::Speculative;
    QuirkStatus au_v3_host_id_from_wrapper = QuirkStatus::Speculative;

    // Documented from each host's vendor docs + reproducer issue (Pulp
    // #3044 / #3045 / #3046 / #3047), no in-tree bench yet →
    // LessonOnly. Cubase 13+ MIDI CC ID stability lands as Speculative
    // because it has a per-host header + per-symptom isolation test,
    // mirroring the rest of the Cubase rows.
    QuirkStatus reaper_auv3_in_process_preferred_size_sync = QuirkStatus::LessonOnly;
    QuirkStatus studio_one_restart_component_ui_thread = QuirkStatus::LessonOnly;
    QuirkStatus digital_performer_param_list_reload = QuirkStatus::LessonOnly;
    QuirkStatus cubase13_midi_cc_param_id_stable = QuirkStatus::Speculative;
};

/// Authoritative meta for the in-tree `HostQuirks`. Plugin authors and
/// tests should consult this rather than re-deriving the tier from
/// the field name. Edited in lock-step with the file header summary
/// and the row in `core/format/host-quirks.json` for each promotion.
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
///
/// This routes through `resolved_quirks(...)`, so the runtime
/// policy (env / API / per-quirk override) applies here too. With no
/// runtime override set, the effective filter is the compile-time
/// default.
HostQuirks detect_quirks();

// ── Runtime policy ────────────────────────────────────────────────────
//
// The compile-time `PULP_HOST_QUIRKS_DEFAULT_POLICY` option sets the
// baseline tier filter. A runtime override layers on top so a user
// who doesn't trust a quirk can dial it back without recompiling, while
// the default stays ON (validated quirks enforced). Precedence, highest
// first:
//
//   per-quirk override  >  set_host_quirk_policy() (API)
//                       >  PULP_HOST_QUIRKS env  >  compile-time default
//
// All of this is init-time configuration — resolve once when the adapter
// is constructed. None of it is real-time safe; never call the setters
// from the audio thread.

/// Where the effective base tier-filter came from (for diagnostics —
/// `pulp doctor` reports this).
enum class QuirkPolicySource : unsigned char {
    CompileDefault,  ///< PULP_HOST_QUIRKS_DEFAULT_POLICY build option.
    Environment,     ///< PULP_HOST_QUIRKS environment variable.
    Api,             ///< set_host_quirk_policy().
};

/// The runtime-resolved base filter together with its source.
struct ResolvedQuirkPolicy {
    QuirkFilter filter;
    QuirkPolicySource source = QuirkPolicySource::CompileDefault;
};

/// Resolve the effective base tier-filter at runtime (does NOT apply
/// per-quirk overrides — those layer on inside `resolved_quirks`). The
/// `PULP_HOST_QUIRKS` env var is parsed once and cached; recognized
/// values (case-insensitive) are `off`, `validated-only`, and `all`. An
/// unrecognized value is ignored with a one-time warning and resolution
/// falls through to the next layer.
ResolvedQuirkPolicy resolve_quirk_policy();

/// Programmatically set the base tier-filter (highest base-layer
/// precedence, above the env var). Pass `std::nullopt` to clear it and
/// fall back to env / compile default. Init-time only.
void set_host_quirk_policy(std::optional<QuirkFilter> filter);

/// Force a single quirk flag on or off by its `HostQuirks` field name,
/// overriding the tier filter for just that flag. `enabled = true` keeps
/// the host-populated value even when its tier is filtered out;
/// `enabled = false` forces it back to the default (off). Unknown names
/// are ignored. Init-time only.
void set_quirk_override(std::string_view flag, bool enabled);

/// Clear all per-quirk overrides set via `set_quirk_override`.
void clear_quirk_overrides();

/// One row of resolved quirk state, for diagnostics (`pulp doctor`).
struct QuirkFieldStatus {
    std::string_view name;  ///< `HostQuirks` field name (static storage).
    QuirkStatus tier;       ///< Validation tier from `kHostQuirksMeta`.
    bool enforced;          ///< Accommodation active (bool true; int != default).
};

/// Enumerate every quirk field of `q` with its tier and whether it is
/// currently enforced, in `HostQuirks` declaration order. Used by
/// `pulp doctor` to render the host-quirks table.
std::vector<QuirkFieldStatus> enumerate_quirk_fields(const HostQuirks& q);

/// The single entry adapters consume: build the host's quirks, apply
/// the runtime base filter (`resolve_quirk_policy`), then layer per-quirk
/// overrides. Equivalent to `make_quirks_for` + the runtime policy.
HostQuirks resolved_quirks(HostType type, HostVersion version);

// ── Accommodation application helpers ─────────────────────────────────
//
// Small pure helpers that apply a single quirk to a value, so format
// adapters share one tested implementation instead of open-coding the
// accommodation at each call site. Each takes the resolved `HostQuirks`
// the adapter cached at init.

/// Apply `clamp_latency_to_nonneg` to a raw `Processor::latency_samples()`
/// value before reporting it to the host. When the quirk is enforced, a
/// negative latency is clamped to 0 (VST3/CLAP report unsigned, so a raw
/// negative would wrap to a huge value); when it is filtered out, the raw
/// value passes through unchanged. Pure + constexpr so it is unit-tested
/// directly without instantiating an adapter.
constexpr int reported_latency_samples(int raw, const HostQuirks& q) noexcept {
    return (q.clamp_latency_to_nonneg && raw < 0) ? 0 : raw;
}

}  // namespace pulp::format

#pragma once

// Tape machine — bake-layer catalog node.
//
// Wraps pulp::signal::TapeMachineT as a lowerable custom node, following the
// character delay's per-variant factory shape: one factory, three registered
// realizations with stable per-archetype type ids.
//
// STEREO, two ports in and two out — unlike the saturator's mono node, and not
// for symmetry's sake. This module has genuine cross-channel state: the
// crosstalk stage feeds each channel a tilted copy of the other, which is a
// headline specification on a professional machine and the thing two instanced
// mono nodes could not reproduce at all.
//
// ── What is a realization here, and what is a parameter ───────────────────
//
// Three things are fixed at registration rather than injected, each for a
// reason that would be a bug if ignored:
//
//   * **The archetype**, obviously — it is what the three registrations ARE.
//
//   * **The tape speed.** It looks like the most natural knob on the whole
//     machine, and it cannot be one. Changing speed reaches the reused physical
//     tier's speed-dependent minimum-phase loss FIR, which is REDESIGNED — a
//     cepstral fold over an FFT — and therefore allocates. A baked parameter is
//     applied on the audio thread; a baked parameter that allocates is a
//     dropout waiting for a busy session. It is also barely a control: the
//     cassette archetype has exactly one legal speed, and a knob with one
//     position is not a knob.
//
//   * **`pre_echo_enabled`.** It changes `latency_samples()`, and a node whose
//     reported latency moves under the audio thread breaks the host's delay
//     compensation — the same rule that makes the saturator's alias policy a
//     realization rather than a param.
//
// The EQ curve MAY be a parameter, which is the one case where this file departs
// from the saturator's "enums are realizations" instinct. It departs because on
// the machines being modelled the EQ standard is a FRONT-PANEL SWITCH — the
// A800's master EQ selector, the cassette deck's Type I/II tape-type selector —
// and because unlike a waveshaper shape it is allocation-free, latency-neutral,
// and confined to four coefficients of a first-order section whose entire state
// is one sample. It is omitted entirely when the archetype has only one legal
// curve; a one-position selector is implementation detail, not a capability.
//
// ── Control parameters are applied per BLOCK, not per sample ──────────────
//
// The saturator's node writes every setter once per sample, because its setters
// recompute three doubles. This one must not, and the ratio says why: `set_age`
// re-derives the reused loss cascade's parameters, redesigns a head-bump bell,
// and recomputes the reproduce alignment gain — which evaluates two FIR
// magnitudes over up to 385 taps each. That is on the order of a thousand
// floating-point operations to prepare ONE sample of audio, and it would be
// spent again on the next sample, and the next.
//
// So each declared parameter is read once per block, at offset 0, and applied
// only if it changed. Nothing musical is lost: these are machine-alignment
// controls — how worn the tape is, which EQ standard is selected, how much
// crosstalk the head stack has — not gestures. Sample-accurate automation of
// "how many hours are on this machine" is not a thing anyone wants.
//
// ── `worst_case_gain` is NOT APPLICABLE, deliberately ─────────────────────
//
// Stated rather than omitted, so a reviewer does not read an absent field as an
// oversight. This design has no feedback path: it is a pure insert, record
// chain then playback chain, both feed-forward. Series law 8's requirement —
// that a registry's `worst_case_gain` cite a bound the module's own suite
// asserts — has no subject here. `tape_machine_insertion_gain_bound()` supplies
// the number a reviewer actually wants instead, and it is asserted by the
// suite rather than estimated.

#include <pulp/host/detail/forge_realization_identity.hpp>
#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/custom_node_type.hpp>

#include <pulp/signal/tape_machine.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::tape {

using Archetype = signal::TapeArchetype;
using Curve = signal::TapeCurve;

// ── Stable type ids ───────────────────────────────────────────────────────
inline constexpr const char* kAmpexTypeId = "tape.ampex350_440";
inline constexpr const char* kStuderTypeId = "tape.studer_a800";
inline constexpr const char* kCassetteTypeId = "tape.cassette";

// ── Injectable param ids ──────────────────────────────────────────────────
// Node-local; the framework namespaces per node so two nodes never collide.
inline constexpr state::ParamID kEqCurve = 1;         // enum index, stepped
inline constexpr state::ParamID kBias = 2;            // −1 .. +1
inline constexpr state::ParamID kDrive = 3;           // 0 .. 1
inline constexpr state::ParamID kAge = 4;             // 0 .. 1
inline constexpr state::ParamID kCrosstalkDb = 5;     // dB, negative
inline constexpr state::ParamID kCompanding = 6;      // bool
inline constexpr state::ParamID kPrintThroughDb = 7;  // dB, negative
inline constexpr state::ParamID kPrintOffsetMs = 8;   // ms
inline constexpr state::ParamID kMix = 9;             // 0 .. 1

/// `worst_case_gain` does not apply to this design — there is no feedback path.
/// Named rather than left absent; see the header note.
inline constexpr bool kWorstCaseGainApplicable = false;

/// The EQ curves an archetype's front panel exposes, as the inclusive
/// `[min, max]` of the stepped `kEqCurve` parameter. An Ampex-class machine is
/// NAB-only (documented American-standard machine), so its range is a single
/// point; the A800 exposes its published NAB/CCIR master switch; a cassette deck
/// exposes the Type I / Type II tape-type switch.
struct CurveRange {
    Curve min_curve = Curve::nab;
    Curve max_curve = Curve::nab;
};

inline CurveRange curve_range(Archetype archetype) noexcept {
    switch (archetype) {
    case Archetype::studer_a800:
        return {Curve::nab, Curve::iec_ccir};
        case Archetype::cassette_deck:
            return {Curve::cassette_type1, Curve::cassette_type2};
        case Archetype::ampex_350_440:
    default:
        return {Curve::nab, Curve::nab};
    }
}

inline const char* tape_type_id(Archetype archetype) noexcept {
    switch (archetype) {
    case Archetype::studer_a800:
        return kStuderTypeId;
    case Archetype::cassette_deck:
        return kCassetteTypeId;
        case Archetype::ampex_350_440:
    default:
        return kAmpexTypeId;
    }
}

inline const char* tape_default_name(Archetype archetype) noexcept {
    switch (archetype) {
    case Archetype::studer_a800:
        return "Tape Machine (Studer A800-class)";
    case Archetype::cassette_deck:
        return "Tape Machine (Cassette Deck)";
        case Archetype::ampex_350_440:
    default:
        return "Tape Machine (Ampex 350/440-class)";
    }
}

inline Archetype normalized_archetype(Archetype archetype) noexcept {
    switch (archetype) {
        case Archetype::ampex_350_440:
        case Archetype::studer_a800:
    case Archetype::cassette_deck:
        return archetype;
    default:
        return Archetype::ampex_350_440;
    }
}

/// The largest gain any single stage of this insert can present, at the
/// realization's own speed and the given sample rate.
///
/// This is what the registry cites in place of `worst_case_gain`, which does not
/// apply. It is deliberately NOT a small number, and the DSP header explains
/// why at length: a reproduce network for a curve with no bass shelf is
/// `(1 + s·t₂)`, a differentiator, so its ceiling near Nyquist is set by the
/// standard's own prototype and grows with the sample rate. The programme never
/// sees it — the record network is that section's exact reciprocal — and the
/// suite asserts the consequence directly by bounding the module's output on
/// silence and at full scale.
float tape_machine_insertion_gain_bound(Archetype archetype, double sample_rate);

struct TapeMachineInstance {
    signal::TapeMachine machine;

    // Last applied values, so a block that changes nothing costs nothing. The
    // sentinels are outside every declared range on purpose: the first block
    // must apply, and "outside the range" is a cheaper way to say that than a
    // parallel set of dirty flags.
    float curve = -1.0f;
    float bias = -99.0f;
    float drive = -99.0f;
    float age = -99.0f;
    float crosstalk_db = 0.0f;
    float companding = -1.0f;
    float print_db = 0.0f;
    float print_offset_ms = -1.0f;
    float mix = -1.0f;

    /// Force the next block to apply every parameter. Called after `prepare`,
    /// which reconfigures the machine behind the cache's back.
    void invalidate_cache() noexcept {
        curve = -1.0f;
        bias = -99.0f;
        drive = -99.0f;
        age = -99.0f;
        crosstalk_db = 0.0f;
        companding = -1.0f;
        print_db = 0.0f;
        print_offset_ms = -1.0f;
        mix = -1.0f;
    }
};

/// One factory, three registered realizations.
///
/// `speed_ips` is snapped to the archetype's legal set by the DSP, so passing
/// 30 to a cassette node yields 1.875 rather than an error — the same behaviour
/// `set_speed_ips` has, and the reason a caller does not need the legal table.
CustomNodeType make_tape_machine_node(Archetype archetype, double speed_ips = 0.0,
                                      bool pre_echo_enabled = false);

ForgeNodeDescriptor descriptor();

}  // namespace pulp::host::tape

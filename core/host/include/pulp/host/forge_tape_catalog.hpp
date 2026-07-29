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

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/detail/forge_realization_identity.hpp>

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
        case Archetype::studer_a800: return {Curve::nab, Curve::iec_ccir};
        case Archetype::cassette_deck:
            return {Curve::cassette_type1, Curve::cassette_type2};
        case Archetype::ampex_350_440:
        default: return {Curve::nab, Curve::nab};
    }
}

inline const char* tape_type_id(Archetype archetype) noexcept {
    switch (archetype) {
        case Archetype::studer_a800: return kStuderTypeId;
        case Archetype::cassette_deck: return kCassetteTypeId;
        case Archetype::ampex_350_440:
        default: return kAmpexTypeId;
    }
}

inline const char* tape_default_name(Archetype archetype) noexcept {
    switch (archetype) {
        case Archetype::studer_a800: return "Tape Machine (Studer A800-class)";
        case Archetype::cassette_deck: return "Tape Machine (Cassette Deck)";
        case Archetype::ampex_350_440:
        default: return "Tape Machine (Ampex 350/440-class)";
    }
}

inline Archetype normalized_archetype(Archetype archetype) noexcept {
    switch (archetype) {
        case Archetype::ampex_350_440:
        case Archetype::studer_a800:
        case Archetype::cassette_deck: return archetype;
        default: return Archetype::ampex_350_440;
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
inline float tape_machine_insertion_gain_bound(Archetype archetype, double sample_rate) {
    signal::TapeMachine probe;
    probe.set_archetype(archetype);
    probe.prepare(sample_rate);
    return static_cast<float>(probe.worst_case_insertion_gain());
}

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
inline CustomNodeType make_tape_machine_node(Archetype archetype, double speed_ips = 0.0,
                                             bool pre_echo_enabled = false) {
    using Machine = signal::TapeMachine;
    const Archetype fixed_archetype = normalized_archetype(archetype);
    const signal::tape::ArchetypePreset preset =
        signal::tape::archetype_preset(fixed_archetype);
    const double requested_speed =
        std::isfinite(speed_ips) && speed_ips > 0.0 ? speed_ips : preset.default_speed_ips;
    const double speed = signal::tape::snap_speed_ips(fixed_archetype, requested_speed);
    const CurveRange curves = curve_range(fixed_archetype);
    const bool curve_is_variable = curves.min_curve != curves.max_curve;

    CustomNodeType t;
    t.type_id = tape_type_id(fixed_archetype);
    if (speed != preset.default_speed_ips || pre_echo_enabled) {
        t.type_id += ".speed_" + detail::realization_real_token(speed);
        if (pre_echo_enabled) t.type_id += ".pre_echo";
    }
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 2;
    t.default_name = tape_default_name(fixed_archetype);
    t.lowerable = true;
    t.latency_samples = [fixed_archetype, speed, pre_echo_enabled](double sample_rate) {
        signal::TapeMachine probe;
        probe.set_archetype(fixed_archetype);
        probe.set_speed_ips(speed);
        probe.prepare(sample_rate);
        probe.set_print_through(static_cast<float>(probe.print_through_db()),
                                static_cast<float>(probe.print_offset_ms()),
                                pre_echo_enabled);
        return probe.latency_samples();
    };

    t.create = []() -> void* { return new TapeMachineInstance{}; };
    t.destroy = [](void* p) { delete static_cast<TapeMachineInstance*>(p); };
    t.prepare = [fixed_archetype, speed, pre_echo_enabled](void* p, double sr,
                                                           int /*max_block*/) {
        auto* instance = static_cast<TapeMachineInstance*>(p);
        instance->machine.set_archetype(fixed_archetype);
        instance->machine.set_speed_ips(speed);
        instance->machine.prepare(sr);
        // Pre-echo is a realization, so it is set once here and never touched
        // again — that is what keeps `latency_samples()` still.
        instance->machine.set_print_through(
            static_cast<float>(instance->machine.print_through_db()),
            static_cast<float>(instance->machine.print_offset_ms()), pre_echo_enabled);
        instance->invalidate_cache();
    };
    t.reset = [](void* p) { static_cast<TapeMachineInstance*>(p)->machine.reset(); };

    // Ranges and defaults are the module's canonical contract, in REAL units;
    // the Forge layer mirrors them and owns the display curve. Every row here is
    // a design-parameter declaration per the series contract.
    if (curve_is_variable) {
        t.baked_params.push_back({kEqCurve, static_cast<float>(curves.min_curve),
                                  static_cast<float>(curves.max_curve),
                                  static_cast<float>(preset.default_curve)});
    }
    t.baked_params.push_back({kBias, static_cast<float>(Machine::kBiasMin),
                              static_cast<float>(Machine::kBiasMax),
                              static_cast<float>(Machine::kBiasDefault)});
    t.baked_params.push_back({kDrive, 0.0f, 1.0f,
                              static_cast<float>(Machine::kDriveDefault)});
    t.baked_params.push_back({kAge, 0.0f, 1.0f, static_cast<float>(preset.age01)});
    t.baked_params.push_back({kCrosstalkDb, static_cast<float>(Machine::kCrosstalkDbMin),
                              static_cast<float>(Machine::kCrosstalkDbMax),
                              static_cast<float>(preset.crosstalk_db)});
    t.baked_params.push_back({kCompanding, 0.0f, 1.0f, preset.companding ? 1.0f : 0.0f});
    t.baked_params.push_back(
        {kPrintThroughDb, static_cast<float>(Machine::kPrintThroughDbMin),
         static_cast<float>(Machine::kPrintThroughDbMax),
         static_cast<float>(signal::tape::age_print_through_db(preset.age01))});
    // With pre-echo disabled the offset is latency-neutral. A pre-echo
    // realization fixes it at construction because it contributes to latency,
    // so exposing a frozen row there would advertise a capability that cannot
    // do anything.
    const float print_offset_default =
        static_cast<float>(Machine::kPrintThroughOffsetMsDefault);
    if (!pre_echo_enabled) {
        t.baked_params.push_back(
            {kPrintOffsetMs, static_cast<float>(Machine::kPrintThroughOffsetMsMin),
             static_cast<float>(Machine::kPrintThroughOffsetMsMax),
             print_offset_default});
    }
    t.baked_params.push_back({kMix, 0.0f, 1.0f, 1.0f});

    t.process_instance_baked_param = [pre_echo_enabled, curve_is_variable](
                                         void* p, audio::BufferView<float>& out,
                                         const audio::BufferView<const float>& in, int n,
                                         const BakedParamView& params) {
        auto* instance = static_cast<TapeMachineInstance*>(p);
        auto& machine = instance->machine;

        // Read at offset 0 and apply once. See the header note on why this node
        // does not write its setters per sample.
        auto changed = [](float& held, float fresh) {
            if (held == fresh) return false;
            held = fresh;
            return true;
        };

        if (curve_is_variable &&
            changed(instance->curve, params.value_at(kEqCurve, 0)))
            machine.set_eq_curve(static_cast<Curve>(
                static_cast<std::uint8_t>(std::lround(instance->curve))));
        if (changed(instance->drive, params.value_at(kDrive, 0)))
            machine.set_drive(instance->drive);

        // Age first, because `set_age` WRITES the print-through level (that is
        // what "storage print grows with age" means) and shifts the effective
        // bias. Applying it after either would let the age axis silently
        // overwrite an explicit parameter — so age goes first, and a block in
        // which it moved must re-apply the print level even if the print
        // parameter itself did not change.
        const bool age_moved = changed(instance->age, params.value_at(kAge, 0));
        if (age_moved) machine.set_age(instance->age);

        if (changed(instance->bias, params.value_at(kBias, 0)) || age_moved)
            machine.set_bias(instance->bias);
        if (changed(instance->crosstalk_db, params.value_at(kCrosstalkDb, 0)))
            machine.set_crosstalk_db(instance->crosstalk_db);
        if (changed(instance->companding, params.value_at(kCompanding, 0)))
            machine.set_companding_enabled(instance->companding >= 0.5f);

        const bool print_moved =
            changed(instance->print_db, params.value_at(kPrintThroughDb, 0));
        const float print_offset_ms =
            pre_echo_enabled
                ? static_cast<float>(Machine::kPrintThroughOffsetMsDefault)
                : params.value_at(kPrintOffsetMs, 0);
        const bool offset_moved =
            changed(instance->print_offset_ms, print_offset_ms);
        if (print_moved || offset_moved || age_moved)
            machine.set_print_through(instance->print_db, instance->print_offset_ms,
                                      pre_echo_enabled);

        if (changed(instance->mix, params.value_at(kMix, 0))) machine.set_mix(instance->mix);

        // Safe to write straight into the output view even when the executor
        // aliases it onto the input: `process()` reads both input samples for a
        // frame before writing either output sample of that frame, and never
        // reads an input frame it has already passed.
        machine.process(in.channel_ptr(0), in.channel_ptr(1), out.channel_ptr(0),
                        out.channel_ptr(1), n);
    };
    return t;
}

inline ForgeNodeDescriptor descriptor() {
    return {
        "tape_machine", "Tape Machine",
        "Models open-reel and cassette recording paths with bias, saturation, wear, crosstalk, companding, and print-through.",
        {{"archetype", "Archetype", "Selects the fixed machine lineage.",
          {{"ampex", "Ampex-class Open Reel", 0},
           {"studer", "Studer-class Open Reel", 1},
           {"cassette", "Cassette Deck", 2}}}},
        {{"ampex", kAmpexTypeId, {{"archetype", "ampex"}}},
         {"studer", kStuderTypeId, {{"archetype", "studer"}}},
         {"cassette", kCassetteTypeId, {{"archetype", "cassette"}}}},
        {
            {"eq_curve", kEqCurve, "EQ Curve", "", "Selects the recording equalization standard.",
             ForgeParamKind::stepped, ForgeParamCurve::linear,
             {{"nab", "NAB", static_cast<float>(Curve::nab), {"studer"}},
              {"iec_ccir", "IEC/CCIR", static_cast<float>(Curve::iec_ccir), {"studer"}},
              {"cassette_type_one", "Cassette Type One",
               static_cast<float>(Curve::cassette_type1), {"cassette"}},
              {"cassette_type_two", "Cassette Type Two",
               static_cast<float>(Curve::cassette_type2), {"cassette"}}},
             {"studer", "cassette"}},
            {"bias", kBias, "Bias", "", "Moves tape bias below or above the calibrated point.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"drive", kDrive, "Drive", "%", "Sets record-level saturation.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"age", kAge, "Age", "%", "Adds wear, loss, and instability.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"crosstalk_db", kCrosstalkDb, "Crosstalk", "dB", "Sets channel bleed.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"companding", kCompanding, "Companding", "", "Enables the machine's encode/decode compression path.",
             ForgeParamKind::stepped, ForgeParamCurve::linear,
             {{"off", "Off", 0}, {"on", "On", 1}}},
            {"print_through_db", kPrintThroughDb, "Print Through", "dB", "Sets magnetic echo level.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
            {"print_offset_ms", kPrintOffsetMs, "Print Offset", "ms", "Sets magnetic echo delay.",
             ForgeParamKind::continuous, ForgeParamCurve::logarithmic},
            {"mix", kMix, "Mix", "%", "Blends dry and tape paths.",
             ForgeParamKind::continuous, ForgeParamCurve::linear},
        }};
}

}  // namespace pulp::host::tape

#pragma once

// Synthesis — bake-layer catalog nodes.
//
// The home for the synthesis family: nodes that MAKE a signal, or that make one
// signal wear another's spectrum. It opens with the sinusoidal ADDITIVE BANK and
// the channel VOCODER, and it is named for the family rather than for either of
// them because a third member is being built alongside it:
//
//   * The GRANULAR ENGINE lands here in its own `granular` namespace, beside
//     `additive` and `vocoder` below. It shares this file's conventions — a
//     source-shaped node, real units in the baked table, a worst-case gain that
//     cites its own DSP suite's asserted invariant — and differs in where its
//     material comes from. Do not add it to a fourth header named after its
//     algorithm; that scatters one set of decisions across several places.
//
// ── PORT SHAPE, which is per member and is the thing to get right ─────────
//
// Nothing about a node is easier to get wrong by pattern-matching than its
// ports, and nothing is less visible in a unit test: a node with its two inputs
// transposed passes every measurement that feeds both ports the same signal.
//
// The ADDITIVE BANK is a SOURCE with a GATE. One input port and one output. The
// input port carries a **gate CV, not audio** — a control signal in the sense
// the lofi catalog already established when its LFO emits a unipolar control on
// an audio port and its VCA reads one on port 1. A synthesiser with no way to
// be struck is not a quiet synthesiser, it is a silent one: the DSP's shared
// onset stays closed until `retrigger()`, so a gate is not a convenience here,
// it is the difference between a node that makes sound and one that cannot.
// Making it a PORT rather than a param is what lets a sequencer, an envelope, or
// an LFO play the instrument; a host with no CV source drives it from a constant
// DC input node, which is what this file's suite does.
//
// The VOCODER takes TWO INPUTS, and this is the family's real design decision:
//
//   * **Two ordinary input ports — port 0 = MODULATOR, port 1 = CARRIER — with
//     one mono output.** Precedent is direct: the lofi catalog's dry/wet mixer
//     is two audio inputs (0 = dry, 1 = wet) into one output, and its VCA is
//     signal + CV into one output.
//
//   * A SIDECHAIN edge was considered and is **not available**. `connect_sidechain`
//     is documented as routing "into the destination PLUGIN's sidechain bus
//     port" and carries a guard that the destination is a Plugin node —
//     "sidechain only makes sense on plugins". A custom catalog node cannot be
//     a sidechain destination, so the choice is not between two conventions;
//     ordinary ports are the only one that exists at this layer. That is a
//     fact about the graph, not a preference, and it is written down here so
//     the next person does not re-litigate it.
//
//   * ORDER: modulator first. Two things agree on it and nothing argues the
//     other way — `VocoderT::process(modulator, carrier_ext, out)` takes them
//     in that order, so a transposed node would disagree with its own DSP at
//     the call site; and every vocoder ever built labels its inputs
//     "Modulator / Carrier" in that order, so it is also what a graph author
//     reaches for. The VCA's "signal on 0, control on 1" reading does NOT
//     apply: the VCA's port 1 is a unipolar CV, while both of the vocoder's
//     ports carry full-range audio, which makes the dry/wet mixer the closer
//     precedent.
//
//   * Because a transposition is invisible to a symmetric test, this file's
//     suite asserts ASYMMETRY directly: swapping the two inputs must change the
//     output. A suite that fed both ports the same signal would pass either
//     way, which is exactly how a node ships unusable with green tests.
//
// ── REALIZATION vs INJECTABLE PARAM, applied per member ───────────────────
//
// The standing rule from the dynamics family: anything that moves
// `latency_samples()` is frozen at REGISTRATION, because a node whose reported
// latency changes under the audio thread breaks the host's delay compensation.
// A genuine TOPOLOGY change is a realization too. Coefficients are params.
//
// Both members here report `latency_samples() == 0` in every configuration —
// the bank is direct time-domain oscillators and the vocoder is all
// minimum-phase IIR — so the latency clause never fires in this file. What
// remains is topology and capacity, and they land differently:
//
//   * ADDITIVE, `voice` → REALIZATION, two registered type ids. Not because it
//     is "a big change", but because `VoiceTable::harmonic` is part of it: a
//     modal voice IGNORES `inharmonicity`, so registering a bell makes an
//     injectable param inert. A param whose existence depends on a choice means
//     that choice is part of what the node IS. (The organ and bell also differ
//     in every partial's ratio, amplitude and decay, so automating between them
//     would step the entire spectrum.) This follows the diode-bridge member's
//     two-type-id split for the same reason.
//
//   * ADDITIVE, `max_partials` → REALIZATION, a construction argument. It is
//     the ONLY allocating dimension in the DSP (`prepare()` is documented as
//     the only method that may allocate, and only when this grows), and it also
//     fixes the declared maximum of the `partial_count` param — a param whose
//     range is decided at registration cannot itself be a param. Same shape as
//     the feedforward compressor's lookahead ceiling.
//
//   * ADDITIVE, `envelope_mode` → INJECTABLE PARAM, and this one was checked
//     against the code rather than assumed. It looks like it should click: a
//     per-partial decay that has fallen to 0.3 ought to jump when the shared
//     envelope takes over. It does not. `shared_ar` sets the per-partial decay
//     COEFFICIENT to 1, which freezes each partial's decay where it stands
//     instead of resetting it, so the switch is continuous in both directions.
//     With no discontinuity, no latency change and no topology change, it is
//     the detector-mode case from the dynamics family: one switch selecting
//     which coefficient feeds the same envelope.
//
//   * ADDITIVE, `retrig_phase` → INJECTABLE PARAM. It is read only inside
//     `retrigger()`, so it cannot take effect mid-note and cannot step anything.
//
//   * VOCODER → NO REALIZATION AXIS AT ALL, like the FET compressor. Worth
//     stating explicitly because `band_count` looks like a structural argument
//     and is not: the DSP holds every per-band array as a fixed
//     `std::array<…, kMaxBands>` and `set_band_count` moves only the active loop
//     bound, allocating nothing and leaving latency at 0. It is a param, and one
//     of the load-bearing ones.
//
// ── PARAM APPLICATION RATE ────────────────────────────────────────────────
//
// The dynamics family applies every param per sample, and says why: its setters
// are "a clamp, a store, and at most one `exp`". That is not true of every
// setter here, and pretending otherwise would put a filter-bank redesign inside
// the sample loop. So this file splits them, and names which is which:
//
//   * PER SAMPLE — setters that clamp and store. Sample-accurate automation.
//   * BLOCK RATE, AND ONLY ON CHANGE — setters that rebuild a filter bank or
//     re-derive per-band ballistics: the vocoder's `band_count`,
//     `freq_lo/hi_hz`, `attack_ms` and `release_ms`. Each of these loops every
//     band computing transcendentals; per-sample they would cost more than the
//     audio. The dry/wet mixer node already established block-rate application
//     where the wrapped API is block-oriented — this is the same call with a
//     different reason.
//
// The change guard is not an optimisation detail that can be dropped: without
// it, holding a knob still would still redesign twenty bands per sample.

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/additive_bank.hpp>
#include <pulp/signal/cyclic_stretch.hpp>
#include <pulp/signal/granular.hpp>
#include <pulp/signal/vocoder.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::host::synthesis {

// ── The sinusoidal additive bank ──────────────────────────────────────────
namespace additive {

/// The two registered voices. See the header note: the split exists because a
/// modal voice has no inharmonicity control, not merely because the tables
/// differ.
enum class Voice : std::uint8_t {
    organ,  ///< Harmonic, sustained. `inharmonicity` applies.
    bell,   ///< Modal, per-partial decays. No inharmonicity parameter.
};

inline constexpr const char* kOrganTypeId = "synthesis.additive_organ";
inline constexpr const char* kBellTypeId = "synthesis.additive_bell";

// Node-local param ids; the framework namespaces per node, so these may restart
// at 1 without colliding with any other node's.
inline constexpr state::ParamID kFundamentalHz = 1;    // Hz
inline constexpr state::ParamID kPartialCount = 2;     // stepped, 1 .. max_partials
inline constexpr state::ParamID kInharmonicity = 3;    // B, dimensionless
inline constexpr state::ParamID kSpectralTilt = 4;     // dB/oct
inline constexpr state::ParamID kMasterGainDb = 5;     // dB
inline constexpr state::ParamID kEnvelopeMode = 6;     // stepped 0 = shared AR, 1 = per-partial
inline constexpr state::ParamID kRetrigPhase = 7;      // stepped 0/1/2
inline constexpr state::ParamID kAttackMs = 8;         // ms
inline constexpr state::ParamID kReleaseMs = 9;        // ms
inline constexpr state::ParamID kDetuneCents = 10;     // cents

// NOT DECLARED, deliberately: `morph` and `spectral_domain`.
//
// Both act only through the DSP's `SpectralEnvelope` PAIR — `morph` crossfades
// envelope A against B, and `spectral_domain` chooses whether that envelope is
// read against absolute Hz or against a ratio to f0. This node supplies no
// envelopes, and it cannot: an envelope is a breakpoint LIST, and a baked-param
// table carries scalars. With both envelopes left at their flat defaults, a
// crossfade between two identical flat curves and a re-mapping of a flat
// curve's abscissa are both provably no-ops.
//
// They were declared in the first draft of this file and the suite caught them:
// injecting either moved the audio by exactly 0.0. A param that cannot move the
// audio must not be in the table, because a host would draw a control that does
// nothing. They become meaningful the day the family can bake a table — at
// which point the envelope pair is registration-time material (it is voice
// data, like the partial table it sits beside) and these two become params
// against it.

/// Partial ceiling this node instantiates. The DSP supports up to
/// `kMaxPartialsCeiling`; this is the value baked in, which sizes the bank at
/// `prepare()` and fixes the `partial_count` param's declared maximum.
/// [design parameter] default 128, range 1 .. 256.
inline constexpr int kNodeMaxPartials = signal::AdditiveBank::kMaxPartialsDefault;

/// Gate thresholds on the CV input, with hysteresis so a noisy or slewed gate
/// cannot retrigger on every sample near the threshold. Schmitt, not a single
/// edge, for the same reason the vocoder's voicing decision is.
/// [design parameter] defaults 0.5 / 0.25, ranges 0.2 .. 0.9 and 0.05 .. 0.5,
/// with `kGateOff < kGateOn` by construction.
inline constexpr float kGateOn = 0.5f;
inline constexpr float kGateOff = 0.25f;

struct Instance {
    signal::AdditiveBank bank;
    /// Latched gate state, so `retrigger()` fires on the EDGE rather than for
    /// every sample the gate is high — retriggering per sample would restart
    /// every decay continuously and the bank would never ring.
    bool gate_high = false;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// Cited, not estimated: the DSP ships `worst_case_gain()` as an EQUALITY
/// attained with aligned phases — its crest normaliser bounds the partial sum
/// to 1 by construction, so the whole worst case is the master trim's legal
/// maximum — and its own suite asserts both halves of that bound. Reported at
/// the node's parameter ceiling, since `master_gain_db` is injectable and a
/// baked param can be automated anywhere in its declared range.
inline float additive_bank_worst_case_gain() {
    return static_cast<float>(signal::AdditiveBank::worst_case_gain());
}

/// `voice` picks the registered type id and the partial table; `max_partials`
/// is the allocation ceiling. Both are frozen here — see the header note.
inline CustomNodeType make_additive_bank_node(Voice voice = Voice::organ,
                                              int max_partials = kNodeMaxPartials) {
    const int partials =
        std::clamp(max_partials, 1, signal::AdditiveBank::kMaxPartialsCeiling);

    CustomNodeType t;
    t.type_id = voice == Voice::organ ? kOrganTypeId : kBellTypeId;
    t.version = 1;
    t.num_input_ports = 1;   // port 0 = GATE CV, not audio
    t.num_output_ports = 1;
    t.default_name = voice == Voice::organ ? "Additive Organ" : "Additive Bell";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [voice, partials](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->bank.prepare(sr, partials);
        // After `prepare()`, which is what sized the bank and would otherwise
        // leave the default organ table in place.
        s->bank.load_voice(voice == Voice::organ ? signal::make_organ_voice(partials)
                                                 : signal::make_bell_voice(partials));
        s->gate_high = false;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<Instance*>(p);
        s->bank.reset();
        s->gate_high = false;
    };

    using Bank = signal::AdditiveBank;
    t.baked_params.push_back({kFundamentalHz, static_cast<float>(Bank::kFundamentalMinHz),
                              static_cast<float>(Bank::kFundamentalMaxHz),
                              static_cast<float>(Bank::kFundamentalDefaultHz)});
    t.baked_params.push_back({kPartialCount, 1.0f, static_cast<float>(partials),
                              static_cast<float>(std::min(Bank::kPartialCountDefault, partials))});
    if (voice == Voice::organ) {
        t.baked_params.push_back({kInharmonicity, 0.0f,
                                  static_cast<float>(Bank::kInharmonicityMax),
                                  static_cast<float>(Bank::kInharmonicityDefault)});
    }
    t.baked_params.push_back({kSpectralTilt, static_cast<float>(Bank::kSpectralTiltMinDbOct),
                              static_cast<float>(Bank::kSpectralTiltMaxDbOct),
                              static_cast<float>(Bank::kSpectralTiltDefaultDbOct)});
    t.baked_params.push_back({kMasterGainDb, static_cast<float>(Bank::kMasterGainMinDb),
                              static_cast<float>(Bank::kMasterGainMaxDb),
                              static_cast<float>(Bank::kMasterGainDefaultDb)});
    t.baked_params.push_back({kEnvelopeMode, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kRetrigPhase, 0.0f, 2.0f, 0.0f});
    t.baked_params.push_back({kAttackMs, static_cast<float>(Bank::kAttackMinMs),
                              static_cast<float>(Bank::kAttackMaxMs),
                              static_cast<float>(Bank::kAttackDefaultMs)});
    t.baked_params.push_back({kReleaseMs, static_cast<float>(Bank::kReleaseMinMs),
                              static_cast<float>(Bank::kReleaseMaxMs),
                              static_cast<float>(Bank::kReleaseDefaultMs)});
    t.baked_params.push_back({kDetuneCents, 0.0f, static_cast<float>(Bank::kDetuneMaxCents),
                              static_cast<float>(Bank::kDetuneDefaultCents)});

    t.process_instance_baked_param = [voice](void* p, audio::BufferView<float>& out,
                                              const audio::BufferView<const float>& in,
                                              int n, const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* gate = in.channel_ptr(0);
        float* output = out.channel_ptr(0);

        // Every setter here clamps and stores; the bank's real work happens on
        // its own control cadence, so per-sample application is sample-accurate
        // automation rather than a per-sample rebuild.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->bank.set_fundamental_hz(params.value_at(kFundamentalHz, offset));
            s->bank.set_partial_count(
                static_cast<int>(std::lround(params.value_at(kPartialCount, offset))));
            if (voice == Voice::organ)
                s->bank.set_inharmonicity_b(params.value_at(kInharmonicity, offset));
            s->bank.set_spectral_tilt_db_oct(params.value_at(kSpectralTilt, offset));
            s->bank.set_master_gain_db(params.value_at(kMasterGainDb, offset));
            s->bank.set_envelope_mode(
                params.value_at(kEnvelopeMode, offset) >= 0.5f
                    ? signal::AdditiveBank::EnvelopeMode::per_partial_decay
                    : signal::AdditiveBank::EnvelopeMode::shared_ar);
            const int phase_step = std::clamp(
                static_cast<int>(std::lround(params.value_at(kRetrigPhase, offset))), 0, 2);
            s->bank.set_retrig_phase(
                static_cast<signal::AdditiveBank::RetrigPhase>(phase_step));
            s->bank.set_attack_ms(params.value_at(kAttackMs, offset));
            s->bank.set_release_ms(params.value_at(kReleaseMs, offset));
            s->bank.set_detune_cents(params.value_at(kDetuneCents, offset));

            // Gate edges, Schmitt-latched. `retrigger()` restarts every decay,
            // so firing it while the gate merely STAYS high would hold the
            // attack open forever and the bell would never ring out.
            const float cv = gate[static_cast<std::size_t>(k)];
            if (!s->gate_high && cv >= kGateOn) {
                s->gate_high = true;
                s->bank.retrigger();
            } else if (s->gate_high && cv <= kGateOff) {
                s->gate_high = false;
                s->bank.release();
            }

            output[static_cast<std::size_t>(k)] = s->bank.next();
        }
    };
    return t;
}

}  // namespace additive

// ── The channel vocoder ───────────────────────────────────────────────────
namespace vocoder {

inline constexpr const char* kTypeId = "synthesis.vocoder";

/// Input port roles. Named constants rather than bare 0 and 1, because the one
/// mistake this node cannot survive is a transposition and a named port is
/// harder to swap by accident than an integer literal.
inline constexpr PortIndex kModulatorPort = 0;
inline constexpr PortIndex kCarrierPort = 1;

inline constexpr state::ParamID kBandCount = 1;        // stepped, 10 .. 20
inline constexpr state::ParamID kFreqLoHz = 2;         // Hz
inline constexpr state::ParamID kFreqHiHz = 3;         // Hz
inline constexpr state::ParamID kCarrierSource = 4;    // stepped 0 = external, 1 = internal
inline constexpr state::ParamID kInternalWave = 5;     // stepped 0 = saw, 1 = pulse
inline constexpr state::ParamID kInternalPw = 6;       // 0.05 .. 0.95
inline constexpr state::ParamID kCarrierPitchHz = 7;   // Hz — internal carrier
inline constexpr state::ParamID kNoiseMix = 8;         // 0..1
inline constexpr state::ParamID kAttackMs = 9;         // ms
inline constexpr state::ParamID kReleaseMs = 10;       // ms
inline constexpr state::ParamID kUnvoicedSens = 11;    // 0..1
inline constexpr state::ParamID kSibilanceMix = 12;    // 0..1
inline constexpr state::ParamID kFormantShiftSt = 13;  // semitones
inline constexpr state::ParamID kFormantFreeze = 14;   // stepped 0/1
inline constexpr state::ParamID kOutputTrimDb = 15;    // dB
inline constexpr state::ParamID kDryWet = 16;          // 0..1

using Voc = signal::Vocoder;

struct Instance {
    signal::Vocoder vocoder;
    /// Last-applied values of the four expensive setters. A filter-bank rebuild
    /// runs only when one of these actually moves; see the header note on
    /// application rate.
    float band_count = -1.0f;
    float freq_lo = -1.0f;
    float freq_hi = -1.0f;
    float attack_ms = -1.0f;
    float release_ms = -1.0f;
};

/// Worst-case linear gain for the Forge registry (series law 8).
///
/// The DSP's own asserted invariant is the RECONSTRUCTION bound: its suite
/// measures the summed synthesis bank against `kWorstCaseGain` at every band
/// count from 10 to 20, with the per-band `VcaT` clamp making that a bound
/// rather than an estimate. But the node exposes two things the DSP's own
/// T-GAIN case deliberately parks: the sibilance path (which that test sets to
/// zero so the bank's bound is measured alone) and the output trim. A baked
/// param can be automated anywhere in its declared range, so the node's number
/// has to include both — the FET compressor reports at its parameter ceilings
/// for the same reason.
///
/// Assembled from shipped constants:
///
///   * the trimmed bank, `kOutputHeadroomTrim · kWorstCaseGain` — 1.0, which is
///     the ≤ 1 the DSP suite asserts post-trim;
///   * plus the sibilance path at its ceiling, `sibilance_mix · u · |hp|` with
///     both controls at 1 and `hp` a first-order high-pass whose L1 norm is
///     `2/(1 + tan(π f_c/f_s))` — just under 2, not the unity its PASSBAND gain
///     suggests, which is the same distinction the chorus family's cross-feed
///     term turns on;
///   * all of it times the output trim's ceiling.
///
/// The dry/wet blend cannot raise it: it is a convex combination of that wet
/// sum and a dry signal bounded by 1.
///
/// **This is a GAIN, i.e. a bound for `|input| ≤ 1`, and the premise matters
/// more here than it does for most nodes.** The two paths respond to input
/// level completely differently: the bank's is amplitude-INSENSITIVE, because
/// the per-band `VcaT` clamps its control and a louder modulator only saturates
/// it sooner — while the sibilance path is amplitude-PROPORTIONAL, passing the
/// modulator's own high end straight to the output. A modulator driven 20 dB
/// hot therefore does not make the vocoding 20 dB louder; it makes the
/// SIBILANCE 20 dB louder, and the balance the `sibilance_mix` control sets
/// moves under the level. Worth knowing at the mixer as well as at the bound.
///
/// Sample-rate dependent through the high-pass term, which is why this takes a
/// rate rather than pretending to be a constant.
inline float vocoder_worst_case_gain(double sample_rate = 48000.0) {
    constexpr double kPi = 3.14159265358979323846;
    const double highpass_l1 =
        2.0 / (1.0 + std::tan(kPi * Voc::kSibilanceCornerHz / sample_rate));
    const double bank = Voc::kOutputHeadroomTrim * Voc::kWorstCaseGain;
    const double trim = std::pow(10.0, Voc::kOutputTrimMaxDb / 20.0);
    return static_cast<float>((bank + highpass_l1) * trim);
}

/// Two inputs, one output. See this file's header for why the ports are plain
/// audio ports rather than a sidechain, and why the modulator is port 0.
inline CustomNodeType make_vocoder_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 2;  // 0 = modulator (the voice), 1 = carrier (the thing that speaks)
    t.num_output_ports = 1;
    t.default_name = "Vocoder";
    t.lowerable = true;

    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<Instance*>(p);
        s->vocoder.prepare(sr);
        // Force the change-guarded setters to apply on the first block: a
        // guard that starts equal to the incoming value would skip the
        // configuration the graph asked for.
        s->band_count = -1.0f;
        s->freq_lo = -1.0f;
        s->freq_hi = -1.0f;
        s->attack_ms = -1.0f;
        s->release_ms = -1.0f;
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->vocoder.reset(); };

    t.baked_params.push_back({kBandCount, static_cast<float>(Voc::kMinBands),
                              static_cast<float>(Voc::kMaxBands), 16.0f});
    t.baked_params.push_back({kFreqLoHz, static_cast<float>(Voc::kFreqLoMinHz),
                              static_cast<float>(Voc::kFreqLoMaxHz), 120.0f});
    t.baked_params.push_back({kFreqHiHz, static_cast<float>(Voc::kFreqHiMinHz),
                              static_cast<float>(Voc::kFreqHiMaxHz), 7000.0f});
    t.baked_params.push_back({kCarrierSource, 0.0f, 1.0f, 1.0f});
    t.baked_params.push_back({kInternalWave, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kInternalPw, static_cast<float>(Voc::kPulseWidthMin),
                              static_cast<float>(Voc::kPulseWidthMax), 0.5f});
    t.baked_params.push_back({kCarrierPitchHz, static_cast<float>(Voc::kMinPitchHz),
                              static_cast<float>(Voc::kMaxPitchHz),
                              static_cast<float>(Voc::kDefaultPitchHz)});
    t.baked_params.push_back({kNoiseMix, 0.0f, 1.0f, 0.15f});
    t.baked_params.push_back({kAttackMs, static_cast<float>(Voc::kAttackMinMs),
                              static_cast<float>(Voc::kAttackMaxMs), 1.5f});
    t.baked_params.push_back({kReleaseMs, static_cast<float>(Voc::kReleaseMinMs),
                              static_cast<float>(Voc::kReleaseMaxMs), 15.0f});
    t.baked_params.push_back({kUnvoicedSens, 0.0f, 1.0f, 0.5f});
    t.baked_params.push_back({kSibilanceMix, 0.0f, 1.0f, 0.35f});
    t.baked_params.push_back({kFormantShiftSt, static_cast<float>(Voc::kFormantShiftMinSt),
                              static_cast<float>(Voc::kFormantShiftMaxSt), 0.0f});
    t.baked_params.push_back({kFormantFreeze, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kOutputTrimDb, static_cast<float>(Voc::kOutputTrimMinDb),
                              static_cast<float>(Voc::kOutputTrimMaxDb), 0.0f});
    t.baked_params.push_back({kDryWet, 0.0f, 1.0f, 1.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<Instance*>(p);
        const float* modulator = in.channel_ptr(kModulatorPort);
        const float* carrier = in.channel_ptr(kCarrierPort);
        float* output = out.channel_ptr(0);

        // ── Block rate, and only on change ─────────────────────────────────
        // Each of these loops every band computing transcendentals. Sampled at
        // the block's first sample, following the dry/wet mixer's precedent for
        // a setter that is not sample-cheap.
        const float bands = params.value_at(kBandCount, 0);
        if (bands != s->band_count) {
            s->band_count = bands;
            s->vocoder.set_band_count(static_cast<int>(std::lround(bands)));
        }
        const float lo = params.value_at(kFreqLoHz, 0);
        const float hi = params.value_at(kFreqHiHz, 0);
        if (lo != s->freq_lo || hi != s->freq_hi) {
            s->freq_lo = lo;
            s->freq_hi = hi;
            s->vocoder.set_band_range_hz(lo, hi);
        }
        const float attack = params.value_at(kAttackMs, 0);
        if (attack != s->attack_ms) {
            s->attack_ms = attack;
            s->vocoder.set_attack_ms(attack);
        }
        const float release = params.value_at(kReleaseMs, 0);
        if (release != s->release_ms) {
            s->release_ms = release;
            s->vocoder.set_release_ms(release);
        }

        // ── Per sample ─────────────────────────────────────────────────────
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->vocoder.set_carrier_source(params.value_at(kCarrierSource, offset) >= 0.5f
                                              ? Voc::CarrierSource::internal
                                              : Voc::CarrierSource::external);
            s->vocoder.set_internal_wave(params.value_at(kInternalWave, offset) >= 0.5f
                                             ? Voc::InternalWave::pulse
                                             : Voc::InternalWave::saw);
            s->vocoder.set_internal_pulse_width(params.value_at(kInternalPw, offset));
            s->vocoder.set_internal_pitch_hz(params.value_at(kCarrierPitchHz, offset));
            s->vocoder.set_noise_mix(params.value_at(kNoiseMix, offset));
            s->vocoder.set_unvoiced_sensitivity(params.value_at(kUnvoicedSens, offset));
            s->vocoder.set_sibilance_mix(params.value_at(kSibilanceMix, offset));
            s->vocoder.set_formant_shift_semitones(params.value_at(kFormantShiftSt, offset));
            s->vocoder.set_formant_freeze(params.value_at(kFormantFreeze, offset) >= 0.5f);
            s->vocoder.set_output_trim_db(params.value_at(kOutputTrimDb, offset));
            s->vocoder.set_dry_wet(params.value_at(kDryWet, offset));

            float y = 0.0f;
            s->vocoder.process(modulator[static_cast<std::size_t>(k)],
                               carrier[static_cast<std::size_t>(k)], y);
            output[static_cast<std::size_t>(k)] = y;
        }
    };
    return t;
}

}  // namespace vocoder

namespace cyclic {
enum class Regime : std::uint8_t { short_frame, long_frame };
inline constexpr const char* kShortTypeId = "synthesis.cyclic_stretch_short";
inline constexpr const char* kLongTypeId = "synthesis.cyclic_stretch_long";
inline constexpr state::ParamID kStretch = 1;
inline constexpr state::ParamID kCaptureMs = 2;
inline constexpr state::ParamID kCrossfadeShape = 3;
inline constexpr state::ParamID kMixPct = 4;
inline constexpr state::ParamID kOutputDb = 5;
using Engine = signal::CyclicStretch;
struct Instance {
    Engine engine;
};
inline float cyclic_stretch_worst_case_gain() {
    return static_cast<float>(Engine::kWorstCaseGain *
                              signal::units::db_to_linear(Engine::kOutputDbMax));
}
inline CustomNodeType make_cyclic_stretch_node(Regime regime = Regime::short_frame) {
    const auto settings = regime == Regime::short_frame ? signal::kCyclicStretchShortFrame
                                                        : signal::kCyclicStretchLongFrame;
    CustomNodeType t;
    t.type_id = regime == Regime::short_frame ? kShortTypeId : kLongTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name =
        regime == Regime::short_frame ? "Cyclic Stretch (Short)" : "Cyclic Stretch (Long)";
    t.lowerable = true;
    t.latency_samples = [settings](double sample_rate) {
        Engine probe;
        probe.prepare(sample_rate);
        probe.set_regime(settings);
        return probe.latency_samples();
    };
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [settings](void* p, double sr, int) {
        auto& e = static_cast<Instance*>(p)->engine;
        e.prepare(sr);
        e.set_regime(settings);
        e.reset();
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };
    t.baked_params.push_back({kStretch, static_cast<float>(Engine::kStretchRatioMin),
                              static_cast<float>(Engine::kStretchRatioMax),
                              static_cast<float>(Engine::kStretchRatioDefault)});
    t.baked_params.push_back({kCaptureMs, static_cast<float>(Engine::kCaptureMsMin),
                              static_cast<float>(Engine::kCaptureMsMax),
                              static_cast<float>(Engine::kCaptureMsDefault)});
    t.baked_params.push_back(
        {kCrossfadeShape, 0.0f, 1.0f, static_cast<float>(settings.crossfade_shape)});
    t.baked_params.push_back({kMixPct, 0.0f, 100.0f, static_cast<float>(Engine::kMixDefault)});
    t.baked_params.push_back({kOutputDb, static_cast<float>(Engine::kOutputDbMin),
                              static_cast<float>(Engine::kOutputDbMax),
                              static_cast<float>(Engine::kOutputDbDefault)});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& e = static_cast<Instance*>(p)->engine;
        e.set_stretch_ratio(params.value_at(kStretch, 0));
        e.set_capture_ms(params.value_at(kCaptureMs, 0));
        e.set_crossfade_shape(params.value_at(kCrossfadeShape, 0));
        e.set_mix(params.value_at(kMixPct, 0));
        e.set_output_db(params.value_at(kOutputDb, 0));
        e.process(in.channel_ptr(0), out.channel_ptr(0), n);
    };
    return t;
}
} // namespace cyclic

namespace granular {
inline constexpr const char* kTypeId = "synthesis.granular_live";
inline constexpr state::ParamID kDensityHz = 1;
inline constexpr state::ParamID kGrainMs = 2;
inline constexpr state::ParamID kPosition = 3;
inline constexpr state::ParamID kPositionSprayMs = 4;
inline constexpr state::ParamID kPitchSt = 5;
inline constexpr state::ParamID kPitchSpraySt = 6;
inline constexpr state::ParamID kPanSpray = 7;
inline constexpr state::ParamID kJitter = 8;
inline constexpr state::ParamID kLevelDb = 9;
inline constexpr state::ParamID kMix = 10;
using Engine = signal::GranularEngine;
struct Instance {
    Engine engine;
};
inline float granular_worst_case_gain() {
    // At most kMaxGrainBudget grains overlap. Each grain, its window, and its
    // pan gain are individually bounded by one; the shipped four-point cubic
    // interpolator has a peak kernel L1 norm of 1.25.
    return static_cast<float>(Engine::kMaxGrainBudget) * 1.25f *
           static_cast<float>(signal::units::db_to_linear(Engine::kMaxLevelDb));
}
inline CustomNodeType make_granular_node() {
    CustomNodeType t;
    t.type_id = kTypeId;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 2;
    t.default_name = "Granular Live";
    t.lowerable = true;
    t.create = []() -> void* { return new Instance{}; };
    t.destroy = [](void* p) { delete static_cast<Instance*>(p); };
    t.prepare = [](void* p, double sr, int) {
        auto& e = static_cast<Instance*>(p)->engine;
        e.prepare(sr);
        e.set_source(signal::GrainSource::live_ring);
        e.reset();
    };
    t.reset = [](void* p) { static_cast<Instance*>(p)->engine.reset(); };
    t.baked_params.push_back({kDensityHz, static_cast<float>(Engine::kMinDensityHz),
                              static_cast<float>(Engine::kMaxDensityHz), 20.0f});
    t.baked_params.push_back({kGrainMs, static_cast<float>(Engine::kMinGrainMs),
                              static_cast<float>(Engine::kMaxGrainMs), 80.0f});
    t.baked_params.push_back({kPosition, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back(
        {kPositionSprayMs, 0.0f, static_cast<float>(Engine::kMaxPositionSprayMs), 0.0f});
    t.baked_params.push_back({kPitchSt, static_cast<float>(-Engine::kMaxPitchSemitones),
                              static_cast<float>(Engine::kMaxPitchSemitones), 0.0f});
    t.baked_params.push_back(
        {kPitchSpraySt, 0.0f, static_cast<float>(Engine::kMaxPitchSpraySemitones), 0.0f});
    t.baked_params.push_back({kPanSpray, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kJitter, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kLevelDb, static_cast<float>(Engine::kMinLevelDb),
                              static_cast<float>(Engine::kMaxLevelDb), 0.0f});
    t.baked_params.push_back({kMix, 0.0f, 1.0f, 1.0f});
    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto& e = static_cast<Instance*>(p)->engine;
        e.set_density_hz(params.value_at(kDensityHz, 0));
        e.set_grain_ms(params.value_at(kGrainMs, 0));
        e.set_position(params.value_at(kPosition, 0));
        e.set_position_spray_ms(params.value_at(kPositionSprayMs, 0));
        e.set_pitch_semitones(params.value_at(kPitchSt, 0));
        e.set_pitch_spray_semitones(params.value_at(kPitchSpraySt, 0));
        e.set_pan_spray(params.value_at(kPanSpray, 0));
        e.set_async_jitter(params.value_at(kJitter, 0));
        e.set_level_db(params.value_at(kLevelDb, 0));
        e.set_mix(params.value_at(kMix, 0));
        e.process(in.channel_ptr(0), out.channel_ptr(0), out.channel_ptr(1), n);
    };
    return t;
}
} // namespace granular

}  // namespace pulp::host::synthesis

#include <pulp/host/detail/forge_synthesis_descriptors.hpp>

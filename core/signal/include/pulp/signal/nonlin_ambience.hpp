#pragma once

/// @file nonlin_ambience.hpp
/// The nonlin / gated ambience engine — a reverb whose envelope is DESIGNED
/// rather than decayed.
///
/// ## Why this is not another reverb
///
/// A Schroeder or FDN tank imposes its envelope through feedback: energy
/// recirculates and dies at a rate set by the loop gain, so the amplitude
/// contour is monotone falling by construction. Such a tank cannot rise, hold
/// flat, and then cut — no setting of any knob makes it do so, because the
/// shape is a consequence of the topology rather than a parameter of it.
///
/// This module has **no recursion in the wet path**. The wet output is a finite
/// impulse response, `y[n] = Σ_k g_k · x[n − d_k]`, whose tap positions `d_k`
/// come from a velvet-noise grid and whose tap gains `g_k` trace an arbitrary
/// designed curve `E(τ)`. The envelope is literally the gain sequence, so any
/// shape that can be drawn can be played back:
///
///   * **Ambience** — a short exponential-ish falling cloud; the "natural" one.
///   * **Gated** — flat body, then a hard cut. The 80s snare-in-a-box.
///   * **Reverse** — amplitude rises to a peak, then cuts. The swell / suck-in.
///   * **NonLin2** — a rippled body with a shaped attack and an abrupt gate;
///     the contour that never decays like a room.
///
/// The finiteness is testable and is tested: with `set_diffusion(0)` the whole
/// wet path is a pure FIR, and the rendered impulse response is **bit-exactly
/// zero** past the last tap. A recursive design cannot produce that sample.
///
/// ## Honest gap — the lineage claim, stated precisely
///
/// The four program names above are documented product history of the AMS
/// RMX16. **No academic paper documents that machine**, and nothing here is
/// measured from it, reverse-engineered from it, or derived from any
/// proprietary constant. The topology combines two published algorithm
/// lineages, and every number is either cited
/// to one of them or carries an explicit `[design parameter]` default and
/// range:
///
///   1. **Velvet noise** — a sparse ternary sequence (mostly zeros, occasional
///      ±1) that sounds smooth at surprisingly low pulse density. Term and
///      method: Karjalainen & Järveläinen, "Reverberation modeling using velvet
///      noise", AES 30th Int. Conf., 2007. Grid / jitter / sign / density
///      recipe: Välimäki, Holm-Rasmussen, Alary & Lehtonen, "Late Reverberation
///      Synthesis Using Filtered Velvet Noise", Applied Sciences 7(5):483,
///      2017 (DOI 10.3390/app7050483). Non-exponential (rising, plateau,
///      arbitrary) velvet envelopes: Fagerström, Schlecht & Välimäki,
///      "Non-Exponential Reverberation Modeling Using Dark Velvet Noise", JAES,
///      2024 — the direct literature support for the gated / reverse / nonlin
///      idea. Stereo decorrelation from independent realizations: Välimäki &
///      Prawda, "Late-Reverberation Synthesis Using Interleaved Velvet-Noise
///      Sequences", IEEE/ACM TASLP, 2021.
///   2. **Multitap early reflections and reflection-density growth** — Moorer,
///      "About This Reverberation Business", Computer Music Journal 3(2):13–28,
///      1979, for the multitap model and the geometric-acoustics result that
///      reflection density grows as `t²`. Allpass diffusion primitive:
///      Schroeder & Logan, "'Colorless' Artificial Reverberation", IRE
///      Transactions on Audio AU-9(6):209–214, 1961.
///
/// The optional converter-character stage (`set_converter_amount`) is likewise
/// **not** a model of any hardware converter: no verifiable public spec exists,
/// so every value in it is a design parameter and the stage is off by default.
///
/// ## Signal flow
///
/// ```
///   x[n] (mono sum) ─► 2 Schroeder allpasses (the ONLY recursion) ─► d[n]
///                                                                    │
///        ┌───────────────────────────────────────────────────────────┘
///        ▼
///   VELVET TAP CLOUD — feedforward FIR, where the envelope lives
///     one shared ring buffer holding d[n]
///     L: tap table {d_k, g_k} keyed on seed_L      R: keyed on seed_R
///     g_k = sign_k · E(τ_k) · sqrt(Td(τ_k)) · norm ,  τ_k = (d_k − predelay)/L
///     taps grouped into S time segments → S one-pole tilts per channel
///        │
///        ▼  wet_L, wet_R
///   optional converter character (off by default: bandlimit → quantize+dither
///                                 → DC block)
///        │
///        ▼
///   width (mid/side) ─► output trim ─► dry/wet mix ─► out
/// ```
///
/// ## Anti-aliasing policy (series law 4)
///
/// **The default path is linear, so there is nothing to alias.** The allpasses,
/// the tap cloud, the segment tilts, the width matrix and the mix are all LTI;
/// none of them generates a harmonic that was not in the input. The only
/// nonlinearity in the file is the converter stage's quantizer, and it exists
/// *for* its lo-fi character. Its alias control is the mandated pre-bandlimit to
/// `[kConverterFcLo, kConverterFcHi]`; a caller who wants clean bit reduction
/// wraps the stage in the house 65-tap Kaiser (β = 8) half-band pair with
/// even-phase decimation, which is the shared series standard and is
/// deliberately not respecified here. ADAA does not apply to quantization (it is
/// a staircase, not a smooth waveshaper) and is not used.
///
/// ## Small-signal gain (series law 1)
///
/// **Law 1 is not applicable to the wet tail by construction**: it is
/// feedforward, so there is no loop and no loop gain to bound or compensate.
/// The worst-case gain is not a measurement but a closed form of the shipped
/// constants — the coherent sum of the tap gains, times the allpass chain's
/// time-domain L1 gain:
///
/// ```
///   worst_case_gain = Π_i (1 + 2·g_i) · G_L1 = (1 + 2·0.70)² · 4.0 = 23.04
/// ```
///
/// An allpass has frequency-domain magnitude exactly 1 but a time-domain L1
/// (peak) gain of `1 + 2g`, from `h[0] = −g`, `h[kM] = (1−g²)g^{k−1}` (k ≥ 1),
/// giving `Σ|h| = g + (1−g²)/(1−g) = 1 + 2g`. Young's convolution inequality
/// then bounds the cascade. `worst_case_gain()` returns this for the current
/// diffusion setting; the test suite renders the actual impulse response and
/// asserts the measured `Σ_n |h[n]|` stays under it, so the registry constant
/// cites a tested invariant rather than an estimate (series law 8). The bound
/// is stated at the shipped `output_gain_db = 0`; that trim scales it by
/// `db_to_linear(output_gain_db)` and nothing else in the chain can exceed it
/// (each segment one-pole has L1 = 1, the width matrix is a convex combination,
/// and the dry/wet law is `(1−m, m)`).
///
/// ## Latency (series law 5)
///
/// `latency_samples()` returns **0**, exactly. The dry path is a straight wire;
/// the wet tail's pre-delay is the effect, not reported latency.
///
/// ## Composition — what this file does not own
///
///   * `rng.hpp` — `mix64`, the stateless keyed hash, generates every tap's
///     jitter and sign. Keying on the tap index rather than advancing a stream
///     makes a tap's position independent of the order taps are generated in.
///     `Xorshift32` drives the converter stage's dither.
///   * `delay_line.hpp` — `DelayLineT` is the storage for the two allpasses.
///   * `units.hpp` — every ms↔sample and dB↔linear conversion.
///   * `crossfade.hpp` — the program-swap blend (smoothstep + equal power) and
///     the dry/wet law (equal gain) both come from the shared gain law.
///   * `tpt_filter.hpp` — the converter stage's band-limiting pair.
///   * `dc_blocker.hpp` — the converter stage's DC removal.
///   * `smoothed_value.hpp` — every continuous gain parameter.
///   * `denormal.hpp` — `snap_to_zero` on every recursive state.
///
/// Two composition exceptions, both deliberate and both with the arithmetic:
///
///   1. **The tap ring is local, not a `DelayLineT`.** `DelayLineT::read(int)`
///      costs an integer division per call. This module performs one read per
///      tap per sample: ~750 taps per channel for a 400 ms field, two channels,
///      48 kHz → 7.2·10⁷ reads/second. At ~6 cycles of division throughput that
///      addressing would cost more than the multiply-adds it addresses. A
///      power-of-two ring with mask indexing makes the same read one `and`. The
///      allpasses, which read once per sample each, do compose `DelayLineT`.
///   2. **The segment tilt is a local one-pole, not a `TptFilterT`.** §4.5 of
///      the spec fixes the coefficient as `a = exp(−2π·fc/fs)` (the impulse-
///      invariant pole); `TptFilterT` uses the bilinear/TPT mapping, which is a
///      different filter. The local form also snaps its state through
///      `snap_to_zero`, which the tail-flush behaviour depends on.
///
/// `DryWetMixerT` is not composed: it is a block-oriented mixer that owns a dry
/// copy and a latency buffer, and this path is per-sample at zero latency. The
/// gain law it would apply is taken directly from `crossfade_gains` instead, so
/// there is still exactly one implementation of the law in the tree.
///
/// ## Where the shipped behaviour departs from the build spec
///
/// Four adjudications, each with the numbers that forced it. They are recorded
/// here because the spec is the contract and a silent divergence is a defect.
///
///   1. **Tap gains carry a `sqrt(Td)` density weight.** The spec's §4.3 gain
///      law is `g_k = sign_k·E(τ_k)·norm`, and its §7 T1 measures a short-time
///      RMS envelope and compares it to `E`. Those two cannot both hold. A
///      window of `N` samples holding `K = Nd·N/fs` taps of magnitude `a` has
///      RMS `a·sqrt(Nd/fs)`, so with the shipped density-growth law the measured
///      envelope is `E(τ)·sqrt(Nd(τ))` — at the shipped `γ = 2` that is a
///      **+7.0 dB** rise across an Ambience tail (`sqrt(4000/800)`) and a
///      **+4.7 dB** rise across a Gated body that T1 requires to be flat within
///      ±1.0 dB. Weighting each tap by `sqrt(Td(τ_k)) = sqrt(fs/Nd(τ_k))` makes
///      the short-time RMS exactly `E(τ)·norm` independent of density, which is
///      what "the envelope is what you asked for" has to mean for a module whose
///      entire product is the envelope. The L1 budget of §4.4 and therefore
///      `worst_case_gain` are unchanged: `norm` is still solved so that
///      `Σ|g_k| = G_L1` exactly.
///   2. **No DC blocker in the default output path.** The spec places one after
///      the tap cloud. The wet path is linear, so it cannot create DC the input
///      did not have; and a DC blocker is IIR, with a time-domain L1 gain of
///      exactly 2 for any pole (`h[0] = 1`, `h[n] = −(1−p)p^{n−1}`, `Σ|h| = 2`).
///      Making it unconditional would therefore double the shipped gain bound to
///      46.08 *and* destroy this module's one distinguishing, testable property
///      — a finite impulse response. The blocker is instead part of the
///      converter stage, where the quantizer genuinely can introduce an offset,
///      and `worst_case_gain()` reports the doubled bound when that stage is on.
///   3. **`width` is a mid/side law, not a re-seed.** §4.6 describes width as
///      crossfading toward "both channels from `seed_L`", but §1 and the
///      parameter table both classify `width` as continuous — a parameter that
///      must never trigger a rebuild. Re-seeding is a rebuild. The shipped law
///      is `out = mid + w·(ch − mid)` with `mid = ½(L+R)`, which is
///      rebuild-free, is exactly mono at `w = 0` (so the spec's "L ≡ R
///      bit-identical" assertion holds), and is exactly the two independent
///      realizations at `w = 1`.
///   4. **`diffusion` does not trigger a rebuild.** It is listed as a topology
///      parameter, but it changes two scalars in the allpasses and no tap: the
///      FIR is untouched. Regenerating ~1500 taps and running a 20 ms crossfade
///      for a coefficient change that is already state-continuous would be cost
///      with no benefit.
///
/// RT contract: `prepare(sample_rate, max_length_ms)` allocates the circular
/// delay line (`predelay_max + L_max + 1` samples, rounded up to a power of two)
/// and the tap-table capacity (`ceil(kNdMax · max_length_ms/1000) + kTapGuard`
/// taps per channel per bank — headroom for the worst-case tap count the
/// jittered grid can produce above its expected value), and may allocate.
/// `rebuild()` regenerates tap tables into the pre-sized **back** bank and never
/// allocates; it is invoked from `prepare` and from topology `set_*`. The audio
/// path consumes the **front** bank and swaps to the back one over a
/// `kSwapFadeMs` equal-power crossfade. `process` and `reset` never allocate,
/// lock, or throw. All randomness is `mix64`-keyed from `seed_L` (`seed_R`
/// derives deterministically) plus an `Xorshift32` dither stream rewound at
/// `reset`; seeds are parameters, never automated. `set_*` are control-thread
/// calls: they are allocation-free and lock-free, but the front/back swap is a
/// plain index write, so a host that drives them from a non-audio thread must
/// provide the hand-off.

#include <pulp/signal/crossfade.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// The four programs. The names are documented RMX16 product history; the
/// shapes below are this module's own designs derived from the cited lineages.
enum class NonlinProgram {
    ambience = 0,  ///< Short falling cloud — the "natural" one.
    gated = 1,     ///< Flat body, hard cut.
    reverse = 2,   ///< Rise to a peak, then cut.
    nonlin2 = 3,   ///< Rippled body, shaped attack, hard gate.
};

/// The calibration register: every number this module ships that is not cited
/// to the literature lives here with a default and a range, so a reviewer finds
/// them in one place rather than spread through the code.
namespace nonlin_ambience {

/// Number of pre-diffusion Schroeder allpasses.
/// [design parameter] default 2, range 0 .. 4.
inline constexpr int kNumAllpass = 2;

/// Allpass delays in ms, rounded to the nearest sample and then nudged to the
/// nearest prime sample count so the two combs never align.
/// [design parameter] default {4.77, 6.13}, range 1 .. 30 ms each.
inline constexpr double kAllpassDelaysMs[2] = {4.77, 6.13};

/// Allpass coefficient, exposed as `diffusion`. `|g| < 1` guarantees stability.
/// [design parameter] default 0.70, range 0 .. 0.85.
inline constexpr double kDiffusionDefault = 0.70;
inline constexpr double kDiffusionMax = 0.85;

/// Instantaneous pulse density at the start and end of the window, in
/// pulses/second. Velvet noise is subjectively smooth by ≈2000 pulses/s and
/// fully smooth by ≈4000 (Karjalainen & Järveläinen 2007; Välimäki et al.
/// 2017), so the upper value is what guarantees a smooth late field.
/// [design parameter] defaults 800 / 4000, ranges 200 .. 2000 / 2000 .. 6000.
inline constexpr double kNdMin = 800.0;
inline constexpr double kNdMax = 4000.0;

/// The `density_pct` value at which `Nd_min` equals `kNdMin` exactly, so the
/// shipped default reproduces the shipped constant rather than a scaled version
/// of it. [design parameter] default 60 %, range 10 .. 100 %.
inline constexpr double kDensityRefPct = 60.0;

/// Density growth exponent γ. 2.0 is the physical `t²` reflection-density
/// growth of geometric acoustics (Moorer 1979); 0 is flat density, which is how
/// the gated and reverse programs deliberately break physics.
/// [design parameter] default 2.0, range 0 .. 2.
inline constexpr double kGammaDefault = 2.0;

/// Ambience drop across the window, in dB; sets `α = A_drop·ln(10)/20`.
/// [design parameter] default 60 dB, range 20 .. 80 dB.
inline constexpr double kAmbDropDb = 60.0;

/// Linear rise over the first fraction of the window, which kills the
/// first-tap click on the programs that would otherwise start at full gain.
/// [design parameter] default 0.02, range 0 .. 0.1.
inline constexpr double kFadeInFrac = 0.02;

/// Gated program: normalized hold point `h` and fall width `w`.
/// [design parameter] defaults 0.70 / 0.05, ranges 0.1 .. 0.95 / 0.01 .. 0.2.
inline constexpr double kGateHold = 0.70;
inline constexpr double kGateFall = 0.05;

/// Reverse program: normalized rise point `r` and swell exponent `p`
/// (`p < 1` concave, `p > 1` convex).
/// [design parameter] defaults 0.85 / 1.0, ranges 0.5 .. 0.98 / 0.5 .. 2.0.
inline constexpr double kRevRise = 0.85;
inline constexpr double kRevPow = 1.0;

/// NonLin2 program: number of raised-cosine humps, ripple depth, gate point.
/// [design parameter] defaults 2 / 0.4 / 0.85, ranges 1 .. 4 / 0 .. 0.8 /
/// 0.05 .. 0.98.
inline constexpr int kNlHumps = 2;
inline constexpr double kNlDepth = 0.4;
inline constexpr double kNlHold = 0.85;

/// The per-channel L1 budget of the tap-gain vector. This sets both the loudness
/// and the peak-gain bound: `Σ_k |g_k| = G_L1` exactly, by construction.
/// [design parameter] default 4.0, range 1.0 .. 8.0.
inline constexpr double kL1Budget = 4.0;

/// Number of time segments the tap range is split into for spectral tilt.
/// Per-tap filtering would be O(taps × filter); this is O(taps + S).
/// [design parameter] default 8, range 4 .. 16.
inline constexpr int kSegments = 8;

/// Segment tilt corners. `kFcDark` is the default of the `hf_damp_hz` parameter.
/// [design parameter] defaults 16000 / 6000 Hz, ranges 4k .. 20k / 1k .. 18k.
inline constexpr double kFcBright = 16000.0;
inline constexpr double kFcDark = 6000.0;

/// Equal-power crossfade length applied when a topology change swaps tap banks.
/// [design parameter] default 20 ms, range 5 .. 100 ms.
inline constexpr double kSwapFadeMs = 20.0;

/// Smoothing time for the continuous gain parameters.
/// [design parameter] default 20 ms, range 1 .. 200 ms.
inline constexpr double kParamSmoothMs = 20.0;

/// `seed_R = seed_L ⊕ kSeedRodd`. **Honest gap:** no citable literature exists
/// for this exact 32-bit constant's adoption here. It is the conventional
/// golden-ratio-derived (`2³²/φ`) multiplicative-hash odd constant, used only
/// because it disperses bits well; any odd 32-bit constant with good dispersion
/// would decorrelate the two seeds equally. A fixed algorithmic constant with a
/// numerical rationale, not a musical design parameter — no range applies.
inline constexpr std::uint32_t kSeedRodd = 0x9E3779B9u;

/// Default `seed_L`. Seeds are a preset choice, never automated (series law 2).
/// [design parameter] default 0x51ED, range 0 .. 2³¹−1.
inline constexpr std::uint32_t kDefaultSeed = 0x51EDu;

/// Headroom above the expected tap count, so the jittered grid can never
/// outgrow the pre-sized table.
/// [design parameter] default 32 taps, range 0 .. 128.
inline constexpr int kTapGuard = 32;

/// Converter-character pre-filter corners (§5). No verifiable public converter
/// spec exists for any hardware, so these are ours.
/// [design parameter] defaults 80 / 15000 Hz, ranges 20 .. 200 / 10k .. 20k Hz.
inline constexpr double kConverterFcLo = 80.0;
inline constexpr double kConverterFcHi = 15000.0;

/// Converter word length at `converter_amount = 0` and the depth of the sweep:
/// `bits = kConverterBitsMax − round(kConverterBitSweep · amount)`.
/// [design parameter] defaults 16 / 4, ranges 8 .. 24 / 0 .. 8.
inline constexpr int kConverterBitsMax = 16;
inline constexpr int kConverterBitSweep = 4;

/// DC-blocker pole used by the converter stage.
/// [design parameter] default 0.995 (≈3.5 Hz at 44.1 kHz), range 0.9 .. 0.9999.
inline constexpr double kConverterDcPole = 0.995;

/// Largest window and pre-delay `prepare()` sizes storage for, in ms. These are
/// the maxima of the corresponding parameter-table rows.
/// [design parameter] defaults 2000 / 200 ms, ranges 100 .. 8000 / 0 .. 1000 ms.
inline constexpr double kMinLengthMs = 20.0;
inline constexpr double kMaxLengthMs = 2000.0;
inline constexpr double kMaxPredelayMs = 200.0;

/// Lower bound of the density control's documented 10 .. 100 % range.
inline constexpr double kMinDensityPct = 10.0;

/// Floor below which the normalization sum is treated as empty, so a program
/// whose taps all land in a zeroed region produces silence rather than a
/// division by a denormal. Guards a divide; any value far below the smallest
/// meaningful L1 sum works. Range 1e-30 .. 1e-9.
inline constexpr double kNormFloor = 1e-20;

/// The time-domain L1 (peak) gain of one Schroeder allpass with coefficient
/// `g`: `Σ|h| = g + (1−g²)/(1−g) = 1 + 2g`. Exact, not an approximation.
constexpr double allpass_l1_gain(double g) { return 1.0 + 2.0 * (g < 0.0 ? -g : g); }

/// The time-domain L1 gain of the DC blocker, which is exactly 2 for any pole
/// `p < 1`: `h[0] = 1`, `h[n] = −(1−p)p^{n−1}`, so `Σ|h| = 1 + (1−p)/(1−p)`.
inline constexpr double kDcBlockerL1Gain = 2.0;

/// The shipped worst-case gain as a closed form of the shipped constants:
/// `Π_i (1 + 2·g_i) · G_L1`, an upper bound on `Σ_n |h[n]|` of the rendered
/// impulse response by Young's convolution inequality. `converter_on` adds the
/// converter stage's DC blocker. This is the value the registry cites and the
/// value the test suite asserts the rendered response stays under.
inline double worst_case_gain(double diffusion = kDiffusionDefault,
                              bool converter_on = false) {
    double g = 1.0;
    for (int i = 0; i < kNumAllpass; ++i) g *= allpass_l1_gain(diffusion);
    return g * kL1Budget * (converter_on ? kDcBlockerL1Gain : 1.0);
}

/// The dimensionless program envelope `E(τ)`, τ ∈ [0, 1] (series law 7 — scale
/// invariant, so changing `length_ms` rescales time without refitting anything).
///
/// `gate_hold` is the Gated program's `h`; `attack` is the Reverse program's `r`
/// and the NonLin2 program's `h`. Both are normalized fractions, not percents.
/// Neither is read by Ambience.
inline double program_envelope(NonlinProgram program,
                               double tau,
                               double gate_hold = kGateHold,
                               double attack = kRevRise) {
    if (!(tau >= 0.0) || tau > 1.0) return 0.0;

    const double fade_in = kFadeInFrac > 0.0 ? std::min(1.0, tau / kFadeInFrac) : 1.0;

    switch (program) {
        case NonlinProgram::ambience: {
            // α from the drop in dB: E(1) = 10^(−A_drop/20).
            constexpr double kLn10 = 2.302585092994045684;
            const double alpha = kAmbDropDb * kLn10 / 20.0;
            return fade_in * std::exp(-alpha * tau);
        }
        case NonlinProgram::gated: {
            const double h = gate_hold;
            const double w = kGateFall;
            if (tau <= h) return 1.0;
            if (tau <= h + w) return 1.0 - (tau - h) / w;
            return 0.0;
        }
        case NonlinProgram::reverse: {
            const double r = attack > 0.0 ? attack : 1e-6;
            if (tau <= r) return std::pow(tau / r, kRevPow);
            return 1.0;  // brief plateau; the hard cut is at τ > 1, handled above
        }
        case NonlinProgram::nonlin2: {
            const double h = attack;
            if (tau > h) return 0.0;
            constexpr double kTwoPi = 6.283185307179586476925286766559;
            const double ripple =
                0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(kNlHumps) * tau);
            return fade_in * ((1.0 - kNlDepth) + kNlDepth * ripple);
        }
    }
    return 0.0;
}

/// Instantaneous pulse density in pulses/second at normalized time `u ∈ [0,1]`:
/// `Nd(u) = Nd_min + (Nd_max − Nd_min)·u^γ`. The `u^γ` law is the geometric-
/// acoustics `t²` density growth (Moorer 1979) with γ exposed, because the
/// gated and reverse programs deliberately break that physics.
inline double pulse_density(double u, double density_pct, double gamma) {
    const double nd_min = kNdMin * (density_pct / kDensityRefPct);
    const double clamped = std::clamp(u, 0.0, 1.0);
    return nd_min + (kNdMax - nd_min) * std::pow(clamped, gamma);
}

}  // namespace nonlin_ambience

/// The nonlin / gated ambience engine. Stereo, processed in place.
template <typename SampleType = float>
class NonlinAmbienceT {
public:
    /// One velvet tap: where to read, how much to scale it, and which spectral
    /// segment it belongs to. `segment` is precomputed at rebuild so the audio
    /// path never divides.
    struct Tap {
        int delay = 0;
        float gain = 0.0f;
        int segment = 0;
    };

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Allocates the tap ring and both tap banks. The only function here that
    /// may allocate. `max_length_ms` and `max_predelay_ms` size the storage; no
    /// later `set_*` can outgrow them because both are clamped to these maxima.
    void prepare(double sample_rate,
                 double max_length_ms = nonlin_ambience::kMaxLengthMs,
                 double max_predelay_ms = nonlin_ambience::kMaxPredelayMs) {
        namespace na = nonlin_ambience;

        sample_rate_ = (std::isfinite(sample_rate) && sample_rate > 0.0) ? sample_rate
                                                                         : 48000.0;
        max_length_ms_ = std::isfinite(max_length_ms)
                             ? std::max(nonlin_ambience::kMinLengthMs, max_length_ms)
                             : nonlin_ambience::kMaxLengthMs;
        max_predelay_ms_ = std::isfinite(max_predelay_ms) ? std::max(0.0, max_predelay_ms)
                                                          : nonlin_ambience::kMaxPredelayMs;

        const int max_len_samples = static_cast<int>(std::ceil(
            units::ms_to_samples(max_length_ms_, sample_rate_)));
        const int max_pre_samples = static_cast<int>(std::ceil(
            units::ms_to_samples(max_predelay_ms_, sample_rate_)));

        // Power-of-two ring: the tap read is the hot loop and a mask is one
        // instruction where a modulo is a division. See the file note.
        ring_mask_ = static_cast<int>(next_pow2(static_cast<std::size_t>(
                         max_len_samples + max_pre_samples + 2))) - 1;
        ring_.assign(static_cast<std::size_t>(ring_mask_) + 1u, SampleType{0});
        ring_write_ = 0;

        // A grid step never advances less than fs/Nd_max samples, so the number
        // of steps inside an L-sample window is at most L·Nd_max/fs — which is
        // (max_length_ms/1000)·Nd_max, independent of the sample rate.
        tap_capacity_ = static_cast<int>(std::ceil(
                            na::kNdMax * max_length_ms_ / 1000.0)) + na::kTapGuard;
        for (int bank = 0; bank < 2; ++bank)
            for (int ch = 0; ch < 2; ++ch) {
                taps_[bank][ch].assign(static_cast<std::size_t>(tap_capacity_), Tap{});
                tap_count_[bank][ch] = 0;
            }

        for (int i = 0; i < na::kNumAllpass; ++i) {
            allpass_len_[i] = nearest_prime(std::max(2, static_cast<int>(std::lround(
                units::ms_to_samples(na::kAllpassDelaysMs[i], sample_rate_)))));
            allpass_[i].prepare(allpass_len_[i] + 1);
        }

        swap_fade_len_ = std::max(1, static_cast<int>(std::lround(
            units::ms_to_samples(na::kSwapFadeMs, sample_rate_))));

        const auto smooth_s = static_cast<SampleType>(na::kParamSmoothMs / 1000.0);
        const auto fs = static_cast<SampleType>(sample_rate_);
        mix_.set_ramp_time(smooth_s, fs);
        width_.set_ramp_time(smooth_s, fs);
        output_gain_.set_ramp_time(smooth_s, fs);
        converter_.set_ramp_time(smooth_s, fs);

        for (int ch = 0; ch < 2; ++ch) {
            conv_hp_[ch].prepare(static_cast<SampleType>(sample_rate_));
            conv_hp_[ch].set_cutoff(static_cast<SampleType>(na::kConverterFcLo));
            conv_lp_[ch].prepare(static_cast<SampleType>(sample_rate_));
            conv_lp_[ch].set_cutoff(static_cast<SampleType>(na::kConverterFcHi));
            conv_dc_[ch].set_pole(static_cast<SampleType>(na::kConverterDcPole));
        }

        update_segment_coefficients();
        rebuild_immediate();
        reset();
    }

    /// Clears every state. Never allocates. Tap tables survive — they are a pure
    /// function of the parameters, not of the signal history.
    void reset() {
        std::fill(ring_.begin(), ring_.end(), SampleType{0});
        ring_write_ = 0;
        for (int i = 0; i < nonlin_ambience::kNumAllpass; ++i) allpass_[i].reset();
        for (auto& bank : seg_state_)
            for (auto& ch : bank) ch.fill(SampleType{0});
        for (int ch = 0; ch < 2; ++ch) {
            conv_hp_[ch].reset();
            conv_lp_[ch].reset();
            conv_dc_[ch].reset();
        }
        dither_.reset();
        mix_.set_immediate(mix_.target());
        width_.set_immediate(width_.target());
        output_gain_.set_immediate(output_gain_.target());
        converter_.set_immediate(converter_.target());
        // A pending swap is resolved rather than carried across a reset: the
        // crossfade is a signal-domain artifact and there is no signal now.
        if (fading_) {
            front_ = pending_;
            fading_ = false;
        }
        fade_pos_ = 0;
    }

    /// Series law 5: exactly zero. The dry path is a straight wire and the wet
    /// pre-delay is the effect, not latency.
    static constexpr int latency_samples() { return 0; }

    // ── Topology parameters (rebuild the FIR) ─────────────────────────────

    void set_program(NonlinProgram program) {
        if (program == program_) return;
        program_ = program;
        rebuild_and_swap();
    }

    /// Window length in ms — the absolute time the dimensionless envelope is
    /// stretched across. Clamped to the `prepare()` maximum.
    void set_length_ms(double ms) {
        if (!std::isfinite(ms)) return;
        const double v = std::clamp(ms, nonlin_ambience::kMinLengthMs, max_length_ms_);
        if (v == length_ms_) return;
        length_ms_ = v;
        rebuild_and_swap();
    }

    void set_predelay_ms(double ms) {
        if (!std::isfinite(ms)) return;
        const double v = std::clamp(ms, 0.0, max_predelay_ms_);
        if (v == predelay_ms_) return;
        predelay_ms_ = v;
        rebuild_and_swap();
    }

    /// Scales `Nd_min` only; `Nd_max` is fixed so the late field stays above the
    /// smoothness floor no matter how sparse the early field is made.
    void set_density_pct(double pct) {
        if (!std::isfinite(pct)) return;
        const double v = std::clamp(pct, nonlin_ambience::kMinDensityPct, 100.0);
        if (v == density_pct_) return;
        density_pct_ = v;
        rebuild_and_swap();
    }

    void set_density_growth(double gamma) {
        if (!std::isfinite(gamma)) return;
        const double v = std::clamp(gamma, 0.0, 2.0);
        if (v == density_growth_) return;
        density_growth_ = v;
        rebuild_and_swap();
    }

    /// The Gated program's hold point, as a percent of the window.
    void set_gate_hold_pct(double pct) {
        if (!std::isfinite(pct)) return;
        const double v = std::clamp(pct, 10.0, 95.0);
        if (v == gate_hold_pct_) return;
        gate_hold_pct_ = v;
        rebuild_and_swap();
    }

    /// The Reverse program's rise point and the NonLin2 program's gate point,
    /// as a percent of the window. Ignored by Ambience and Gated.
    void set_attack_pct(double pct) {
        if (!std::isfinite(pct)) return;
        const double v = std::clamp(pct, 5.0, 98.0);
        if (v == attack_pct_) return;
        attack_pct_ = v;
        rebuild_and_swap();
    }

    /// Atomically applies the complete topology snapshot and regenerates the
    /// inactive tap bank at most once. Catalog/host block boundaries use this
    /// instead of invoking seven independently rebuilding setters.
    void set_topology(NonlinProgram program, double length_ms, double predelay_ms,
                      double density_pct, double density_growth,
                      double gate_hold_pct, double attack_pct) {
        const double length = std::isfinite(length_ms)
                                  ? std::clamp(length_ms, nonlin_ambience::kMinLengthMs,
                                               max_length_ms_)
                                  : length_ms_;
        const double predelay = std::isfinite(predelay_ms)
                                    ? std::clamp(predelay_ms, 0.0, max_predelay_ms_)
                                    : predelay_ms_;
        const double density = std::isfinite(density_pct)
                                   ? std::clamp(density_pct,
                                                nonlin_ambience::kMinDensityPct, 100.0)
                                   : density_pct_;
        const double growth = std::isfinite(density_growth)
                                  ? std::clamp(density_growth, 0.0, 2.0)
                                  : density_growth_;
        const double hold = std::isfinite(gate_hold_pct)
                                ? std::clamp(gate_hold_pct, 10.0, 95.0)
                                : gate_hold_pct_;
        const double attack = std::isfinite(attack_pct)
                                  ? std::clamp(attack_pct, 5.0, 98.0)
                                  : attack_pct_;
        if (program == program_ && length == length_ms_ && predelay == predelay_ms_ &&
            density == density_pct_ && growth == density_growth_ &&
            hold == gate_hold_pct_ && attack == attack_pct_)
            return;
        program_ = program;
        length_ms_ = length;
        predelay_ms_ = predelay;
        density_pct_ = density;
        density_growth_ = growth;
        gate_hold_pct_ = hold;
        attack_pct_ = attack;
        rebuild_and_swap();
    }

    std::uint64_t topology_rebuild_count() const { return topology_rebuild_count_; }

    /// Seed for the L channel; R derives as `seed ⊕ kSeedRodd`. Series law 2 —
    /// a preset choice, never automated.
    void set_seed(std::uint32_t seed) {
        if (seed == seed_) return;
        seed_ = seed;
        rebuild_and_swap();
    }

    /// Allpass coefficient. Touches two scalars and no tap, so it does not
    /// rebuild (see the file note). Clamped below 1 for stability.
    void set_diffusion(double g) {
        if (!std::isfinite(g)) return;
        diffusion_ = std::clamp(g, 0.0, nonlin_ambience::kDiffusionMax);
    }

    // ── Continuous parameters (smoothed, never rebuild) ───────────────────

    /// Spectral tilt `T ∈ [−1, +1]`: positive keeps highs later (brighter),
    /// negative darkens. Recomputes the segment coefficients rather than
    /// smoothing them — a one-pole corner change is state-continuous, so it
    /// produces no step in the output the way a gain change would.
    void set_tone(double tone) {
        if (!std::isfinite(tone)) return;
        const double v = std::clamp(tone, -1.0, 1.0);
        if (v == tone_) return;
        tone_ = v;
        update_segment_coefficients();
    }

    void set_hf_damp_hz(double hz) {
        if (!std::isfinite(hz)) return;
        const double v = std::clamp(hz, 1000.0, 18000.0);
        if (v == hf_damp_hz_) return;
        hf_damp_hz_ = v;
        update_segment_coefficients();
    }

    /// 0 % is mono (both channels exactly the mid signal), 100 % is the two
    /// independent velvet realizations.
    void set_width_pct(double pct) {
        if (!std::isfinite(pct)) return;
        width_.set_target(static_cast<SampleType>(std::clamp(pct, 0.0, 100.0) / 100.0));
    }

    /// 0 disables the converter stage entirely.
    void set_converter_amount(double amount) {
        if (!std::isfinite(amount)) return;
        converter_.set_target(static_cast<SampleType>(std::clamp(amount, 0.0, 1.0)));
    }

    void set_output_gain_db(double db) {
        if (!std::isfinite(db)) return;
        output_gain_.set_target(
            units::db_to_linear(static_cast<SampleType>(std::clamp(db, -24.0, 24.0))));
    }

    /// Dry/wet, 0 .. 100 %. The default is 100 (send style).
    void set_mix_pct(double pct) {
        if (!std::isfinite(pct)) return;
        mix_.set_target(static_cast<SampleType>(std::clamp(pct, 0.0, 100.0) / 100.0));
    }

    // ── Processing ────────────────────────────────────────────────────────

    /// One stereo frame, in place. Allocation-free, lock-free, branch-light.
    void process_sample(SampleType& left, SampleType& right) {
        if (!std::isfinite(static_cast<double>(left)) ||
            !std::isfinite(static_cast<double>(right))) {
            left = SampleType{0};
            right = SampleType{0};
            return;
        }
        const SampleType dry_l = left;
        const SampleType dry_r = right;

        // Mono sum into the diffuser: the tap cloud is one shared field.
        SampleType d = SampleType{0.5} * (dry_l + dry_r);
        const auto g = static_cast<SampleType>(diffusion_);
        for (int i = 0; i < nonlin_ambience::kNumAllpass; ++i) {
            const SampleType delayed = allpass_[i].read(allpass_len_[i] - 1);
            const SampleType w = snap_to_zero(d + g * delayed);
            d = -g * w + delayed;
            allpass_[i].push(w);
        }

        ring_write_ = (ring_write_ + 1) & ring_mask_;
        ring_[static_cast<std::size_t>(ring_write_)] = d;

        SampleType wet_l = SampleType{0};
        SampleType wet_r = SampleType{0};

        if (!fading_) {
            wet_l = render_bank(front_, 0);
            wet_r = render_bank(front_, 1);
        } else {
            SampleType old_gain{}, new_gain{};
            const auto u = static_cast<SampleType>(fade_pos_) /
                           static_cast<SampleType>(swap_fade_len_);
            crossfade_gains(crossfade_smoothstep(u), CrossfadeGainLaw::EqualPower,
                            old_gain, new_gain);
            wet_l = old_gain * render_bank(front_, 0) + new_gain * render_bank(pending_, 0);
            wet_r = old_gain * render_bank(front_, 1) + new_gain * render_bank(pending_, 1);
            if (++fade_pos_ >= swap_fade_len_) {
                // The retiring bank's tilt filters are cleared so a later swap
                // back into it does not start from stale state.
                seg_state_[front_][0].fill(SampleType{0});
                seg_state_[front_][1].fill(SampleType{0});
                front_ = pending_;
                fading_ = false;
                fade_pos_ = 0;
            }
        }

        const SampleType conv = converter_.next();
        if (conv > SampleType{0}) {
            wet_l = converter_stage(0, wet_l, conv);
            wet_r = converter_stage(1, wet_r, conv);
        }

        // Width: mid/side. Exactly mono at 0, exactly the two realizations at 1.
        const SampleType w = width_.next();
        const SampleType mid = SampleType{0.5} * (wet_l + wet_r);
        wet_l = mid + w * (wet_l - mid);
        wet_r = mid + w * (wet_r - mid);

        const SampleType trim = output_gain_.next();
        wet_l *= trim;
        wet_r *= trim;

        SampleType dry_gain{}, wet_gain{};
        crossfade_gains(mix_.next(), CrossfadeGainLaw::EqualGain, dry_gain, wet_gain);
        left = dry_gain * dry_l + wet_gain * wet_l;
        right = dry_gain * dry_r + wet_gain * wet_r;
    }

    /// A block of stereo frames, in place.
    void process(SampleType* left, SampleType* right, int num_samples) {
        for (int n = 0; n < num_samples; ++n) process_sample(left[n], right[n]);
    }

    // ── Introspection ─────────────────────────────────────────────────────
    //
    // The tap table IS the product of this module, so it is readable. The
    // acceptance suite asserts the realized gains against an independent
    // transcription of the envelope math, which is only possible because these
    // are public.

    /// Number of taps currently live on `channel` (0 = L, 1 = R).
    int tap_count(int channel) const { return tap_count_[front_][channel & 1]; }

    /// Tap `index` on `channel`. Delay is in samples from the write head;
    /// `segment` is its spectral-tilt group.
    const Tap& tap(int channel, int index) const {
        return taps_[front_][channel & 1][static_cast<std::size_t>(index)];
    }

    /// The L1 normalization scalar solved for `channel`'s live tap table, so a
    /// caller can predict `|g_k|` without re-deriving the normalization.
    double tap_norm(int channel) const { return tap_norm_[front_][channel & 1]; }

    /// Window length in samples — the denominator of every `τ`.
    ///
    /// Every geometry accessor describes the bank currently being heard, not the
    /// most recent `set_*`. During the `kSwapFadeMs` window after a topology
    /// change the two differ, and reporting the new geometry against the old
    /// tap table would make `tap(ch, k).delay / window_samples()` a τ that
    /// belongs to neither.
    int window_samples() const { return bank_window_[front_]; }
    int predelay_samples() const { return bank_predelay_[front_]; }
    int allpass_length(int index) const { return allpass_len_[index]; }

    /// The worst-case gain for the current settings, as a closed form of the
    /// shipped constants (series law 8).
    double worst_case_gain() const {
        return nonlin_ambience::worst_case_gain(
            diffusion_, converter_.target() > SampleType{0});
    }

    NonlinProgram program() const { return program_; }
    double length_ms() const { return length_ms_; }
    double tone() const { return tone_; }
    bool swap_in_progress() const { return fading_; }

    /// The normalized envelope this instance's current settings ask for. The
    /// program-specific `h` / `r` are resolved from the percent parameters here
    /// so callers do not have to know which program reads which.
    double envelope(double tau) const {
        return nonlin_ambience::program_envelope(program_, tau, gate_hold_pct_ / 100.0,
                                                 attack_pct_ / 100.0);
    }

private:
    // ── Tap-cloud rendering ───────────────────────────────────────────────

    SampleType render_bank(int bank, int channel) {
        std::array<SampleType, nonlin_ambience::kSegments> acc{};
        const Tap* taps = taps_[bank][channel].data();
        const int count = tap_count_[bank][channel];
        const SampleType* ring = ring_.data();
        const int write = ring_write_;
        const int mask = ring_mask_;
        for (int k = 0; k < count; ++k)
            acc[static_cast<std::size_t>(taps[k].segment)] +=
                static_cast<SampleType>(taps[k].gain) *
                ring[static_cast<std::size_t>((write - taps[k].delay) & mask)];

        SampleType out{0};
        auto& state = seg_state_[bank][channel];
        for (int s = 0; s < nonlin_ambience::kSegments; ++s) {
            const SampleType a = seg_pole_[static_cast<std::size_t>(s)];
            state[static_cast<std::size_t>(s)] = snap_to_zero(
                a * state[static_cast<std::size_t>(s)] +
                (SampleType{1} - a) * acc[static_cast<std::size_t>(s)]);
            out += state[static_cast<std::size_t>(s)];
        }
        return out;
    }

    /// Bandlimit → quantize with TPDF dither → DC block. The one nonlinearity in
    /// the file; see the anti-aliasing note.
    SampleType converter_stage(int channel, SampleType x, SampleType amount) {
        namespace na = nonlin_ambience;
        SampleType y = conv_hp_[channel].process_highpass(x);
        y = conv_lp_[channel].process_lowpass(y);

        const int bits = na::kConverterBitsMax -
                         static_cast<int>(std::lround(
                             static_cast<double>(amount) * na::kConverterBitSweep));
        const auto step = static_cast<SampleType>(
            1.0 / static_cast<double>(1 << (std::max(2, bits) - 1)));
        // TPDF dither, 1 LSB peak: the difference of two independent uniforms.
        const SampleType d =
            (dither_.next_unit<SampleType>() - dither_.next_unit<SampleType>()) * step;
        y = std::round((y + d) / step) * step;

        return conv_dc_[channel].process(y);
    }

    // ── Tap-table construction ────────────────────────────────────────────

    void rebuild_immediate() {
        build_bank(front_);
        fading_ = false;
        fade_pos_ = 0;
    }

    void rebuild_and_swap() {
        if (ring_.empty()) return;  // not prepared yet
        if (fading_) {
            // A second topology change inside one fade collapses the first: the
            // pending bank becomes live immediately so the back bank is free to
            // be rewritten. Documented, and only reachable by changing topology
            // twice within kSwapFadeMs.
            front_ = pending_;
            fading_ = false;
            fade_pos_ = 0;
        }
        const int back = 1 - front_;
        build_bank(back);
        ++topology_rebuild_count_;
        seg_state_[back][0].fill(SampleType{0});
        seg_state_[back][1].fill(SampleType{0});
        pending_ = back;
        fade_pos_ = 0;
        fading_ = true;
    }

    void build_bank(int bank) {
        namespace na = nonlin_ambience;

        const int window_samples = std::max(1, static_cast<int>(std::lround(
            units::ms_to_samples(length_ms_, sample_rate_))));
        const int predelay_samples = std::max(0, static_cast<int>(std::lround(
            units::ms_to_samples(predelay_ms_, sample_rate_))));
        bank_window_[bank] = window_samples;
        bank_predelay_[bank] = predelay_samples;

        const double gate_hold = gate_hold_pct_ / 100.0;
        const double attack = attack_pct_ / 100.0;
        const bool reverse_segments = program_ == NonlinProgram::reverse;

        for (int ch = 0; ch < 2; ++ch) {
            const std::uint32_t seed = ch == 0 ? seed_ : (seed_ ^ na::kSeedRodd);
            Tap* out = taps_[bank][ch].data();
            int count = 0;
            double l1 = 0.0;

            double t = 0.0;  // position inside the window, in samples
            int index = 0;
            while (t < static_cast<double>(window_samples) && count < tap_capacity_) {
                const double u = t / static_cast<double>(window_samples);
                const double nd = na::pulse_density(u, density_pct_, density_growth_);
                const double grid = sample_rate_ / nd;  // Td, in samples

                // mix64 is keyed on the tap index, so a tap's jitter and sign do
                // not depend on how many taps were generated before it — which
                // is what makes the layout independent of block size and of the
                // order the generator happens to walk the grid.
                const double jitter =
                    unit_from<double>(mix64(seed, static_cast<std::uint64_t>(index), 0u));
                const int sign =
                    (mix64(seed, static_cast<std::uint64_t>(index), 1u) & 1ull) ? 1 : -1;

                const int delay = std::clamp(
                    static_cast<int>(std::lround(t + jitter * std::max(0.0, grid - 1.0))),
                    0, window_samples - 1);

                const double tau = static_cast<double>(delay) /
                                   static_cast<double>(window_samples);
                const double env =
                    na::program_envelope(program_, tau, gate_hold, attack);

                // A tap whose envelope is exactly zero is past a hard gate. It
                // is dropped rather than stored, which is what makes the "IR is
                // zero after the last tap" property meaningful: the last stored
                // tap is the last audible one.
                if (env > 0.0) {
                    // sqrt(Td) density weighting — see the file note. It makes
                    // the short-time RMS envelope equal E(τ) regardless of how
                    // the density law varies underneath it.
                    const double weight = std::sqrt(grid);
                    const double magnitude = env * weight;
                    out[count].delay = predelay_samples + delay;
                    out[count].gain = static_cast<float>(sign * magnitude);
                    int segment = static_cast<int>(tau * na::kSegments);
                    segment = std::clamp(segment, 0, na::kSegments - 1);
                    out[count].segment =
                        reverse_segments ? na::kSegments - 1 - segment : segment;
                    l1 += magnitude;
                    ++count;
                }

                t += grid;
                ++index;
            }

            // §4.4: solve `norm` so Σ|g_k| == G_L1 exactly. That single
            // constraint sets both the loudness and the peak-gain bound.
            const double norm = l1 > na::kNormFloor ? na::kL1Budget / l1 : 0.0;
            for (int k = 0; k < count; ++k)
                out[k].gain = static_cast<float>(static_cast<double>(out[k].gain) * norm);

            tap_count_[bank][ch] = count;
            tap_norm_[bank][ch] = norm;
        }
    }

    // ── Segment tilt ──────────────────────────────────────────────────────

    void update_segment_coefficients() {
        namespace na = nonlin_ambience;
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        const double bright = na::kFcBright;
        const double dark = hf_damp_hz_;
        for (int s = 0; s < na::kSegments; ++s) {
            const double q = std::clamp(
                static_cast<double>(s) / static_cast<double>(na::kSegments - 1) -
                    0.5 * tone_,
                0.0, 1.0);
            const double fc = bright * std::pow(dark / bright, q);
            // §4.5's stated coefficient: the impulse-invariant pole.
            seg_pole_[static_cast<std::size_t>(s)] = static_cast<SampleType>(
                std::exp(-kTwoPi * fc / sample_rate_));
        }
    }

    // ── Small helpers ─────────────────────────────────────────────────────

    static std::size_t next_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    static bool is_prime(int n) {
        if (n < 2) return false;
        if (n % 2 == 0) return n == 2;
        for (int d = 3; d * d <= n; d += 2)
            if (n % d == 0) return false;
        return true;
    }

    /// Nudges a delay to the nearest prime sample count so the two allpass combs
    /// share no period and their echoes never align.
    static int nearest_prime(int n) {
        for (int offset = 0; offset < 64; ++offset) {
            if (is_prime(n + offset)) return n + offset;
            if (n - offset > 1 && is_prime(n - offset)) return n - offset;
        }
        return n;
    }

    // ── State ─────────────────────────────────────────────────────────────

    double sample_rate_ = 48000.0;
    double max_length_ms_ = nonlin_ambience::kMaxLengthMs;
    double max_predelay_ms_ = nonlin_ambience::kMaxPredelayMs;

    NonlinProgram program_ = NonlinProgram::ambience;
    double length_ms_ = 350.0;
    double predelay_ms_ = 0.0;
    double density_pct_ = nonlin_ambience::kDensityRefPct;
    double density_growth_ = nonlin_ambience::kGammaDefault;
    double diffusion_ = nonlin_ambience::kDiffusionDefault;
    double gate_hold_pct_ = nonlin_ambience::kGateHold * 100.0;
    double attack_pct_ = nonlin_ambience::kRevRise * 100.0;
    std::uint32_t seed_ = nonlin_ambience::kDefaultSeed;
    double tone_ = 0.0;
    double hf_damp_hz_ = nonlin_ambience::kFcDark;

    int tap_capacity_ = 0;

    std::vector<Tap> taps_[2][2];
    int tap_count_[2][2] = {{0, 0}, {0, 0}};
    double tap_norm_[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
    // Geometry is stored PER BANK so the introspection accessors can describe
    // the bank being heard rather than the most recent `set_*`.
    int bank_window_[2] = {0, 0};
    int bank_predelay_[2] = {0, 0};

    std::vector<SampleType> ring_;
    int ring_mask_ = 0;
    int ring_write_ = 0;

    DelayLineT<SampleType> allpass_[nonlin_ambience::kNumAllpass];
    int allpass_len_[nonlin_ambience::kNumAllpass] = {0, 0};

    std::array<SampleType, nonlin_ambience::kSegments> seg_pole_{};
    std::array<std::array<SampleType, nonlin_ambience::kSegments>, 2> seg_state_[2]{};

    int front_ = 0;
    int pending_ = 0;
    bool fading_ = false;
    int fade_pos_ = 0;
    std::uint64_t topology_rebuild_count_ = 0;
    int swap_fade_len_ = 1;

    SmoothedValue<SampleType> mix_{SampleType{1}};
    SmoothedValue<SampleType> width_{SampleType{1}};
    SmoothedValue<SampleType> output_gain_{SampleType{1}};
    SmoothedValue<SampleType> converter_{SampleType{0}};

    TptFilterT<SampleType> conv_hp_[2];
    TptFilterT<SampleType> conv_lp_[2];
    DcBlocker<SampleType> conv_dc_[2];
    Xorshift32 dither_{0x2C1B3A5Du};
};

using NonlinAmbience = NonlinAmbienceT<float>;
using NonlinAmbience64 = NonlinAmbienceT<double>;

}  // namespace pulp::signal

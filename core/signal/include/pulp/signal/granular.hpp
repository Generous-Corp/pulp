#pragma once

/// @file granular.hpp
/// A granular synthesis engine: a seeded grain scheduler driving a fixed pool
/// of windowed, resampled, panned grains.
///
/// Granular synthesis builds a texture out of thousands of short windowed
/// fragments — grains, typically 1–100 ms — laid down densely enough to fuse
/// (Roads, "Introduction to Granular Synthesis", CMJ 12(2):11–13, 1988;
/// Roads, *Microsound*, MIT Press, 2001). The idea descends from Gabor's
/// acoustic quantum (Gabor, "Acoustical Quanta and the Theory of Hearing",
/// *Nature* 159(4044):591–594, 1947), and the grain-rate / grain-duration /
/// density vocabulary used here is the one Truax's real-time implementation
/// established ("Real-Time Granular Synthesis with a Digital Signal
/// Processor", CMJ 12(2):14–26, 1988).
///
/// **Three independent time axes**, and keeping them independent is the whole
/// design:
///
///   - **Density** `D` — how often grains are born. Sets texture, not pitch and
///     not the source-time mapping.
///   - **Stretch** `α` — how fast the playhead traverses the source. The output
///     timeline is dilated by `1/α`; `α = 0` freezes.
///   - **Pitch** `r` — how fast each grain reads *inside* itself. Every
///     component `f` in a grain comes out at `r·f`.
///
/// They do not interact because a grain is short: the playhead decides *where
/// on the output timeline* a slice lands, the ratio decides *what pitch* that
/// slice plays at, and the window's brief support is what keeps the two from
/// reaching each other. Half-speed and a fifth up is `α = 0.5, r = 2^(7/12)`,
/// and neither number moves the other.
///
/// **Determinism is a design constraint here, not a nicety.** Every per-grain
/// random field is a *stateless keyed draw*
/// `unit_from(mix64(seed, grain_index, field))`, keyed off the monotonic grain
/// index — never the pool slot, never a block boundary. That is what makes a
/// draw independent of buffer size and of voice-steal: a stolen grain simply
/// stops, and the next grain's draws are untouched, because nothing in the
/// random stream ever advanced. A sequential generator would be reproducible
/// run-to-run and would still change its answer when the host handed you 64
/// samples instead of 512. See `rng.hpp`, whose doc block draws exactly this
/// distinction.
///
/// **Amplitude follows the √N energy law.** Mean simultaneous overlap is
/// `N̄ = D·T_g`. Grains at scattered onsets are uncorrelated, so their *powers*
/// add and output RMS grows as `√N̄` — hence `g = 1/(w_rms·√N̄)`, and doubling
/// density leaves the level where it was. Grains on a fixed grid at the same
/// source position add in *amplitude* instead, so the constant-overlap-add
/// reciprocal `g = 1/(w_mean·N̄)` applies. Those are different laws, not
/// different tunings of one: at `N̄ = 10` with a Hann window they differ by
/// 8.24 dB, which is why `set_coherence` exists.
///
/// `w_mean` and `w_rms` are measured from the shipped window table rather than
/// assumed, so the law stays correct as the taper knob moves. At full taper the
/// table reproduces the exact Hann identities `w_mean = 1/2` and
/// `w_rms = √(3/8)` to machine precision, which the suite asserts.
///
/// **Anti-aliasing policy.** The grain read is a resampler, not a static
/// nonlinearity, so the house half-band pair does not apply. Pitching *down*
/// cannot alias. Pitching *up* undersamples the source, and the only aliasing
/// mechanism is the interpolator's imaging, mitigated by the default 4-point
/// cubic Hermite. There is deliberately no engine-wide oversampling and no
/// tracking pre-decimation low-pass; over ±24 st the interpolator is the whole
/// anti-imaging story. That is a stated limitation, not a hidden one.
///
/// **Linearity.** Windowing, resampling, gain, pan, and summing are all linear,
/// and grains never write back into the ring — only `write_live` does. There is
/// no gain-carrying nonlinearity and no feedback path, so series law 1 is not
/// triggered and no small-signal-gain or unity-compensation obligation exists.
/// The worst-case gain is a feed-forward headroom figure, bounded by
/// `max_grains` times the interpolator's peak kernel gain.
///
/// Relationship to `FreezeLoopSamplerT` (freeze_loop_sampler.hpp): both keep a
/// continuously-written ring so freezing costs nothing, and there the
/// similarity ends. That class holds *one* contiguous slice and loops it with a
/// single equal-power seam, preserving the moment verbatim. This one holds a
/// *cloud* over a moment — many short overlapping slices at randomised offsets
/// and pitches, where the grain window is the crossfade and there is no seam to
/// hide. They are complements, and chaining them (freeze a loop, then granulate
/// it in buffer mode) is a real patch.
///
/// RT contract: `prepare()` sizes the live ring and the window table and is the
/// only member that allocates. `set_*`, `write_live`, `process`, `reset`, and
/// every accessor allocate nothing, take no locks, and perform no I/O; all are
/// wait-free. The grain pool is a fixed array of `kMaxGrainBudget` PODs — there
/// is no dynamic voice allocation at any point. `latency_samples()` is 0: grains
/// read past source samples only, never future ones, and the delay implied by
/// `position` is a musical offset rather than reportable codec latency. Source
/// buffers passed to `set_buffer` are **borrowed** — the engine never owns,
/// copies, or frees them, and the caller must keep them alive.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/panner.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// Where grains read from. The grain core is identical either way; only source
/// addressing differs.
enum class GrainSource : std::uint8_t {
    buffer,     ///< A borrowed static buffer with a traversable playhead.
    live_ring,  ///< A continuously captured ring of recent input.
};

/// Which normalization law applies — a statement about whether grains are
/// correlated with each other, not a tone control.
enum class Coherence : std::uint8_t {
    incoherent,  ///< Scattered grains; powers add; `1/(w_rms·√N̄)`.
    coherent,    ///< Grid-aligned grains; amplitudes add; `1/(w_mean·N̄)`.
};

/// What happens when a grain is due and the pool is full.
enum class StealPolicy : std::uint8_t {
    oldest,    ///< Recycle the grain furthest through its window — the quietest
               ///< one when durations match, so the edit is inaudible.
    quietest,  ///< Recycle the smallest current `w(phase)·g`. For mixed lengths.
    drop_new,  ///< Skip the incoming grain. Thins clouds rather than cutting.
};

/// Fractional-read quality inside a grain. Named `GrainInterp` rather than the
/// bare `Interp` the spec suggests because `pulp::signal` is a shared namespace
/// and an unqualified `Interp` would be the first thing to collide.
enum class GrainInterp : std::uint8_t {
    linear,         ///< 2-point. Cheapest; audible imaging when pitching up.
    cubic_hermite,  ///< 4-point Catmull-Rom. The default and the anti-imaging
                    ///< measure for the whole ±24 st range.
};

/// A read-only view of one grain, for metering and grain-cloud displays.
struct GrainView {
    bool active = false;
    double phase = 0.0;
    double read_position = 0.0;
    double ratio = 1.0;
    double gain = 0.0;
    double pan_left = 0.0;
    double pan_right = 0.0;
};

template <typename SampleType = float>
class GranularEngineT {
public:
    // ── Fixed capacities ──────────────────────────────────────────────────

    /// Slot capacity of the grain pool. Pinned to the top of `max_grains`'
    /// declared range rather than independently ranged: raising it would
    /// require re-deriving the worst-case gain bound.
    /// [design parameter] 64 slots, pinned to the `max_grains` range ceiling.
    static constexpr int kMaxGrainBudget = 64;

    /// Floor of the `max_grains` range.
    /// [design parameter] 32, the bottom of the declared 32..64 budget.
    static constexpr int kMinGrainBudget = 32;

    /// [design parameter] default 48 grains, range 32 .. 64.
    static constexpr int kDefaultMaxGrains = 48;

    /// Window table resolution. Powers of two only, so the phase index is a
    /// multiply. [design parameter] default 2048, range 512 .. 4096.
    static constexpr int kWindowTableSize = 2048;

    /// Live ring length in seconds. The floor of the range is not free: the
    /// causality guard below derives to a fixed 2.5 s at any sample rate, so
    /// the ring must clear that with enough margin left for a full grain span.
    /// [design parameter] default 4 s, range 3 .. 8 s.
    static constexpr double kRingSeconds = 4.0;

    // ── Parameter ranges ──────────────────────────────────────────────────

    /// [design parameter] default 40 grains/s, range 0.1 .. 2000.
    static constexpr double kDefaultDensityHz = 40.0;
    static constexpr double kMinDensityHz = 0.1;
    static constexpr double kMaxDensityHz = 2000.0;

    /// [design parameter] default 50 ms, range 1 .. 500 ms. Roads' typical
    /// granular range is 1–100 ms; the ceiling here is higher so the engine
    /// also covers slow overlapping-drone territory.
    static constexpr double kDefaultGrainMs = 50.0;
    static constexpr double kMinGrainMs = 1.0;
    static constexpr double kMaxGrainMs = 500.0;

    /// [design parameter] default 0 (synchronous clock), range 0 .. 1.
    static constexpr double kDefaultJitter = 0.0;

    /// [design parameter] default 0 ms, range 0 .. 500 ms.
    static constexpr double kMaxPositionSprayMs = 500.0;

    /// [design parameter] default 1, range 0 .. 4. Zero is freeze.
    static constexpr double kDefaultStretch = 1.0;
    static constexpr double kMaxStretch = 4.0;

    /// [design parameter] default 0 st, range −24 .. +24 st. The ±24 bound is
    /// what sets `r_max = 4` in the causality guard.
    static constexpr double kMaxPitchSemitones = 24.0;

    /// [design parameter] default 0 st, range 0 .. 12 st.
    static constexpr double kMaxPitchSpraySemitones = 12.0;

    /// [design parameter] default 0 dB, range −24 .. +12 dB.
    static constexpr double kMinLevelDb = -24.0;
    static constexpr double kMaxLevelDb = 12.0;

    /// Seed. Fixed at `reset()`, never automatable (series law 2).
    /// [design parameter] any `uint32_t`; no automation range applies.
    static constexpr std::uint32_t kDefaultSeed = 0x9E3779B9u;

    /// Control-smoothing time for output level and dry/wet. Only these two are
    /// smoothed — every per-grain parameter is captured once at spawn, so it
    /// cannot zipper.
    /// [design parameter] default 10 ms, range 1 .. 100 ms.
    static constexpr double kControlSmoothingMs = 10.0;

    /// Floor on a keyed draw before `−ln(u)`. Guards `log(0)`, which the 53-bit
    /// draw can produce with probability 2^-53. Any value far below the
    /// smallest non-zero draw works; range 1e-18 .. 1e-6.
    static constexpr double kMinDrawForLog = 1e-12;

    /// Floor on the inter-onset interval, in samples. Bounds the spawn work per
    /// sample to one grain, which is what makes `process` wait-free rather than
    /// merely allocation-free. It never binds at `jitter = 0`, where the
    /// interval is `fs/D ≥ 24` samples at the maximum density and 48 kHz.
    /// [design parameter] default 1 sample, range 1 .. 8.
    static constexpr double kMinOnsetIntervalSamples = 1.0;

    // ── Keyed-draw field ids ──────────────────────────────────────────────
    // Distinct ids keep draws that share a grain index from being perfectly
    // correlated. Values are arbitrary but must never be reused or reordered:
    // changing one changes every render made with a given seed.
    static constexpr std::uint64_t kFieldOnsetJitter = 1;
    static constexpr std::uint64_t kFieldPositionSpray = 2;
    static constexpr std::uint64_t kFieldPitchSpray = 3;
    static constexpr std::uint64_t kFieldPanSpray = 4;

    GranularEngineT() {
        panner_.set_law(PanLaw::EqualPower);
        rebuild_window();
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Sizes the live ring and the window table. The only member that
    /// allocates.
    void prepare(double sample_rate) {
        sample_rate_ = std::isfinite(sample_rate) && sample_rate > 0.0 ? sample_rate
                                                                       : sample_rate_;
        ring_length_ = static_cast<int>(std::ceil(kRingSeconds * sample_rate_));
        ring_.assign(static_cast<std::size_t>(ring_length_), SampleType{0});
        guard_samples_ = derived_guard_samples(sample_rate_);
        level_.set_ramp_time(kControlSmoothingMs * 0.001, sample_rate_);
        mix_.set_ramp_time(kControlSmoothingMs * 0.001, sample_rate_);
        rebuild_window();
        reset();
    }

    /// Clears grains and the ring, rewinds the schedule, and restarts the
    /// random stream. Restarting the stream costs nothing here: the draws are a
    /// pure function of the grain index, so rewinding the index to zero *is*
    /// reseeding. Never allocates.
    void reset() {
        std::fill(ring_.begin(), ring_.end(), SampleType{0});
        reset_runtime_state();
    }

private:
    /// Rewinds every logical owner of ring validity without touching the ring
    /// allocation itself. The grain pool is a compile-time 64-slot bound; all
    /// remaining work is scalar. After `write_head_ = 0`, `place_live_read()`'s
    /// oldest/newest bounds admit only slots overwritten since this rewind, so
    /// stale physical storage is unreachable.
    void audio_fault_reset() {
        reset_runtime_state();
    }

    void reset_runtime_state() {
        for (auto& grain : grains_) grain = Grain{};
        write_head_ = 0;
        sample_clock_ = 0;
        grain_index_ = 0;
        next_onset_ = 0.0;
        steal_count_ = 0;
        clamp_count_ = 0;
        playhead_ = position01_ * static_cast<double>(std::max(buffer_length_, 1));
        live_centre_ = 0.0;
        level_.set_immediate(units::db_to_linear(level_db_));
        mix_.set_immediate(mix_target_);
    }

public:
    // ── Source ────────────────────────────────────────────────────────────

    void set_source(GrainSource source) { source_ = source; }
    GrainSource source() const { return source_; }

    /// Borrows an external interleaved buffer. The engine never owns, copies,
    /// or frees it; `length` is in FRAMES and `channels` is the interleave.
    void set_buffer(const SampleType* data, int length, int channels) {
        buffer_ = data;
        buffer_length_ = std::max(length, 0);
        buffer_channels_ = std::max(channels, 1);
        playhead_ = position01_ * static_cast<double>(std::max(buffer_length_, 1));
    }

    /// Pushes `n` mono samples into the live ring. See the note on `process`'s
    /// interleaved overload for why the two-call form is not block-size
    /// independent.
    void write_live(const SampleType* in, int n) {
        if (ring_length_ <= 0) return;
        // Live freeze holds the captured material, not merely its nominal
        // centre while the ring overwrites it underneath us.
        if (!accepts_live_input()) return;
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(static_cast<double>(in[i]))) {
                audio_fault_reset();
                continue;
            }
            write_one(in[i]);
        }
    }

    /// Playhead advance multiplier. 1 is realtime, 0 freezes, 4 scans four
    /// times as fast. In live-ring mode there is no traversal to scale — the
    /// playhead IS the write head — so `stretch` acts purely as the freeze
    /// switch there: 0 holds the captured centre, anything above 0 tracks.
    void set_stretch(double rate) {
        if (!std::isfinite(rate)) return;
        stretch_ = std::clamp(rate, 0.0, kMaxStretch);
    }
    double stretch() const { return stretch_; }

    /// Playhead centre as a fraction of the source. In buffer mode this jumps
    /// the playhead; in live mode it is how far behind the write head grains
    /// sample, as a fraction of the ring.
    void set_position(double pos01) {
        if (!std::isfinite(pos01)) return;
        position01_ = std::clamp(pos01, 0.0, 1.0);
        if (source_ == GrainSource::buffer) {
            playhead_ = position01_ * static_cast<double>(std::max(buffer_length_, 1));
        }
    }
    double position() const { return position01_; }

    void set_position_spray_ms(double ms) {
        if (!std::isfinite(ms)) return;
        position_spray_ms_ = std::clamp(ms, 0.0, kMaxPositionSprayMs);
    }
    double position_spray_ms() const { return position_spray_ms_; }

    // ── Schedule ──────────────────────────────────────────────────────────

    void set_density_hz(double hz) {
        if (!std::isfinite(hz)) return;
        density_hz_ = std::clamp(hz, kMinDensityHz, kMaxDensityHz);
    }
    double density_hz() const { return density_hz_; }

    void set_grain_ms(double ms) {
        if (!std::isfinite(ms)) return;
        grain_ms_ = std::clamp(ms, kMinGrainMs, kMaxGrainMs);
    }
    double grain_ms() const { return grain_ms_; }

    /// 0 is the exact clock; 1 is a true Poisson process.
    void set_async_jitter(double j01) {
        if (!std::isfinite(j01)) return;
        jitter_ = std::clamp(j01, 0.0, 1.0);
    }
    double async_jitter() const { return jitter_; }

    /// Grains in slots above the new budget stop immediately, so lowering the
    /// budget cannot leave a grain sounding from a slot the pool no longer
    /// scans.
    void set_max_grains(int count) {
        const int clamped = std::clamp(count, kMinGrainBudget, kMaxGrainBudget);
        for (int i = clamped; i < max_grains_; ++i) grains_[static_cast<std::size_t>(i)].active = false;
        max_grains_ = clamped;
    }
    int max_grains() const { return max_grains_; }

    void set_steal_policy(StealPolicy policy) { steal_policy_ = policy; }
    StealPolicy steal_policy() const { return steal_policy_; }

    // ── Per-grain shaping ─────────────────────────────────────────────────

    /// 0 is rectangular, 1 is Hann (cosine mode) or triangle (trapezoid mode);
    /// in between is a Tukey / flat-topped trapezoid whose tapers occupy
    /// `taper/2` of the grain at each end.
    void set_window_taper(double t01) {
        if (!std::isfinite(t01)) return;
        taper_ = std::clamp(t01, 0.0, 1.0);
        rebuild_window();
    }
    double window_taper() const { return taper_; }

    void set_window_trapezoid(bool trapezoid) {
        trapezoid_ = trapezoid;
        rebuild_window();
    }
    bool window_trapezoid() const { return trapezoid_; }

    void set_pitch_semitones(double st) {
        if (!std::isfinite(st)) return;
        pitch_st_ = std::clamp(st, -kMaxPitchSemitones, kMaxPitchSemitones);
    }
    double pitch_semitones() const { return pitch_st_; }

    void set_pitch_spray_semitones(double st) {
        if (!std::isfinite(st)) return;
        pitch_spray_st_ = std::clamp(st, 0.0, kMaxPitchSpraySemitones);
    }

    void set_pan_spray(double s01) {
        if (!std::isfinite(s01)) return;
        pan_spray_ = std::clamp(s01, 0.0, 1.0);
    }

    void set_coherence(Coherence coherence) { coherence_ = coherence; }
    Coherence coherence() const { return coherence_; }

    void set_interp(GrainInterp interp) { interp_ = interp; }
    GrainInterp interp() const { return interp_; }

    void set_level_db(double db) {
        if (!std::isfinite(db)) return;
        level_db_ = std::clamp(db, kMinLevelDb, kMaxLevelDb);
        level_.set_target(units::db_to_linear(level_db_));
    }

    /// Dry/wet, 0 = dry .. 1 = wet. Only the interleaved `process` overload has
    /// a dry signal to blend; the two-output overload is a pure source and
    /// ignores this.
    void set_mix(double mix01) {
        if (!std::isfinite(mix01)) return;
        mix_target_ = std::clamp(mix01, 0.0, 1.0);
        mix_.set_target(mix_target_);
    }
    double mix() const { return mix_target_; }

    /// Fixed at `reset()`; never automatable (series law 2).
    void set_seed(std::uint32_t seed) { seed_ = seed; }
    std::uint32_t seed() const { return seed_; }

    // ── Queries ───────────────────────────────────────────────────────────

    /// Zero. Grains read past samples only; the `position` offset is musical,
    /// not codec latency.
    std::size_t latency_samples() const { return 0; }

    /// Mean simultaneous overlap `N̄ = D·T_g` — the quantity both normalization
    /// laws are written in terms of.
    double mean_overlap() const { return density_hz_ * grain_ms_ * 0.001; }

    /// Per-grain gain the next spawn will use, from the current law and params.
    double grain_gain() const {
        const double overlap = std::max(mean_overlap(), 1e-9);
        const double raw = coherence_ == Coherence::incoherent
                               ? 1.0 / (window_rms_ * std::sqrt(overlap))
                               : 1.0 / (window_mean_ * overlap);
        return std::min(1.0, raw);
    }

    /// Measured from the shipped table, not assumed. At full cosine taper these
    /// are the exact Hann identities 1/2 and √(3/8).
    double window_mean() const { return window_mean_; }
    double window_rms() const { return window_rms_; }

    /// Window value at a normalised grain phase — the same table read the audio
    /// path uses, exposed so a test measures what the engine plays.
    double window_at(double phase) const {
        const double x = std::clamp(phase, 0.0, 1.0) * static_cast<double>(kWindowTableSize);
        int index = static_cast<int>(x);
        if (index >= kWindowTableSize) index = kWindowTableSize - 1;
        const double frac = x - static_cast<double>(index);
        const double a = window_[static_cast<std::size_t>(index)];
        const double b = window_[static_cast<std::size_t>(index) + 1];
        return a + frac * (b - a);
    }

    int active_grain_count() const {
        int count = 0;
        for (int i = 0; i < max_grains_; ++i) {
            if (grains_[static_cast<std::size_t>(i)].active) ++count;
        }
        return count;
    }

    GrainView grain(int slot) const {
        GrainView view;
        if (slot < 0 || slot >= kMaxGrainBudget) return view;
        const Grain& g = grains_[static_cast<std::size_t>(slot)];
        view.active = g.active;
        view.phase = g.phase;
        view.read_position = g.read_position;
        view.ratio = g.ratio;
        view.gain = g.gain;
        view.pan_left = g.pan_left;
        view.pan_right = g.pan_right;
        return view;
    }

    /// Grains born since `reset()`. Monotonic; this is the key every draw uses.
    std::uint64_t grain_index() const { return grain_index_; }

    /// Grains recycled out from under a still-sounding predecessor.
    std::uint64_t steal_count() const { return steal_count_; }

    /// Spawns whose read position the causality clamp had to move.
    std::uint64_t clamp_count() const { return clamp_count_; }

    int ring_length() const { return ring_length_; }

    /// Physical live-ring storage for diagnostics. Logical validity is still
    /// governed by `write_head_`; after an audio fault this may expose stale
    /// storage that the renderer must not read until it has been overwritten.
    SampleType ring_storage_sample(int index) const {
        if (index < 0 || index >= ring_length_) return SampleType{0};
        return ring_[static_cast<std::size_t>(index)];
    }

    /// The derived causality guard, in samples: the worst-case total
    /// displacement a grain's read pointer can reach away from its nominal
    /// centre, which is the maximum position spray plus the longest source span
    /// a grain can consume at the highest pitch-up ratio. Not a free constant.
    int causality_guard_samples() const { return guard_samples_; }

    static int derived_guard_samples(double sample_rate) {
        const double r_max = std::exp2(kMaxPitchSemitones / 12.0);
        return static_cast<int>(
            std::ceil(sample_rate * (kMaxPositionSprayMs * 0.001 + kMaxGrainMs * 0.001 * r_max)));
    }

    // ── Render ────────────────────────────────────────────────────────────

    /// Pure-source render. In live-ring mode the caller must have pushed this
    /// block's input with `write_live` first.
    void process(SampleType* out_left, SampleType* out_right, int n) {
        render(nullptr, out_left, out_right, n);
    }

    /// Live render that consumes its own input, one sample at a time.
    ///
    /// This overload exists because the `write_live` + `process` pair CANNOT be
    /// block-size independent: writing a whole block before rendering any of it
    /// puts samples in the ring that a grain rendered at the start of that block
    /// can legally read, and rendering sample-by-sample does not. Interleaving
    /// the two removes the ambiguity, so this is the overload whose output is
    /// identical at 1×N and N×1 — and it is also the natural shape for a plugin
    /// callback. It is additionally the only overload with a dry signal, so
    /// `set_mix` applies here and nowhere else.
    void process(const SampleType* in, SampleType* out_left, SampleType* out_right, int n) {
        render(in, out_left, out_right, n);
    }

private:
    bool accepts_live_input() const {
        return source_ != GrainSource::live_ring || stretch_ != 0.0;
    }

    struct Grain {
        bool active = false;
        double phase = 0.0;
        double phase_increment = 0.0;
        double read_position = 0.0;
        double ratio = 1.0;
        double gain = 0.0;
        double pan_left = 0.0;
        double pan_right = 0.0;
    };

    static constexpr double kPi = 3.14159265358979323846;

    // ── Window table ──────────────────────────────────────────────────────

    /// Rebuilds the table and re-measures its mean and RMS. The table is
    /// indexed by phase, not by sample, so one table serves every grain length
    /// — which is the reason a table exists at all rather than a closed form
    /// evaluated per sample.
    void rebuild_window() {
        window_.resize(static_cast<std::size_t>(kWindowTableSize) + 1);
        for (int i = 0; i <= kWindowTableSize; ++i) {
            window_[static_cast<std::size_t>(i)] =
                window_shape(static_cast<double>(i) / static_cast<double>(kWindowTableSize));
        }
        // Mean and RMS over the half-open phase domain [0, 1): the last table
        // entry is the wrap guard for interpolation and would double-count the
        // p = 0 point.
        double sum = 0.0;
        double sum_squares = 0.0;
        for (int i = 0; i < kWindowTableSize; ++i) {
            const double w = window_[static_cast<std::size_t>(i)];
            sum += w;
            sum_squares += w * w;
        }
        window_mean_ = sum / static_cast<double>(kWindowTableSize);
        window_rms_ = std::sqrt(sum_squares / static_cast<double>(kWindowTableSize));
    }

    double window_shape(double p) const {
        if (taper_ <= 0.0) return 1.0;
        const double half = 0.5 * taper_;
        const double edge = p < half ? p : (p > 1.0 - half ? 1.0 - p : half);
        if (edge >= half) return 1.0;
        const double t = edge / half;  // 0 at the grain edge, 1 where the flat top starts
        if (trapezoid_) return t;
        // Cosine taper. At taper = 1 the two halves meet at p = 0.5 and this is
        // exactly Hann: 0.5·(1 − cos(2πp)) = sin²(πp).
        return 0.5 * (1.0 - std::cos(kPi * t));
    }

    // ── Schedule ──────────────────────────────────────────────────────────

    /// Inter-onset interval in samples for the grain that follows index `k`.
    ///
    /// `jitter` blends the exact clock into a Poisson process by blending the
    /// INTERVAL, not the grid position. That choice is deliberate: it makes both
    /// endpoints exactly what they are named — `jitter = 0` is the exact clock
    /// and `jitter = 1` is an exponential inter-onset, i.e. a genuine Poisson
    /// process — while a blend of grid POSITION reaches only a triangular
    /// interval distribution and never becomes Poisson at all. The mean
    /// interval is `H` at every jitter, so the long-run onset rate is `D`
    /// regardless.
    double onset_interval_samples(std::uint64_t k) const {
        const double h = sample_rate_ / density_hz_;
        if (jitter_ <= 0.0) return h;
        double u = unit_from<double>(mix64(seed_, k, kFieldOnsetJitter));
        if (u < kMinDrawForLog) u = kMinDrawForLog;
        const double exponential = -std::log(u) * h;
        const double blended = (1.0 - jitter_) * h + jitter_ * exponential;
        return std::max(blended, kMinOnsetIntervalSamples);
    }

    int allocate_slot() {
        for (int i = 0; i < max_grains_; ++i) {
            if (!grains_[static_cast<std::size_t>(i)].active) return i;
        }
        if (steal_policy_ == StealPolicy::drop_new) return -1;

        int best = 0;
        if (steal_policy_ == StealPolicy::oldest) {
            // Furthest through its window. With equal durations that is also the
            // quietest, which is why stealing is click-free.
            double best_phase = -1.0;
            for (int i = 0; i < max_grains_; ++i) {
                const double phase = grains_[static_cast<std::size_t>(i)].phase;
                if (phase > best_phase) {
                    best_phase = phase;
                    best = i;
                }
            }
        } else {
            double best_level = 1e300;
            for (int i = 0; i < max_grains_; ++i) {
                const Grain& g = grains_[static_cast<std::size_t>(i)];
                const double level = window_at(g.phase) * g.gain;
                if (level < best_level) {
                    best_level = level;
                    best = i;
                }
            }
        }
        ++steal_count_;
        return best;
    }

    void spawn(std::uint64_t k) {
        const double u_position = unit_from<double>(mix64(seed_, k, kFieldPositionSpray));
        const double u_pitch = unit_from<double>(mix64(seed_, k, kFieldPitchSpray));
        const double u_pan = unit_from<double>(mix64(seed_, k, kFieldPanSpray));

        const double semitones = pitch_st_ + (u_pitch * 2.0 - 1.0) * pitch_spray_st_;
        Grain next{};
        next.ratio = units::semitones_to_ratio(semitones);

        const double grain_seconds = grain_ms_ * 0.001;
        next.phase = 0.0;
        next.phase_increment = 1.0 / std::max(grain_seconds * sample_rate_, 1.0);
        next.gain = grain_gain();

        const double spray =
            (u_position * 2.0 - 1.0) * units::ms_to_samples(position_spray_ms_, sample_rate_);
        const double requested_read = source_ == GrainSource::buffer ? playhead_ + spray
                                                                     : live_centre_ - spray;
        if (source_ == GrainSource::buffer) {
            next.read_position = requested_read;
        } else if (!place_live_read(requested_read, next.ratio, grain_seconds,
                                    next.read_position)) {
            // Nothing valid to read yet — the ring has not captured enough past.
            // The grain is skipped rather than placed on unwritten samples;
            // `grain_index_` still advances, so no draw shifts and determinism
            // is untouched.
            return;
        }

        // Placement is known-good before a sounding slot can be selected. A
        // failed live birth therefore cannot partially overwrite a victim or
        // count as a steal.
        const int slot = allocate_slot();
        if (slot < 0) return;
        if (next.read_position != requested_read) ++clamp_count_;

        panner_.set_pan(std::clamp((u_pan * 2.0 - 1.0) * pan_spray_, -1.0, 1.0));
        panner_.compute_gains(next.pan_left, next.pan_right);

        next.active = true;
        grains_[static_cast<std::size_t>(slot)] = next;
    }

    /// Places a grain's read pointer so its whole span stays inside the region
    /// of the ring that has actually been written. Returns false when no such
    /// placement exists, which happens only in the first grain-length after
    /// `reset()`, before the ring holds a grain's worth of past.
    ///
    /// Three ends to respect, and the third is the one that is easy to miss:
    ///
    ///   - The read must never outrun the write head. Both advance during the
    ///     grain's life — the head by one sample per output sample — so for
    ///     `ratio > 1` the grain has to start `(ratio − 1)·duration` behind it.
    ///     (This is why the live causality guarantee assumes input keeps
    ///     arriving at the render rate, which is what a live granulator's
    ///     caller does.)
    ///   - The write head must never lap the read from behind. For `ratio < 1`
    ///     the separation grows during the grain, so the start moves forward
    ///     by `(1 − ratio)·duration`. The public derived guard is the
    ///     worst-case capacity calculation (spray plus pitched source span),
    ///     not an additional exclusion zone: applying it here as well would
    ///     count the grain span twice and make legal long, pitched grains
    ///     impossible to place.
    ///   - **The ring is not full at the start.** Only `write_head` samples have
    ///     ever been written, so the oldest valid index is `0`, not
    ///     `write_head − ring_length`. Bounding only relative to the head lets a
    ///     grain read slots that have never held anything, which is silent and
    ///     therefore invisible until a test looks for it.
    bool place_live_read(double start, double ratio, double grain_seconds, double& out) {
        const double duration = grain_seconds * sample_rate_;
        const double head = static_cast<double>(write_head_);
        const double newest = head - std::max(0.0, ratio - 1.0) * duration - 2.0;
        const double oldest =
            std::max(0.0, head - static_cast<double>(ring_length_) +
                              std::max(0.0, 1.0 - ratio) * duration) +
            2.0;
        if (newest < oldest) return false;
        out = std::clamp(start, oldest, newest);
        return true;
    }

    // ── Source reads ──────────────────────────────────────────────────────

    void write_one(SampleType x) {
        ring_[static_cast<std::size_t>(write_head_ % ring_length_)] = x;
        ++write_head_;
    }

    /// Positive modulo — `%` on a negative left operand rounds toward zero and
    /// would index behind the buffer.
    static int wrap_index(std::int64_t index, int length) {
        std::int64_t m = index % length;
        if (m < 0) m += length;
        return static_cast<int>(m);
    }

    double buffer_frame(std::int64_t frame) const {
        if (buffer_ == nullptr || buffer_length_ <= 0) return 0.0;
        const int index = wrap_index(frame, buffer_length_);
        // Multi-channel folds to mono as the channel MEAN, not the sum, so a
        // stereo source does not arrive 6 dB hotter than a mono one and the
        // normalization law stays valid for both.
        double sum = 0.0;
        for (int c = 0; c < buffer_channels_; ++c) {
            const double sample = static_cast<double>(
                buffer_[static_cast<std::size_t>(index) * static_cast<std::size_t>(buffer_channels_) +
                        static_cast<std::size_t>(c)]);
            if (!std::isfinite(sample)) return 0.0;
            sum += sample;
        }
        return sum / static_cast<double>(buffer_channels_);
    }

    double ring_sample(std::int64_t index) const {
        if (ring_length_ <= 0) return 0.0;
        return static_cast<double>(ring_[static_cast<std::size_t>(wrap_index(index, ring_length_))]);
    }

    double read_source(double position) const {
        const double floored = std::floor(position);
        const auto base = static_cast<std::int64_t>(floored);
        const double t = position - floored;

        double y0 = 0.0;
        double y1 = 0.0;
        if (source_ == GrainSource::buffer) {
            y0 = buffer_frame(base);
            y1 = buffer_frame(base + 1);
        } else {
            y0 = ring_sample(base);
            y1 = ring_sample(base + 1);
        }
        if (interp_ == GrainInterp::linear) return Interpolator::linear(t, y0, y1);

        const double ym1 =
            source_ == GrainSource::buffer ? buffer_frame(base - 1) : ring_sample(base - 1);
        const double y2 =
            source_ == GrainSource::buffer ? buffer_frame(base + 2) : ring_sample(base + 2);
        // Catmull-Rom, from the shared interpolator rather than a second copy of
        // the same four coefficients.
        return Interpolator::hermite(t, ym1, y0, y1, y2);
    }

    // ── Render core ───────────────────────────────────────────────────────

    void render(const SampleType* in, SampleType* out_left, SampleType* out_right, int n) {
        const double buffer_span = static_cast<double>(std::max(buffer_length_, 1));
        for (int i = 0; i < n; ++i) {
            const double input_sample = in != nullptr ? static_cast<double>(in[i]) : 0.0;
            if (in != nullptr && !std::isfinite(input_sample)) {
                // Live input is ring history and the interleaved overload also
                // exposes it as dry audio. Reject it before either path sees it,
                // then restart from the documented fresh state.
                audio_fault_reset();
                out_left[i] = SampleType{0};
                out_right[i] = SampleType{0};
                continue;
            }
            if (in != nullptr && accepts_live_input()) {
                write_one(in[i]);
            }

            if (source_ == GrainSource::live_ring && stretch_ > 0.0) {
                live_centre_ = static_cast<double>(write_head_) -
                               position01_ * static_cast<double>(ring_length_);
            }

            // Spawn every grain whose onset has arrived. Driven by the absolute
            // sample clock, so where the block boundaries fall changes nothing.
            while (next_onset_ <= static_cast<double>(sample_clock_)) {
                spawn(grain_index_);
                next_onset_ += onset_interval_samples(grain_index_);
                ++grain_index_;
            }

            double left = 0.0;
            double right = 0.0;
            for (int s = 0; s < max_grains_; ++s) {
                Grain& g = grains_[static_cast<std::size_t>(s)];
                if (!g.active) continue;
                const double y = read_source(g.read_position) * window_at(g.phase) * g.gain;
                left += y * g.pan_left;
                right += y * g.pan_right;
                g.read_position += g.ratio;
                g.phase += g.phase_increment;
                if (g.phase >= 1.0) g.active = false;
            }

            const double level = level_.next();
            const double blend = mix_.next();
            const double dry = input_sample;
            const double wet_weight = in != nullptr ? blend : 1.0;
            const double dry_weight = in != nullptr ? 1.0 - blend : 0.0;

            out_left[i] = static_cast<SampleType>(
                snap_to_zero(left * level * wet_weight + dry * dry_weight));
            out_right[i] = static_cast<SampleType>(
                snap_to_zero(right * level * wet_weight + dry * dry_weight));

            if (source_ == GrainSource::buffer) {
                playhead_ += stretch_;
                if (playhead_ >= buffer_span) playhead_ -= buffer_span;
                if (playhead_ < 0.0) playhead_ += buffer_span;
            }
            ++sample_clock_;
        }
    }

    // ── State ─────────────────────────────────────────────────────────────

    double sample_rate_ = 44100.0;

    GrainSource source_ = GrainSource::buffer;
    const SampleType* buffer_ = nullptr;
    int buffer_length_ = 0;
    int buffer_channels_ = 1;

    std::vector<SampleType> ring_{};
    int ring_length_ = 0;
    int guard_samples_ = 0;
    std::int64_t write_head_ = 0;
    double live_centre_ = 0.0;

    double density_hz_ = kDefaultDensityHz;
    double grain_ms_ = kDefaultGrainMs;
    double jitter_ = kDefaultJitter;
    double stretch_ = kDefaultStretch;
    double position01_ = 0.0;
    double position_spray_ms_ = 0.0;
    double pitch_st_ = 0.0;
    double pitch_spray_st_ = 0.0;
    double pan_spray_ = 0.0;
    double taper_ = 0.5;
    bool trapezoid_ = false;
    double level_db_ = 0.0;
    double mix_target_ = 1.0;

    Coherence coherence_ = Coherence::incoherent;
    StealPolicy steal_policy_ = StealPolicy::oldest;
    GrainInterp interp_ = GrainInterp::cubic_hermite;
    int max_grains_ = kDefaultMaxGrains;
    std::uint32_t seed_ = kDefaultSeed;

    std::vector<double> window_{};
    double window_mean_ = 1.0;
    double window_rms_ = 1.0;

    Grain grains_[kMaxGrainBudget]{};
    double playhead_ = 0.0;
    double next_onset_ = 0.0;
    std::int64_t sample_clock_ = 0;
    std::uint64_t grain_index_ = 0;
    std::uint64_t steal_count_ = 0;
    std::uint64_t clamp_count_ = 0;

    SmoothedValue<double> level_{1.0};
    SmoothedValue<double> mix_{1.0};
    PannerT<double> panner_{};
};

using GranularEngine = GranularEngineT<float>;
using GranularEngine64 = GranularEngineT<double>;

}  // namespace pulp::signal

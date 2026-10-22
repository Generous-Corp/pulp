#pragma once

// The multirate bridge: host rate in, tank rate out, and back.
//
// The tank runs at its OWN sample rate, chosen from eight pinned steps. Low
// rates are darker and grittier by design — the band limit plus the resampler's
// own character — and high rates are airier. Non-integer ratios (44.1 kHz host
// into a 20 kHz tank) are the normal case, never the exception, so nothing here
// may assume an integer decimation factor.
//
// HOW POSITIONS ARE TRACKED
// Each leg keeps the ABSOLUTE position, in its source stream, of the next
// sample it must produce, and advances it by a constant step. That is stronger
// than a wrapped phase accumulator: there is no per-block reset, no rounding
// that can accumulate, and the sequence of produced positions depends only on
// how many samples have been seen — never on how they were divided into blocks.
// Rendering one 4096-sample block and rendering the same audio as 8 x 512 give
// bit-identical output, which is the property the determinism test pins.
//
// WHY THERE IS A LAG
// A 4-point Hermite read at position p needs the samples at floor(p)-1 through
// floor(p)+2, so each leg can only produce a sample once its source stream has
// advanced two samples past it. The output leg's two-sample window is in TANK
// samples, so in host time it is worth 2 * fs_host / fs_tank — six host samples
// at a 16 kHz tank against a 48 kHz host. That is the whole of the engine's
// reported-zero latency story: there is no block-scale buffering anywhere, only
// this interpolation window, and it is ratio-dependent by construction.
//
// BOTH RAILS AT ONCE
// The bridge carries a stereo pair through identical filters at identical
// positions, so it stores the rails interleaved and runs each filter section
// and each Hermite read on a two-lane double vector holding left and right.
// Input is sanitized where it enters each leg; the recursive filter state is
// snapped and sanitized every kHealIntervalSamples of the filter's own clock
// rather than at every multiply-add.
//
// RT contract: prepare() allocates for the worst case (96 kHz tank at the
// maximum block size). configure() re-derives every rate-dependent value and
// allocates nothing, so a live tank-rate change is RT-safe.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/filter_state.hpp>
#include <pulp/signal/fdn/interp.hpp>
#include <pulp/signal/detail/simd_lanes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace pulp::signal::fdn {

namespace lanes = signal::detail::lanes;

// A ring addressed by ABSOLUTE stream position rather than by delay. Positions
// before the start of the stream read as silence, which is what makes the
// startup transient of both legs simply "the tail has not arrived yet".
template <typename SampleType = float>
class PositionRing {
public:
    void prepare(int capacity) {
        std::size_t size = 8;
        while (size < static_cast<std::size_t>(std::max(capacity, 1) + 4)) size <<= 1;
        buffer_.assign(size, SampleType{0});
        mask_ = size - 1;
        written_ = 0;
    }

    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), SampleType{0});
        written_ = 0;
    }

    void push(SampleType x) {
        buffer_[static_cast<std::size_t>(written_) & mask_] = x;
        ++written_;
    }

    std::int64_t written() const { return written_; }

    // Highest position that can be read with all four Hermite taps present.
    double readable_through() const { return static_cast<double>(written_ - 3); }

    double read(double position) const {
        const double floor_pos = std::floor(position);
        const auto i = static_cast<std::int64_t>(floor_pos);
        const double t = position - floor_pos;
        return hermite4(at(i - 1), at(i), at(i + 1), at(i + 2), t);
    }

private:
    double at(std::int64_t index) const {
        if (index < 0 || index >= written_) return 0.0;
        return static_cast<double>(buffer_[static_cast<std::size_t>(index) & mask_]);
    }

    std::vector<SampleType> buffer_;
    std::size_t mask_ = 0;
    std::int64_t written_ = 0;
};

// Two cascaded 2-pole lowpasses with the 4-pole Butterworth Q pair. One
// coefficient design serves both roles (anti-alias before decimation,
// reconstruction after interpolation) because both always run in the FASTER of
// the two domains at the same cutoff; only the state is per role and channel.
class ButterworthPair {
public:
    void design(double cutoff_hz, double sample_rate) {
        BiquadT<double> designer;
        designer.set_coefficients(BiquadT<double>::Type::lowpass, cutoff_hz,
                                  kButterworthQ0, sample_rate);
        section_[0] = designer.coefficients();
        designer.set_coefficients(BiquadT<double>::Type::lowpass, cutoff_hz,
                                  kButterworthQ1, sample_rate);
        section_[1] = designer.coefficients();
    }

    const std::array<BiquadCoefficientsT<double>, 2>& sections() const {
        return section_;
    }

private:
    std::array<BiquadCoefficientsT<double>, 2> section_{};
};

// The state half of the pair: one instance per (role, channel).
class ButterworthState {
public:
    void reset() { s_ = {}; }

    double process(const std::array<BiquadCoefficientsT<double>, 2>& sections,
                   double x) {
        for (int k = 0; k < 2; ++k)
            x = s_[static_cast<std::size_t>(k)].process(
                sections[static_cast<std::size_t>(k)], x);
        return x;
    }

private:
    std::array<SanitizedBiquadState, 2> s_{};
};

// An interleaved stereo ring addressed by absolute stream position, as
// PositionRing is for one rail. Positions before the start of the stream read
// as the silence reset() left there: the bridge only ever reads a few samples
// before zero, while the ring is still far from wrapping.
template <typename SampleType = float> class StereoPositionRing {
  public:
    void prepare(int capacity) {
        std::size_t size = 8;
        while (size < static_cast<std::size_t>(std::max(capacity, 1) + 4))
            size <<= 1;
        buffer_.assign(size * 2, SampleType{0});
        mask_ = size - 1;
        written_ = 0;
    }

    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), SampleType{0});
        written_ = 0;
    }

    void push(lanes::D2 x) {
        const std::size_t slot = (static_cast<std::size_t>(written_) & mask_) * 2;
        if constexpr (std::is_same_v<SampleType, float>) {
            lanes::store_narrow2(&buffer_[slot], x);
        } else {
            buffer_[slot] = static_cast<SampleType>(lanes::lane(x, 0));
            buffer_[slot + 1] = static_cast<SampleType>(lanes::lane(x, 1));
        }
        ++written_;
    }

    std::int64_t written() const {
        return written_;
    }
    double readable_through() const {
        return static_cast<double>(written_ - 3);
    }

    lanes::D2 read(double position) const {
        const double floor_pos = std::floor(position);
        const auto i = static_cast<std::int64_t>(floor_pos);
        const lanes::D2 t = lanes::splat(position - floor_pos);
        const lanes::D2 y0 = at(i - 1);
        const lanes::D2 y1 = at(i);
        const lanes::D2 y2 = at(i + 1);
        const lanes::D2 y3 = at(i + 2);
        // Interpolator::hermite, both rails at once.
        const lanes::D2 c1 = lanes::splat(0.5) * (y2 - y0);
        const lanes::D2 c2 =
            y0 - lanes::splat(2.5) * y1 + lanes::splat(2.0) * y2 - lanes::splat(0.5) * y3;
        const lanes::D2 c3 = lanes::splat(0.5) * (y3 - y0) + lanes::splat(1.5) * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

  private:
    lanes::D2 at(std::int64_t index) const {
        const std::size_t slot = (static_cast<std::size_t>(index) & mask_) * 2;
        if constexpr (std::is_same_v<SampleType, float>)
            return lanes::load_widen2(&buffer_[slot]);
        else
            return lanes::make(static_cast<double>(buffer_[slot]),
                               static_cast<double>(buffer_[slot + 1]));
    }

    std::vector<SampleType> buffer_;
    std::size_t mask_ = 0;
    std::int64_t written_ = 0;
};

// A normalized biquad section rearranged so its recursion is one
// multiply-add deep. Transposed direct form II computes y = b0 x + s1 and then
// s1' = b1 x - a1 y + s2, which puts two dependent multiply-adds between one
// s1 and the next. Substituting y gives
//   s1' = (b1 - a1 b0) x - a1 s1 + s2,   s2' = (b2 - a2 b0) x - a2 s1,
// the same state and the same transfer function, with y now off the
// recursion — half the latency per sample for a filter that cannot be
// vectorized along time.
struct StereoSection {
    double b0 = 1.0, c1 = 0.0, c2 = 0.0, a1 = 0.0, a2 = 0.0;

    static StereoSection from(const BiquadCoefficientsT<double>& c) {
        return {c.b0, c.b1 - c.a1 * c.b0, c.b2 - c.a2 * c.b0, c.a1, c.a2};
    }
};

// One biquad cascade's state for both rails, two lanes per section. The state
// is snapped and sanitized every kHealIntervalSamples samples it processes,
// counted from reset(), so the cleanup never depends on the caller's blocks.
// A StereoSection with every coefficient already broadcast to both lanes,
// built once per block rather than at every sample.
struct StereoSectionLanes {
    lanes::D2 b0, c1, c2, a1, a2;

    static StereoSectionLanes from(const StereoSection& s) {
        return {lanes::splat(s.b0), lanes::splat(s.c1), lanes::splat(s.c2), lanes::splat(s.a1),
                lanes::splat(s.a2)};
    }
};

template <int Sections> class StereoBiquadState {
  public:
    void reset() {
        for (auto& s : s1_)
            s = lanes::splat(0.0);
        for (auto& s : s2_)
            s = lanes::splat(0.0);
        count_ = 0;
    }

    lanes::D2 process(const StereoSection* sections, lanes::D2 x) {
        StereoSectionLanes l[Sections];
        for (int k = 0; k < Sections; ++k) l[k] = StereoSectionLanes::from(sections[k]);
        return process(l, x);
    }

    lanes::D2 process(const StereoSectionLanes* sections, lanes::D2 x) {
        for (int k = 0; k < Sections; ++k) {
            const auto& c = sections[k];
            const lanes::D2 s1 = s1_[k];
            const lanes::D2 y = c.b0 * x + s1;
            s1_[k] = c.c1 * x + s2_[k] - c.a1 * s1;
            s2_[k] = c.c2 * x - c.a2 * s1;
            x = y;
        }
        if (++count_ == kHealIntervalSamples) {
            heal();
            count_ = 0;
        }
        return x;
    }

    // Snap decayed state to zero and clear state that went non-finite.
    void heal() {
        for (int k = 0; k < Sections; ++k) {
            s1_[k] = healed(s1_[k]);
            s2_[k] = healed(s2_[k]);
        }
    }

  private:
    static lanes::D2 healed(lanes::D2 v) {
        const double a = lanes::lane(v, 0);
        const double b = lanes::lane(v, 1);
        return lanes::make(std::isfinite(a) ? snap_to_zero(a) : 0.0,
                           std::isfinite(b) ? snap_to_zero(b) : 0.0);
    }

  public:
  private:
    lanes::D2 s1_[Sections] = {};
    lanes::D2 s2_[Sections] = {};
    int count_ = 0;
};

// Host <-> tank stereo bridge.
template <typename SampleType = float>
class MultirateBridge {
public:
    static constexpr int kNumRails = 2;

    void prepare(double host_rate, int max_block) {
        host_rate_ = host_rate > 0.0 ? host_rate : 48000.0;
        max_block_ = std::max(max_block, 1);
        max_tank_samples_ =
            static_cast<int>(std::ceil(static_cast<double>(max_block_) * kMaxTankRate /
                                       host_rate_)) +
            kHermiteGuard;
        in_ring_.prepare(max_block_ * 2 + kHermiteGuard);
        out_ring_.prepare(max_tank_samples_ * 2 + kHermiteGuard);
        for (int c = 0; c < kNumRails; ++c) {
            tank_in_[c].assign(static_cast<std::size_t>(max_tank_samples_), SampleType{0});
            tank_out_[c].assign(static_cast<std::size_t>(max_tank_samples_), SampleType{0});
        }
        configure(kTankRates[1]);
    }

    // Re-derive every rate-dependent value. Never allocates: a live tank-rate
    // change is a hard, clean flush, not a re-prepare.
    void configure(double tank_rate) {
        tank_rate_ = tank_rate;
        resampling_ = std::abs(tank_rate_ - host_rate_) > 1e-6;
        host_per_tank_ = host_rate_ / tank_rate_;
        tank_per_host_ = tank_rate_ / host_rate_;
        if (resampling_) {
            const double fast = std::max(host_rate_, tank_rate_);
            const double slow = std::min(host_rate_, tank_rate_);
            filter_.design(kAaCutoffFraction * slow, fast);
            for (int k = 0; k < 2; ++k)
                sections_[k] = StereoSection::from(filter_.sections()[static_cast<std::size_t>(k)]);
        }
        reset();
    }

    double tank_rate() const { return tank_rate_; }
    bool resampling() const { return resampling_; }

    // Full flush: lines, filter states, and every position accumulator. A
    // tank-rate change drops the running tail rather than reinterpreting it at
    // the new rate, so no pitch-shifted tail artifact is possible.
    void reset() {
        in_ring_.reset();
        out_ring_.reset();
        aa_in_.reset();
        aa_out_.reset();
        for (int c = 0; c < kNumRails; ++c) {
            std::fill(tank_in_[c].begin(), tank_in_[c].end(), SampleType{0});
            std::fill(tank_out_[c].begin(), tank_out_[c].end(), SampleType{0});
        }
        next_tank_pos_ = 0.0;
        // Start the output leg behind the tank stream by enough that its
        // Hermite window is satisfied for every block size: three tank samples
        // for its own taps, plus the input leg's three-host-sample window
        // expressed in tank samples, plus a sample of slack for the floor() at
        // the boundary.
        next_host_pos_ = -(4.0 * tank_per_host_ + 4.0);
    }

    // Upper bound on the interpolation skew, in host samples, between an input
    // impulse and the wet response it produces. The engine still reports zero
    // host-compensated latency: this is a 4-point interpolation window, not
    // bufferable delay, and the output leg's window is in TANK samples — which
    // is why the bound grows as the tank rate falls below the host rate.
    double interpolation_skew_samples() const {
        if (!resampling_) return 0.0;
        return 3.0 + (4.0 * tank_per_host_ + 4.0) * host_per_tank_;
    }

    SampleType* tank_input(int rail) { return tank_in_[rail].data(); }
    SampleType* tank_output(int rail) { return tank_out_[rail].data(); }

    // Leg 1: consume a host block, fill tank_input(), return the tank sample
    // count. Filtering happens in the faster domain: at the host rate before
    // decimation when downsampling, at the tank rate after interpolation when
    // upsampling.
    int host_to_tank(const SampleType* left, const SampleType* right, int n) {
        SampleType* tl = tank_in_[0].data();
        SampleType* tr = tank_in_[1].data();
        if (!resampling_) {
            for (int i = 0; i < n; ++i) {
                tl[i] = finite_or_zero(left[i]);
                tr[i] = finite_or_zero(right[i]);
            }
            return n;
        }

        const bool downsampling = tank_rate_ < host_rate_;
        const StereoSectionLanes sections[2] = {StereoSectionLanes::from(sections_[0]),
                                                StereoSectionLanes::from(sections_[1])};
        int produced = 0;
        // The filter state as a local for the loop: the stores to the tank
        // buffers would otherwise force it through memory every sample.
        StereoBiquadState<2> aa = aa_in_;
        for (int i = 0; i < n; ++i) {
            const lanes::D2 x = lanes::make(static_cast<double>(finite_or_zero(left[i])),
                                            static_cast<double>(finite_or_zero(right[i])));
            in_ring_.push(downsampling ? aa.process(sections, x) : x);
            const double limit = in_ring_.readable_through();
            while (next_tank_pos_ <= limit && produced < max_tank_samples_) {
                lanes::D2 v = in_ring_.read(next_tank_pos_);
                if (!downsampling)
                    v = aa.process(sections, v);
                tl[produced] = static_cast<SampleType>(lanes::lane(v, 0));
                tr[produced] = static_cast<SampleType>(lanes::lane(v, 1));
                ++produced;
                next_tank_pos_ += host_per_tank_;
            }
        }
        aa_in_ = aa;
        return produced;
    }

    // Leg 2: consume the tank's output stream and emit exactly host_n host
    // samples. The lag chosen in reset() guarantees every position read below
    // is already inside the tank stream's Hermite window.
    void tank_to_host(int tank_n, SampleType* left, SampleType* right, int host_n) {
        const SampleType* tl = tank_out_[0].data();
        const SampleType* tr = tank_out_[1].data();
        if (!resampling_) {
            for (int i = 0; i < tank_n; ++i) {
                left[i] = finite_or_zero(tl[i]);
                right[i] = finite_or_zero(tr[i]);
            }
            return;
        }
        const bool downsampling = tank_rate_ < host_rate_;
        const StereoSectionLanes sections[2] = {StereoSectionLanes::from(sections_[0]),
                                                StereoSectionLanes::from(sections_[1])};
        StereoBiquadState<2> aa = aa_out_;
        for (int i = 0; i < tank_n; ++i) {
            const lanes::D2 v = lanes::make(static_cast<double>(finite_or_zero(tl[i])),
                                            static_cast<double>(finite_or_zero(tr[i])));
            out_ring_.push(downsampling ? v : aa.process(sections, v));
        }
        double position = next_host_pos_;
        for (int i = 0; i < host_n; ++i) {
            lanes::D2 v = out_ring_.read(position);
            if (downsampling)
                v = aa.process(sections, v);
            left[i] = static_cast<SampleType>(lanes::lane(v, 0));
            right[i] = static_cast<SampleType>(lanes::lane(v, 1));
            position += tank_per_host_;
        }
        next_host_pos_ = position;
        aa_out_ = aa;
    }

  private:
    StereoPositionRing<SampleType> in_ring_{};
    StereoPositionRing<SampleType> out_ring_{};
    std::array<std::vector<SampleType>, kNumRails> tank_in_{};
    std::array<std::vector<SampleType>, kNumRails> tank_out_{};
    StereoBiquadState<2> aa_in_{};
    StereoBiquadState<2> aa_out_{};
    ButterworthPair filter_{};
    StereoSection sections_[2] = {};

    double host_rate_ = 48000.0;
    double tank_rate_ = 20000.0;
    double host_per_tank_ = 1.0;
    double tank_per_host_ = 1.0;
    double next_tank_pos_ = 0.0;
    double next_host_pos_ = 0.0;
    int max_block_ = 0;
    int max_tank_samples_ = 0;
    bool resampling_ = false;
};

}  // namespace pulp::signal::fdn

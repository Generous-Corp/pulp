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
// RT contract: prepare() allocates for the worst case (96 kHz tank at the
// maximum block size). configure() re-derives every rate-dependent value and
// allocates nothing, so a live tank-rate change is RT-safe.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/interp.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pulp::signal::fdn {

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
        for (int k = 0; k < 2; ++k) {
            const auto& c = sections[static_cast<std::size_t>(k)];
            const double out = c.b0 * x + s_[static_cast<std::size_t>(k)].s1;
            s_[static_cast<std::size_t>(k)].s1 =
                c.b1 * x - c.a1 * out + s_[static_cast<std::size_t>(k)].s2;
            s_[static_cast<std::size_t>(k)].s2 = c.b2 * x - c.a2 * out;
            x = out;
        }
        return x;
    }

private:
    struct Section {
        double s1 = 0.0;
        double s2 = 0.0;
    };
    std::array<Section, 2> s_{};
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
        for (int c = 0; c < kNumRails; ++c) {
            in_ring_[c].prepare(max_block_ * 2 + kHermiteGuard);
            out_ring_[c].prepare(max_tank_samples_ * 2 + kHermiteGuard);
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
        }
        reset();
    }

    double tank_rate() const { return tank_rate_; }
    bool resampling() const { return resampling_; }

    // Full flush: lines, filter states, and every position accumulator. A
    // tank-rate change drops the running tail rather than reinterpreting it at
    // the new rate, so no pitch-shifted tail artifact is possible.
    void reset() {
        for (int c = 0; c < kNumRails; ++c) {
            in_ring_[c].reset();
            out_ring_[c].reset();
            aa_in_[c].reset();
            aa_out_[c].reset();
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
        if (!resampling_) {
            for (int i = 0; i < n; ++i) {
                tank_in_[0][static_cast<std::size_t>(i)] = left[i];
                tank_in_[1][static_cast<std::size_t>(i)] = right[i];
            }
            return n;
        }

        const bool downsampling = tank_rate_ < host_rate_;
        int produced = 0;
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < kNumRails; ++c) {
                const double x = static_cast<double>(c == 0 ? left[i] : right[i]);
                in_ring_[c].push(static_cast<SampleType>(
                    downsampling ? aa_in_[c].process(filter_.sections(), x) : x));
            }
            const double limit = in_ring_[0].readable_through();
            while (next_tank_pos_ <= limit && produced < max_tank_samples_) {
                for (int c = 0; c < kNumRails; ++c) {
                    double v = in_ring_[c].read(next_tank_pos_);
                    if (!downsampling) v = aa_in_[c].process(filter_.sections(), v);
                    tank_in_[c][static_cast<std::size_t>(produced)] =
                        static_cast<SampleType>(v);
                }
                ++produced;
                next_tank_pos_ += host_per_tank_;
            }
        }
        return produced;
    }

    // Leg 2: consume the tank's output stream and emit exactly host_n host
    // samples. The lag chosen in reset() guarantees every position read below
    // is already inside the tank stream's Hermite window.
    void tank_to_host(int tank_n, SampleType* left, SampleType* right, int host_n) {
        if (!resampling_) {
            for (int i = 0; i < tank_n; ++i) {
                left[i] = tank_out_[0][static_cast<std::size_t>(i)];
                right[i] = tank_out_[1][static_cast<std::size_t>(i)];
            }
            return;
        }
        const bool downsampling = tank_rate_ < host_rate_;
        for (int i = 0; i < tank_n; ++i) {
            for (int c = 0; c < kNumRails; ++c) {
                double v = static_cast<double>(tank_out_[c][static_cast<std::size_t>(i)]);
                if (!downsampling) v = aa_out_[c].process(filter_.sections(), v);
                out_ring_[c].push(static_cast<SampleType>(v));
            }
        }
        for (int i = 0; i < host_n; ++i) {
            for (int c = 0; c < kNumRails; ++c) {
                double v = out_ring_[c].read(next_host_pos_);
                if (downsampling) v = aa_out_[c].process(filter_.sections(), v);
                (c == 0 ? left : right)[i] = static_cast<SampleType>(v);
            }
            next_host_pos_ += tank_per_host_;
        }
    }

private:
    std::array<PositionRing<SampleType>, kNumRails> in_ring_{};
    std::array<PositionRing<SampleType>, kNumRails> out_ring_{};
    std::array<std::vector<SampleType>, kNumRails> tank_in_{};
    std::array<std::vector<SampleType>, kNumRails> tank_out_{};
    std::array<ButterworthState, kNumRails> aa_in_{};
    std::array<ButterworthState, kNumRails> aa_out_{};
    ButterworthPair filter_{};

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

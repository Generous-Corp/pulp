#pragma once

/// @file multi_channel_meter.hpp
/// Multi-channel metering: sample peak, RMS, K-weighted momentary and gated
/// integrated loudness,
/// stereo correlation, clip detection. All computations are lock-free and
/// suitable for the audio thread.
///
/// RT contract: `prepare()` initializes fixed-size accumulator state and may be
/// called off the audio thread. After preparation, `process()`, `snapshot()`,
/// and `reset()` allocate no memory for channel counts up to
/// kMaxMeterChannels. `MultiChannelBallistics::update()` and `clear_clips()`
/// also use fixed storage and allocate no memory.

#include <pulp/signal/detail/simd_lanes.hpp>

#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::signal {

namespace detail {
struct MultiChannelMeterTestAccess;
}

/// Maximum supported channel count for metering.
static constexpr int kMaxMeterChannels = 16;

/// Finite samples outside this deliberately generous audio-domain ceiling are
/// clipped before every peak, RMS, correlation, and loudness calculation.
/// This keeps double-input arithmetic deterministic instead of allowing a
/// finite but non-audio value to overflow a squared-energy accumulator.
static constexpr double kMaxMeterInputMagnitude = 1.0e6;

/// What a MultiChannelMeter computes. Sample peak, RMS, and clip detection are
/// always on; stereo correlation and BS.1770 loudness (K-weighted momentary,
/// integrated, short-term, and loudness range) can be left out when nothing
/// reads them, which removes their per-sample cost. Combine with `|`.
enum class MeterFeatures : std::uint8_t {
    levels = 0,
    correlation = 1u << 0,
    loudness = 1u << 1,
    all = correlation | loudness,
};

constexpr MeterFeatures operator|(MeterFeatures a, MeterFeatures b) noexcept {
    return static_cast<MeterFeatures>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr bool has_feature(MeterFeatures set, MeterFeatures feature) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(feature)) != 0;
}

/// Speaker roles used by the BS.1770 channel summation. LFE is excluded;
/// lateral surrounds receive 1.41 while rear channels at +/-135 degrees use
/// 1.0 as specified for advanced channel layouts.
enum class LoudnessChannelRole : std::uint8_t {
    left,
    right,
    center,
    lfe,
    left_surround,
    right_surround,
    left_rear_surround,
    right_rear_surround,
    unknown
};

struct LoudnessBiquadCoefficients {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

struct KWeightingCoefficients {
    LoudnessBiquadCoefficients shelf;
    LoudnessBiquadCoefficients high_pass;
    bool valid = false;
};

/// Return the two-stage K-weighting design for a sample rate. The design is
/// the bilinear-frequency-mapped form of the BS.1770-5 48 kHz reference
/// filters; rates whose Nyquist frequency cannot represent the shelf return
/// `valid == false`.
inline KWeightingCoefficients k_weighting_coefficients(double sample_rate) {
    if (!std::isfinite(sample_rate) || sample_rate <= 2.0 * 1681.974450955533)
        return {};

    constexpr double shelf_f0 = 1681.974450955533;
    constexpr double shelf_gain_db = 3.999843853973347;
    constexpr double shelf_q = 0.7071752369554196;
    const double shelf_k = std::tan(std::numbers::pi * shelf_f0 / sample_rate);
    const double vh = std::pow(10.0, shelf_gain_db / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double shelf_a0 = 1.0 + shelf_k / shelf_q + shelf_k * shelf_k;

    constexpr double high_pass_f0 = 38.13547087602444;
    constexpr double high_pass_q = 0.5003270373238773;
    const double high_pass_k = std::tan(std::numbers::pi * high_pass_f0 / sample_rate);
    const double high_pass_a0 = 1.0 + high_pass_k / high_pass_q
                              + high_pass_k * high_pass_k;

    return {
        {(vh + vb * shelf_k / shelf_q + shelf_k * shelf_k) / shelf_a0,
         2.0 * (shelf_k * shelf_k - vh) / shelf_a0,
         (vh - vb * shelf_k / shelf_q + shelf_k * shelf_k) / shelf_a0,
         2.0 * (shelf_k * shelf_k - 1.0) / shelf_a0,
         (1.0 - shelf_k / shelf_q + shelf_k * shelf_k) / shelf_a0},
        {1.0, -2.0, 1.0,
         2.0 * (high_pass_k * high_pass_k - 1.0) / high_pass_a0,
         (1.0 - high_pass_k / high_pass_q + high_pass_k * high_pass_k) / high_pass_a0},
        true};
}

/// Per-channel level measurements computed on the audio thread.
struct ChannelLevels {
    float peak = 0.0f;           // Sample peak (linear, 0–1+)
    float rms = 0.0f;            // RMS (linear, 0–1+)
    float lufs_momentary = -std::numeric_limits<float>::infinity(); // This channel's weighted 400 ms contribution
    bool clipped = false;        // True if any sample >= 1.0
};

/// Complete metering snapshot for all channels, published lock-free.
struct MultiChannelMeterData {
    std::array<ChannelLevels, kMaxMeterChannels> channels{};
    int num_channels = 0;
    float correlation = 0.0f;    // Stereo correlation (-1 to +1), valid when num_channels >= 2
    float lufs_integrated = -std::numeric_limits<float>::infinity(); // BS.1770 gated, start to current block
    // Appended after the legacy aggregate fields so positional aggregate
    // initialization continues to bind its fourth value to lufs_integrated.
    float lufs_momentary = -std::numeric_limits<float>::infinity(); // BS.1770 K-weighted, 400 ms
    float short_term_lufs = -std::numeric_limits<float>::infinity(); // EBU 3 s, 100 ms hop
    bool short_term_valid = false;
    float loudness_range_lu = 0.0f;
    bool loudness_range_valid = false;
};

/// Configurable ballistics for multi-channel meter display (UI thread).
struct MultiChannelBallistics {
    struct Channel {
        float display_peak = 0.0f;
        float display_rms = 0.0f;
        float held_peak = 0.0f;
        float hold_counter = 0.0f;
        bool clip_indicator = false;
        float clip_hold_counter = 0.0f;
    };

    std::array<Channel, kMaxMeterChannels> channels{};
    int num_channels = 0;

    float attack_time = 0.001f;     // seconds
    float release_time = 0.3f;      // seconds
    float peak_hold_time = 1.5f;    // seconds
    float clip_hold_time = 3.0f;    // seconds

    /// Update ballistics from new meter data. Call once per UI frame.
    ///
    /// Returns true when a displayed value actually moved. A UI thread polls
    /// this every frame for as long as it is bound, silence included, so the
    /// return value is what lets a still meter ask for nothing: repaint on true,
    /// and a settled meter costs zero composites instead of one per frame
    /// forever. Only the fields that reach the screen count — the hold counters
    /// tick down invisibly. `MeterBallistics::update` reports movement the same
    /// way. If you are only advancing the ballistics and will never paint, say
    /// so with `(void)`.
    [[nodiscard]] bool update(const MultiChannelMeterData& data, float dt) {
        const int requested = std::clamp(data.num_channels, 0, kMaxMeterChannels);
        bool changed = requested != num_channels;
        num_channels = requested;

        float attack_coeff = 1.0f - std::exp(-dt / attack_time);
        float release_coeff = 1.0f - std::exp(-dt / release_time);

        for (int ch = 0; ch < num_channels; ++ch) {
            auto& b = channels[ch];
            auto& d = data.channels[ch];
            const float prev_peak = b.display_peak;
            const float prev_rms = b.display_rms;
            const float prev_held = b.held_peak;
            const bool prev_clip = b.clip_indicator;

            // ONE NON-FINITE SAMPLE WOULD DISABLE THE GATE PERMANENTLY. A NaN
            // takes the release branch, poisons the display value, survives the
            // snaps below (every comparison against NaN is false), and then
            // compares unequal to itself forever — so `changed` would be true on
            // every frame for the rest of the process. Sanitise the input, and
            // self-heal state that arrived non-finite by another route.
            const float in_peak = std::isfinite(d.peak) ? d.peak : 0.0f;
            const float in_rms = std::isfinite(d.rms) ? d.rms : 0.0f;
            if (!std::isfinite(b.display_peak)) b.display_peak = 0.0f;
            if (!std::isfinite(b.display_rms)) b.display_rms = 0.0f;
            if (!std::isfinite(b.held_peak)) b.held_peak = 0.0f;

            // Peak
            if (in_peak > b.display_peak)
                b.display_peak += (in_peak - b.display_peak) * attack_coeff;
            else
                b.display_peak += (in_peak - b.display_peak) * release_coeff;

            // RMS
            if (in_rms > b.display_rms)
                b.display_rms += (in_rms - b.display_rms) * attack_coeff;
            else
                b.display_rms += (in_rms - b.display_rms) * release_coeff;

            // Peak hold
            if (in_peak >= b.held_peak) {
                b.held_peak = in_peak;
                b.hold_counter = peak_hold_time;
            } else {
                // Floor the counters at zero. Left to run they decrement without
                // bound for as long as the meter is polled, which is forever.
                b.hold_counter = std::max(0.0f, b.hold_counter - dt);
                if (b.hold_counter <= 0)
                    b.held_peak += (0.0f - b.held_peak) * release_coeff;
            }

            // Clip indicator
            if (d.clipped) {
                b.clip_indicator = true;
                b.clip_hold_counter = clip_hold_time;
            } else {
                b.clip_hold_counter = std::max(0.0f, b.clip_hold_counter - dt);
                if (b.clip_hold_counter <= 0)
                    b.clip_indicator = false;
            }

            // Clamp noise floor. The decays are exponential, so each of these
            // approaches zero without ever arriving: unsnapped they change by a
            // fraction of a bit every frame FOREVER, which is invisible on
            // screen but means the meter never reports a still frame.
            //
            // These three rest at ZERO, so a floor test is enough. A smoother
            // whose resting value is NOT zero needs the general form —
            // `if (std::abs(v - target) < eps) v = target` — because a floor
            // test never fires for it. Copying these three lines to such a
            // smoother silently does nothing; see CorrelationMeter::update.
            if (b.display_peak < 1e-6f) b.display_peak = 0;
            if (b.display_rms < 1e-6f) b.display_rms = 0;
            if (b.held_peak < 1e-6f) b.held_peak = 0;

            changed = changed || b.display_peak != prev_peak ||
                      b.display_rms != prev_rms || b.held_peak != prev_held ||
                      b.clip_indicator != prev_clip;
        }
        return changed;
    }

    /// Reset all clip indicators immediately.
    void clear_clips() {
        for (auto& ch : channels) {
            ch.clip_indicator = false;
            ch.clip_hold_counter = 0;
        }
    }
};

/// Audio-thread metering processor. Computes sample peak, RMS, stereo
/// correlation, clip detection, and the channel-based BS.1770-5 loudness core
/// for up to kMaxMeterChannels. Loudness uses K-weighting, 400 ms blocks at
/// 75% overlap, and the -70 LUFS absolute / -10 LU relative integrated gates.
/// Finite inputs beyond +/-kMaxMeterInputMagnitude are treated as deterministic
/// over-range audio and clipped to that supported-domain boundary.
///
/// The default layouts are mono, stereo, L/R/C, quad, 5.0, 5.1, and 7.1 in
/// L/R/C/LFE/Ls/Rs/Lrs/Rrs order. There is no implicit seven-channel mapping
/// or default above eight channels; pass explicit roles for those cases. Roles
/// may repeat when multiple input channels share the same BS.1770 weighting.
/// Roles outside this enum are unsupported. `loudness_supported()` reports
/// whether the prepared sample rate and layout have an honest BS.1770
/// interpretation; peak, RMS, correlation, and clip metering remain available
/// when they do not.
/// This supplies the EBU short-term (3 s) and loudness-range core. Scale/UI
/// behavior and true peak remain outside this processor.
///
/// process() works a block at a time: it splits the block at loudness-hop
/// boundaries, measures each piece's peak, energy, and correlation with vector
/// reductions, and runs the K-weighting recursion for channel pairs as one
/// two-lane vector. A piece holding any non-finite or over-range sample is
/// measured sample by sample with the sanitizing rules above instead, so the
/// readings follow the same definitions either way; only the summation order
/// of the energy accumulators differs, at double-precision rounding.
///
/// Call process() from the audio callback. Read results via snapshot().
template <typename SampleType = float>
class MultiChannelMeterT {
public:
    void prepare(double sample_rate, int num_channels) {
        prepare(sample_rate, num_channels, nullptr);
    }

    /// Prepare with a feature set. Without MeterFeatures::loudness the
    /// loudness fields stay at their empty values and loudness_supported()
    /// reports false; without MeterFeatures::correlation the correlation reads
    /// 0. Peak, RMS, and clip detection are always measured.
    void prepare(double sample_rate, int num_channels, const LoudnessChannelRole* roles,
                 MeterFeatures features) {
        features_ = features;
        prepare(sample_rate, num_channels, roles);
    }

    /// The feature set the meter was prepared with.
    MeterFeatures features() const noexcept {
        return features_;
    }

    /// Prepare the loudness meter with an explicit speaker layout. Integrated
    /// gating uses a prepared 0.01 LU Fenwick histogram: memory and update
    /// cost remain fixed regardless of measurement duration. Allocation occurs
    /// only here.
    void prepare(double sample_rate, int num_channels,
                 const LoudnessChannelRole* roles) {
        constexpr double maximum_safe_sample_rate =
            static_cast<double>(std::numeric_limits<int>::max());
        const bool sample_rate_supported = std::isfinite(sample_rate)
                                        && sample_rate > 0.0
                                        && sample_rate <= maximum_safe_sample_rate;
        sample_rate_ = sample_rate_supported ? sample_rate : 0.0;
        const bool channel_count_supported = num_channels > 0
                                          && num_channels <= kMaxMeterChannels;
        num_channels_ = std::clamp(num_channels, 0, kMaxMeterChannels);
        loudness_valid_ = has_feature(features_, MeterFeatures::loudness) &&
                          sample_rate_supported && channel_count_supported &&
                          configure_channel_roles(roles);
        loudness_hop_samples_ = loudness_valid_
            ? std::max(1, static_cast<int>(std::llround(sample_rate_ * 0.1))) : 0;

        configure_k_weighting();
        gate_energy_tree_.assign(kGateBinCount + 1, 0.0);
        gate_count_tree_.assign(kGateBinCount + 1, 0);
        gate_node_epoch_.assign(kGateBinCount + 1, 0);
        lra_count_tree_.assign(kGateBinCount + 1, 0);
        lra_node_epoch_.assign(kGateBinCount + 1, 0);
        gate_epoch_ = 0;
        gate_epoch_active_ = true;

        reset_measurement_state(num_channels_);
    }

    /// Process a block of interleaved or deinterleaved audio.
    /// channels: array of channel pointers. num_samples: samples per channel.
    void process(const SampleType* const* channels, int num_channels, int num_samples) {
        const int requested_num_channels = num_channels;
        num_channels = std::clamp(num_channels, 0, num_channels_);
        if (channels == nullptr || num_samples <= 0) return;

        for (int ch = 0; ch < num_channels; ++ch) {
            if (channels[ch] == nullptr) {
                num_channels = ch;
                break;
            }
        }

        const bool process_loudness = loudness_valid_
                                   && requested_num_channels == num_channels_
                                   && num_channels == num_channels_;
        // A topology or loudness-eligibility transition starts a new
        // measurement programme. The raw requested count participates in the
        // eligibility check before safe level-meter clamping, so an oversized
        // layout cannot masquerade as the prepared layout.
        if (num_channels != active_num_channels_
            || process_loudness != active_loudness_processing_)
            reset_measurement_state(num_channels, process_loudness);

        const bool correlate =
            num_channels >= 2 && has_feature(features_, MeterFeatures::correlation);
        int offset = 0;
        while (offset < num_samples) {
            int piece = num_samples - offset;
            if (process_loudness)
                piece = std::min(piece, loudness_hop_samples_ - loudness_hop_position_);
            if (!measure_piece(channels, num_channels, offset, piece, correlate, process_loudness))
                measure_piece_sanitized(channels, num_channels, offset, piece, correlate,
                                        process_loudness);
            offset += piece;
            block_samples_ += piece;
            if (correlate)
                correlation_samples_ += piece;
            if (process_loudness) {
                loudness_hop_position_ += piece;
                if (loudness_hop_position_ == loudness_hop_samples_)
                    finish_loudness_hop(num_channels);
            }
        }

        // Emit snapshot when we have enough samples for a meaningful measurement
        // Use ~10ms blocks for responsive metering
        int block_size = static_cast<int>(sample_rate_ * 0.01);
        if (block_size < 1) block_size = 1;

        if (block_samples_ >= block_size) {
            emit_snapshot(num_channels);
        }
    }

    /// Get the latest metering snapshot.
    const MultiChannelMeterData& snapshot() const { return snapshot_; }

    /// Whether the prepared sample rate and speaker layout support BS.1770
    /// loudness. False does not disable the non-loudness meter fields.
    bool loudness_supported() const noexcept { return loudness_valid_; }

    void reset() {
        reset_measurement_state(0);
    }

private:
    friend struct detail::MultiChannelMeterTestAccess;

    using D2 = detail::lanes::D2;
    using F4 = detail::lanes::F4;

    // A normalized K-weighting section.
    struct Section {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

        static Section from(const LoudnessBiquadCoefficients& c) {
            return {c.b0, c.b1, c.b2, c.a1, c.a2};
        }
    };

    // One piece's level readings for one channel.
    struct PieceLevels {
        double peak = 0.0;
        double energy = 0.0;
    };

    // Vector pass over one channel's piece: peak, sum of squares (in double),
    // and whether every sample is finite and inside the supported magnitude.
    // Returns false at the first piece that is not, leaving the caller to
    // measure it sample by sample.
    static bool measure_levels(const SampleType* x, int n, PieceLevels& out) {
        namespace lanes = detail::lanes;
        int k = 0;
        double peak = 0.0;
        double energy = 0.0;
        if constexpr (std::is_same_v<SampleType, float>) {
            const F4 limit = lanes::splat(static_cast<float>(kMaxMeterInputMagnitude));
            F4 peak4 = lanes::splat(0.0f);
            auto in_range = lanes::less_equal(peak4, limit);
            D2 e0 = lanes::splat(0.0), e1 = lanes::splat(0.0);
            for (; k + 4 <= n; k += 4) {
                const F4 v = lanes::load(x + k);
                const F4 a = lanes::abs(v);
                in_range = lanes::both(in_range, lanes::less_equal(a, limit));
                peak4 = lanes::max(peak4, a);
                const D2 lo = lanes::widen_low(v);
                const D2 hi = lanes::widen_high(v);
                e0 = e0 + lo * lo;
                e1 = e1 + hi * hi;
            }
            if (!lanes::all(in_range))
                return false;
            for (int lane = 0; lane < 4; ++lane)
                peak = std::max(peak, static_cast<double>(lanes::lane(peak4, lane)));
            energy = (lanes::lane(e0, 0) + lanes::lane(e1, 0)) +
                     (lanes::lane(e0, 1) + lanes::lane(e1, 1));
        }
        for (; k < n; ++k) {
            const double v = static_cast<double>(x[k]);
            const double a = std::abs(v);
            if (!(a <= kMaxMeterInputMagnitude))
                return false;
            peak = std::max(peak, a);
            energy += v * v;
        }
        out.peak = peak;
        out.energy = energy;
        return true;
    }

    // Sum of x[k] * y[k] in double, for a piece already known to be clean.
    static double cross_energy(const SampleType* x, const SampleType* y, int n) {
        namespace lanes = detail::lanes;
        int k = 0;
        double sum = 0.0;
        if constexpr (std::is_same_v<SampleType, float>) {
            D2 s0 = lanes::splat(0.0), s1 = lanes::splat(0.0);
            for (; k + 4 <= n; k += 4) {
                const F4 a = lanes::load(x + k);
                const F4 b = lanes::load(y + k);
                s0 = s0 + lanes::widen_low(a) * lanes::widen_low(b);
                s1 = s1 + lanes::widen_high(a) * lanes::widen_high(b);
            }
            sum = (lanes::lane(s0, 0) + lanes::lane(s1, 0)) +
                  (lanes::lane(s0, 1) + lanes::lane(s1, 1));
        }
        for (; k < n; ++k)
            sum += static_cast<double>(x[k]) * static_cast<double>(y[k]);
        return sum;
    }

    // Measure a piece with the vector kernels. Returns false, having changed
    // nothing, when any sample in it needs sanitizing.
    bool measure_piece(const SampleType* const* channels, int num_channels, int offset, int n,
                       bool correlate, bool loudness) {
        std::array<PieceLevels, kMaxMeterChannels> levels{};
        for (int ch = 0; ch < num_channels; ++ch)
            if (!measure_levels(channels[ch] + offset, n, levels[static_cast<std::size_t>(ch)]))
                return false;
        for (int ch = 0; ch < num_channels; ++ch) {
            const auto& l = levels[static_cast<std::size_t>(ch)];
            block_peak_[ch] = std::max(block_peak_[ch], l.peak);
            if (l.peak >= 1.0)
                block_clipped_[ch] = true;
            block_sum_sq_[ch] += l.energy;
        }
        if (correlate) {
            correlation_sum_xy_ += cross_energy(channels[0] + offset, channels[1] + offset, n);
            correlation_sum_xx_ += levels[0].energy;
            correlation_sum_yy_ += levels[1].energy;
        }
        if (loudness)
            k_weight_piece(channels, num_channels, offset, n);
        return true;
    }

    // The per-sample definition: every sample sanitized, then measured.
    void measure_piece_sanitized(const SampleType* const* channels, int num_channels, int offset,
                                 int n, bool correlate, bool loudness) {
        for (int i = offset; i < offset + n; ++i) {
            for (int ch = 0; ch < num_channels; ++ch) {
                // Non-finite samples (NaN/Inf) would poison RMS/LUFS/integrated
                // accumulators irrecoverably. Treat them as silence.
                const double sd = sanitize_sample(channels[ch][i]);
                const double abs_s = std::abs(sd);
                if (abs_s > block_peak_[ch])
                    block_peak_[ch] = abs_s;
                if (abs_s >= 1.0)
                    block_clipped_[ch] = true;
                block_sum_sq_[ch] += sd * sd;
                if (loudness) {
                    const double weighted = k_weight(ch, sd);
                    loudness_hop_energy_[ch] += weighted * weighted;
                }
            }
            if (correlate) {
                const double ld = sanitize_sample(channels[0][i]);
                const double rd = sanitize_sample(channels[1][i]);
                correlation_sum_xy_ += ld * rd;
                correlation_sum_xx_ += ld * ld;
                correlation_sum_yy_ += rd * rd;
            }
        }
    }

    // K-weighting for a clean piece: channels in pairs, each pair one
    // two-lane vector through both sections, accumulating its weighted
    // energy into two alternating sums. An odd last channel runs alone.
    void k_weight_piece(const SampleType* const* channels, int num_channels, int offset, int n) {
        namespace lanes = detail::lanes;
        int ch = 0;
        for (; ch + 2 <= num_channels; ch += 2) {
            const SampleType* a = channels[ch] + offset;
            const SampleType* b = channels[ch + 1] + offset;
            const auto u = static_cast<std::size_t>(ch);
            auto pair = [&](const std::array<double, kMaxMeterChannels>& v) {
                return lanes::make(v[u], v[u + 1]);
            };
            D2 x1 = pair(k_x1_), x2 = pair(k_x2_);
            D2 y1 = pair(k_y1_), y2 = pair(k_y2_);
            D2 w1 = pair(k_w1_), w2 = pair(k_w2_);
            D2 even = lanes::splat(0.0), odd = lanes::splat(0.0);
            const Section& sc = shelf_;
            const Section& hc = high_pass_;
            int k = 0;
            auto step = [&](D2 x) {
                const D2 y = section(sc, x, x1, x2, y1, y2);
                const D2 w = section(hc, y, y1, y2, w1, w2);
                x2 = x1;
                x1 = x;
                y2 = y1;
                y1 = y;
                w2 = w1;
                w1 = w;
                return w;
            };
            for (; k + 2 <= n; k += 2) {
                const D2 w0 =
                    step(lanes::make(static_cast<double>(a[k]), static_cast<double>(b[k])));
                even = even + w0 * w0;
                const D2 w1k =
                    step(lanes::make(static_cast<double>(a[k + 1]), static_cast<double>(b[k + 1])));
                odd = odd + w1k * w1k;
            }
            for (; k < n; ++k) {
                const D2 w0 =
                    step(lanes::make(static_cast<double>(a[k]), static_cast<double>(b[k])));
                even = even + w0 * w0;
            }
            const D2 energy = even + odd;
            store_pair(k_x1_, u, x1);
            store_pair(k_x2_, u, x2);
            store_pair(k_y1_, u, y1);
            store_pair(k_y2_, u, y2);
            store_pair(k_w1_, u, w1);
            store_pair(k_w2_, u, w2);
            loudness_hop_energy_[u] += lanes::lane(energy, 0);
            loudness_hop_energy_[u + 1] += lanes::lane(energy, 1);
        }
        for (; ch < num_channels; ++ch) {
            const SampleType* a = channels[ch] + offset;
            double energy = 0.0;
            for (int k = 0; k < n; ++k) {
                const double w = k_weight(ch, static_cast<double>(a[k]));
                energy += w * w;
            }
            loudness_hop_energy_[ch] += energy;
        }
    }

    // One K-weighting section in direct form I, evaluated so the recursion is
    // one multiply-add per sample: the feed-forward sum and the older output
    // term do not depend on the newest output.
    template <typename T> static T section(const Section& c, T x, T x1, T x2, T y1, T y2) {
        if constexpr (std::is_same_v<T, double>) {
            const double head = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a2 * y2;
            return head - c.a1 * y1;
        } else {
            namespace lanes = detail::lanes;
            const T head = lanes::splat(c.b0) * x + lanes::splat(c.b1) * x1 +
                           lanes::splat(c.b2) * x2 - lanes::splat(c.a2) * y2;
            return head - lanes::splat(c.a1) * y1;
        }
    }

    static void store_pair(std::array<double, kMaxMeterChannels>& v, std::size_t u, D2 x) {
        v[u] = detail::lanes::lane(x, 0);
        v[u + 1] = detail::lanes::lane(x, 1);
    }

    void reset_measurement_state(int active_channels,
                                 bool process_loudness = false) {
        for (int ch = 0; ch < kMaxMeterChannels; ++ch) {
            block_peak_[ch] = 0.0f;
            block_sum_sq_[ch] = 0.0f;
            block_clipped_[ch] = false;
        }
        block_samples_ = 0;
        correlation_sum_xy_ = 0.0;
        correlation_sum_xx_ = 0.0;
        correlation_sum_yy_ = 0.0;
        correlation_samples_ = 0;
        reset_loudness_state();
        snapshot_ = {};
        active_num_channels_ = active_channels;
        active_loudness_processing_ = process_loudness;
        snapshot_.num_channels = active_channels;
    }

    void reset_correlation_accumulators() {
        correlation_sum_xy_ = 0.0;
        correlation_sum_xx_ = 0.0;
        correlation_sum_yy_ = 0.0;
        correlation_samples_ = 0;
    }

    void emit_snapshot(int num_channels) {
        snapshot_.num_channels = num_channels;

        for (int ch = 0; ch < num_channels; ++ch) {
            auto& out = snapshot_.channels[ch];
            out.peak = static_cast<float>(block_peak_[ch]);
            out.rms = block_samples_ > 0
                ? static_cast<float>(std::sqrt(block_sum_sq_[ch] / block_samples_))
                : 0.0f;
            out.clipped = block_clipped_[ch];

            out.lufs_momentary = channel_momentary_[ch];

            // Reset block accumulators
            block_peak_[ch] = 0.0f;
            block_sum_sq_[ch] = 0.0f;
            block_clipped_[ch] = false;
        }

        // Stereo correlation
        if (num_channels >= 2 && correlation_samples_ > 0) {
            double denom = std::sqrt(correlation_sum_xx_ * correlation_sum_yy_);
            snapshot_.correlation = denom > 1e-10
                ? static_cast<float>(correlation_sum_xy_ / denom)
                : 0.0f;
        } else {
            snapshot_.correlation = 0.0f;
        }

        // Reset correlation accumulators periodically (every ~100ms)
        int corr_window = static_cast<int>(sample_rate_ * 0.1);
        if (correlation_samples_ >= corr_window) {
            reset_correlation_accumulators();
        }

        block_samples_ = 0;
    }

    static constexpr double kAbsoluteGateLufs = -70.0;
    // The input ceiling above bounds any supported programme below
    // +140 LUFS, including K-weighting gain and surround weights. 0.01 LU is
    // comfortably inside the 0.1 LU EBU minimum-requirements tolerance while
    // keeping each prepared histogram near 0.5 MiB including epoch stamps.
    static constexpr double kGateMaximumLufs = 140.0;
    static constexpr double kGateBinWidthLu = 0.01;
    static constexpr std::size_t kGateBinCount = 21001;
    static constexpr double kGateMaximumEnergy = 1.0e14;
    static constexpr std::size_t kShortTermHopCount = 30;

    // One channel, one sample, through both K-weighting sections — the same
    // arithmetic k_weight_piece() runs on channel pairs.
    double k_weight(int channel, double x) {
        const auto c = static_cast<std::size_t>(channel);
        const double y = section(shelf_, x, k_x1_[c], k_x2_[c], k_y1_[c], k_y2_[c]);
        const double w = section(high_pass_, y, k_y1_[c], k_y2_[c], k_w1_[c], k_w2_[c]);
        k_x2_[c] = k_x1_[c];
        k_x1_[c] = x;
        k_y2_[c] = k_y1_[c];
        k_y1_[c] = y;
        k_w2_[c] = k_w1_[c];
        k_w1_[c] = w;
        return w;
    }

    static double sanitize_sample(SampleType sample) {
        const double value = static_cast<double>(sample);
        if (!std::isfinite(value)) return 0.0;
        return std::clamp(value, -kMaxMeterInputMagnitude,
                          kMaxMeterInputMagnitude);
    }

    static float energy_to_lufs(double energy) {
        return energy > 0.0 && std::isfinite(energy)
            ? static_cast<float>(-0.691 + 10.0 * std::log10(energy))
            : -std::numeric_limits<float>::infinity();
    }

    static bool role_weight(LoudnessChannelRole role, double& weight) noexcept {
        switch (role) {
            case LoudnessChannelRole::lfe:
                weight = 0.0;
                return true;
            case LoudnessChannelRole::left_surround:
            case LoudnessChannelRole::right_surround:
                weight = 1.41;
                return true;
            case LoudnessChannelRole::left:
            case LoudnessChannelRole::right:
            case LoudnessChannelRole::center:
            case LoudnessChannelRole::left_rear_surround:
            case LoudnessChannelRole::right_rear_surround:
                weight = 1.0;
                return true;
            case LoudnessChannelRole::unknown:
                return false;
        }
        return false;
    }

    bool configure_channel_roles(const LoudnessChannelRole* roles) noexcept {
        channel_weights_.fill(0.0);
        if (num_channels_ < 1) return false;

        constexpr std::array<LoudnessChannelRole, 8> eight_channel_default{
            LoudnessChannelRole::left, LoudnessChannelRole::right,
            LoudnessChannelRole::center, LoudnessChannelRole::lfe,
            LoudnessChannelRole::left_surround, LoudnessChannelRole::right_surround,
            LoudnessChannelRole::left_rear_surround, LoudnessChannelRole::right_rear_surround};
        std::array<LoudnessChannelRole, kMaxMeterChannels> configured{};

        if (roles) {
            for (int ch = 0; ch < num_channels_; ++ch) {
                const auto role = roles[ch];
                configured[static_cast<std::size_t>(ch)] = role;
            }
        } else {
            switch (num_channels_) {
                case 1:
                    configured[0] = LoudnessChannelRole::center;
                    break;
                case 2:
                    configured[0] = LoudnessChannelRole::left;
                    configured[1] = LoudnessChannelRole::right;
                    break;
                case 3:
                    std::copy_n(eight_channel_default.begin(), 3, configured.begin());
                    break;
                case 4:
                    configured = {LoudnessChannelRole::left, LoudnessChannelRole::right,
                                  LoudnessChannelRole::left_surround,
                                  LoudnessChannelRole::right_surround};
                    break;
                case 5:
                    configured = {LoudnessChannelRole::left, LoudnessChannelRole::right,
                                  LoudnessChannelRole::center,
                                  LoudnessChannelRole::left_surround,
                                  LoudnessChannelRole::right_surround};
                    break;
                case 6:
                    std::copy_n(eight_channel_default.begin(), 6, configured.begin());
                    break;
                case 8:
                    std::copy(eight_channel_default.begin(),
                              eight_channel_default.end(), configured.begin());
                    break;
                default:
                    return false;
            }
        }

        bool has_program_channel = false;
        for (int ch = 0; ch < num_channels_; ++ch) {
            double weight = 0.0;
            if (!role_weight(configured[static_cast<std::size_t>(ch)], weight)) return false;
            channel_weights_[static_cast<std::size_t>(ch)] = weight;
            has_program_channel = has_program_channel || weight > 0.0;
        }
        return has_program_channel;
    }

    void configure_k_weighting() {
        shelf_ = {};
        high_pass_ = {};
        reset_k_weighting_state();
        const auto coefficients = k_weighting_coefficients(sample_rate_);
        if (!loudness_valid_ || !coefficients.valid) {
            loudness_valid_ = false;
            return;
        }
        shelf_ = Section::from(coefficients.shelf);
        high_pass_ = Section::from(coefficients.high_pass);
    }

    void reset_k_weighting_state() {
        for (auto* v : {&k_x1_, &k_x2_, &k_y1_, &k_y2_, &k_w1_, &k_w2_})
            v->fill(0.0);
    }

    void reset_loudness_state() {
        reset_k_weighting_state();
        loudness_hop_energy_.fill(0.0);
        for (auto& hop : loudness_energy_ring_) hop.fill(0.0);
        loudness_short_term_ring_.fill(0.0);
        channel_momentary_.fill(-std::numeric_limits<float>::infinity());
        loudness_hop_position_ = 0;
        loudness_hops_completed_ = 0;
        loudness_ring_position_ = 0;
        loudness_short_term_position_ = 0;
        snapshot_.lufs_momentary = -std::numeric_limits<float>::infinity();
        snapshot_.lufs_integrated = -std::numeric_limits<float>::infinity();
        snapshot_.short_term_lufs = -std::numeric_limits<float>::infinity();
        snapshot_.short_term_valid = false;
        snapshot_.loudness_range_lu = 0.0f;
        snapshot_.loudness_range_valid = false;
        gate_nodes_initialized_ = 0;
        // A 64-bit epoch cannot wrap in a practical process lifetime. If it
        // nevertheless exhausts, stop accumulating integrated loudness until
        // the control thread calls prepare(); never bulk-clear on process().
        if (gate_epoch_ == std::numeric_limits<std::uint64_t>::max())
            gate_epoch_active_ = false;
        else
            ++gate_epoch_;
    }

    void finish_loudness_hop(int num_channels) {
        double hop_program_energy = 0.0;
        for (int ch = 0; ch < num_channels; ++ch)
            hop_program_energy += channel_weights_[ch] * loudness_hop_energy_[ch]
                                  / static_cast<double>(loudness_hop_samples_);
        loudness_short_term_ring_[loudness_short_term_position_] = std::min(
            hop_program_energy, kGateMaximumEnergy);
        loudness_short_term_position_ = (loudness_short_term_position_ + 1) % kShortTermHopCount;
        loudness_energy_ring_[loudness_ring_position_] = loudness_hop_energy_;
        loudness_hop_energy_.fill(0.0);
        loudness_hop_position_ = 0;
        loudness_ring_position_ = (loudness_ring_position_ + 1) % 4;
        ++loudness_hops_completed_;
        if (loudness_hops_completed_ < 4) return;

        double program_energy = 0.0;
        for (int ch = 0; ch < num_channels; ++ch) {
            double channel_energy = 0.0;
            for (const auto& hop : loudness_energy_ring_) channel_energy += hop[ch];
            channel_energy /= static_cast<double>(4 * loudness_hop_samples_);
            const double weighted_energy = std::min(
                channel_weights_[ch] * channel_energy, kGateMaximumEnergy);
            channel_momentary_[ch] = energy_to_lufs(weighted_energy);
            program_energy = std::min(
                program_energy + weighted_energy, kGateMaximumEnergy);
        }
        snapshot_.lufs_momentary = energy_to_lufs(program_energy);

        if (energy_to_lufs(program_energy) > kAbsoluteGateLufs) {
            add_gating_block(program_energy);
            update_integrated_loudness();
        }
        update_short_term_and_range();
    }

    void add_gating_block(double energy) {
        if (!gate_epoch_active_) return;
        const double loudness = energy_to_lufs(energy);
        const double clamped = std::clamp(loudness, kAbsoluteGateLufs, kGateMaximumLufs);
        const auto bin = std::min(kGateBinCount - 1, static_cast<std::size_t>(
            (clamped - kAbsoluteGateLufs) / kGateBinWidthLu));
        for (std::size_t index = bin + 1; index <= kGateBinCount;
             index += low_bit(index)) {
            if (gate_node_epoch_[index] != gate_epoch_) {
                gate_node_epoch_[index] = gate_epoch_;
                gate_energy_tree_[index] = 0.0;
                gate_count_tree_[index] = 0;
                ++gate_nodes_initialized_;
            }
            gate_energy_tree_[index] += energy;
            ++gate_count_tree_[index];
        }
    }

    std::pair<double, std::uint64_t> gating_blocks_from(std::size_t first_bin) const {
        if (!gate_epoch_active_) return {0.0, 0};
        double prefix_energy = 0.0;
        std::uint64_t prefix_count = 0;
        for (std::size_t index = first_bin; index > 0; index -= low_bit(index)) {
            if (gate_node_epoch_[index] == gate_epoch_) {
                prefix_energy += gate_energy_tree_[index];
                prefix_count += gate_count_tree_[index];
            }
        }
        double total_energy = 0.0;
        std::uint64_t total_count = 0;
        for (std::size_t index = kGateBinCount; index > 0; index -= low_bit(index)) {
            if (gate_node_epoch_[index] == gate_epoch_) {
                total_energy += gate_energy_tree_[index];
                total_count += gate_count_tree_[index];
            }
        }
        return {total_energy - prefix_energy, total_count - prefix_count};
    }

    static constexpr std::size_t low_bit(std::size_t value) {
        return value & (~value + 1);
    }

    void update_integrated_loudness() {
        const auto [absolute_sum, absolute_count] = gating_blocks_from(0);
        if (absolute_count == 0) {
            snapshot_.lufs_integrated = -std::numeric_limits<float>::infinity();
            return;
        }
        const double relative_gate = energy_to_lufs(absolute_sum / absolute_count) - 10.0;
        const auto first_bin = relative_gate <= kAbsoluteGateLufs ? 0
            : std::min(kGateBinCount, static_cast<std::size_t>(
                (relative_gate - kAbsoluteGateLufs) / kGateBinWidthLu) + 1);
        const auto [gated_sum, gated_count] = gating_blocks_from(first_bin);
        snapshot_.lufs_integrated = gated_count > 0
            ? energy_to_lufs(gated_sum / gated_count)
            : -std::numeric_limits<float>::infinity();
    }

    void update_short_term_and_range() {
        if (loudness_hops_completed_ < kShortTermHopCount) return;
        double energy = 0.0;
        for (const double hop : loudness_short_term_ring_) energy += hop;
        snapshot_.short_term_lufs = energy_to_lufs(energy / kShortTermHopCount);
        snapshot_.short_term_valid = std::isfinite(snapshot_.short_term_lufs);
        if (!snapshot_.short_term_valid) return;
        if (snapshot_.short_term_lufs > kAbsoluteGateLufs)
            add_lra_block(snapshot_.short_term_lufs);
        update_loudness_range();
    }

    void add_lra_block(double loudness) {
        const auto bin = std::min(kGateBinCount - 1, static_cast<std::size_t>(
            (std::clamp(loudness, kAbsoluteGateLufs, kGateMaximumLufs)
             - kAbsoluteGateLufs) / kGateBinWidthLu));
        for (std::size_t index = bin + 1; index <= kGateBinCount; index += low_bit(index)) {
            if (lra_node_epoch_[index] != gate_epoch_) {
                lra_node_epoch_[index] = gate_epoch_;
                lra_count_tree_[index] = 0;
            }
            ++lra_count_tree_[index];
        }
    }

    std::uint64_t lra_prefix_count(std::size_t exclusive_bin) const {
        std::uint64_t count = 0;
        for (std::size_t index = exclusive_bin; index > 0; index -= low_bit(index))
            if (lra_node_epoch_[index] == gate_epoch_) count += lra_count_tree_[index];
        return count;
    }

    float lra_value_at_rank(std::size_t first_bin, std::uint64_t rank) const {
        const std::uint64_t target = lra_prefix_count(first_bin) + rank;
        std::size_t low = 0, high = kGateBinCount - 1;
        while (low < high) {
            const auto mid = low + (high - low) / 2;
            if (lra_prefix_count(mid + 1) >= target) high = mid;
            else low = mid + 1;
        }
        return static_cast<float>(kAbsoluteGateLufs + low * kGateBinWidthLu);
    }

    void update_loudness_range() {
        if (!std::isfinite(snapshot_.lufs_integrated)) return;
        const double gate = std::max(kAbsoluteGateLufs,
            static_cast<double>(snapshot_.lufs_integrated) - 20.0);
        const auto first_bin = gate <= kAbsoluteGateLufs ? 0 : std::min(kGateBinCount,
            static_cast<std::size_t>((gate - kAbsoluteGateLufs) / kGateBinWidthLu) + 1);
        const auto count = lra_prefix_count(kGateBinCount) - lra_prefix_count(first_bin);
        if (count == 0) return;
        const auto p10 = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(count * 0.10)));
        const auto p95 = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(count * 0.95)));
        snapshot_.loudness_range_lu = lra_value_at_rank(first_bin, p95)
                                   - lra_value_at_rank(first_bin, p10);
        snapshot_.loudness_range_valid = true;
    }

    double sample_rate_ = 44100.0;
    int num_channels_ = 2;
    MeterFeatures features_ = MeterFeatures::all;

    // Block accumulators
    double block_peak_[kMaxMeterChannels] = {};
    double block_sum_sq_[kMaxMeterChannels] = {};
    bool block_clipped_[kMaxMeterChannels] = {};
    int block_samples_ = 0;

    bool loudness_valid_ = false;
    int loudness_hop_samples_ = 4410;
    int loudness_hop_position_ = 0;
    std::uint64_t loudness_hops_completed_ = 0;
    std::size_t loudness_ring_position_ = 0;
    int active_num_channels_ = 0;
    bool active_loudness_processing_ = false;
    Section shelf_{};
    Section high_pass_{};
    // Direct-form-I history per channel: the input (x), the shelf output
    // (y, which is also the high-pass input), and the high-pass output (w).
    std::array<double, kMaxMeterChannels> k_x1_{};
    std::array<double, kMaxMeterChannels> k_x2_{};
    std::array<double, kMaxMeterChannels> k_y1_{};
    std::array<double, kMaxMeterChannels> k_y2_{};
    std::array<double, kMaxMeterChannels> k_w1_{};
    std::array<double, kMaxMeterChannels> k_w2_{};
    std::array<double, kMaxMeterChannels> channel_weights_{};
    std::array<double, kMaxMeterChannels> loudness_hop_energy_{};
    std::array<std::array<double, kMaxMeterChannels>, 4> loudness_energy_ring_{};
    std::array<double, kShortTermHopCount> loudness_short_term_ring_{};
    std::size_t loudness_short_term_position_ = 0;
    std::array<float, kMaxMeterChannels> channel_momentary_{};

    // Correlation accumulators
    double correlation_sum_xy_ = 0.0;
    double correlation_sum_xx_ = 0.0;
    double correlation_sum_yy_ = 0.0;
    int correlation_samples_ = 0;

    std::vector<double> gate_energy_tree_;
    std::vector<std::uint64_t> gate_count_tree_;
    std::vector<std::uint64_t> gate_node_epoch_;
    std::vector<std::uint64_t> lra_count_tree_;
    std::vector<std::uint64_t> lra_node_epoch_;
    std::uint64_t gate_epoch_ = 0;
    std::size_t gate_nodes_initialized_ = 0;
    bool gate_epoch_active_ = true;

    MultiChannelMeterData snapshot_;
};

using MultiChannelMeter = MultiChannelMeterT<float>;
using MultiChannelMeter64 = MultiChannelMeterT<double>;

} // namespace pulp::signal

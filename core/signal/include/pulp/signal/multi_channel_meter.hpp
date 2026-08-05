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

#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>
#include <utility>
#include <vector>

namespace pulp::signal {

/// Maximum supported channel count for metering.
static constexpr int kMaxMeterChannels = 16;

/// Finite samples outside this deliberately generous audio-domain ceiling are
/// clipped before every peak, RMS, correlation, and loudness calculation.
/// This keeps double-input arithmetic deterministic instead of allowing a
/// finite but non-audio value to overflow a squared-energy accumulator.
static constexpr double kMaxMeterInputMagnitude = 1.0e6;

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
    void update(const MultiChannelMeterData& data, float dt) {
        num_channels = std::clamp(data.num_channels, 0, kMaxMeterChannels);

        float attack_coeff = 1.0f - std::exp(-dt / attack_time);
        float release_coeff = 1.0f - std::exp(-dt / release_time);

        for (int ch = 0; ch < num_channels; ++ch) {
            auto& b = channels[ch];
            auto& d = data.channels[ch];

            // Peak
            if (d.peak > b.display_peak)
                b.display_peak += (d.peak - b.display_peak) * attack_coeff;
            else
                b.display_peak += (d.peak - b.display_peak) * release_coeff;

            // RMS
            if (d.rms > b.display_rms)
                b.display_rms += (d.rms - b.display_rms) * attack_coeff;
            else
                b.display_rms += (d.rms - b.display_rms) * release_coeff;

            // Peak hold
            if (d.peak >= b.held_peak) {
                b.held_peak = d.peak;
                b.hold_counter = peak_hold_time;
            } else {
                b.hold_counter -= dt;
                if (b.hold_counter <= 0)
                    b.held_peak += (0.0f - b.held_peak) * release_coeff;
            }

            // Clip indicator
            if (d.clipped) {
                b.clip_indicator = true;
                b.clip_hold_counter = clip_hold_time;
            } else {
                b.clip_hold_counter -= dt;
                if (b.clip_hold_counter <= 0)
                    b.clip_indicator = false;
            }

            // Clamp noise floor
            if (b.display_peak < 1e-6f) b.display_peak = 0;
            if (b.display_rms < 1e-6f) b.display_rms = 0;
        }
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
/// The default layout is mono, stereo, L/R/C, quad, 5.0, 5.1, then the common
/// L/R/C/LFE/Ls/Rs/Lrs/Rrs order. Pass explicit roles for any other order.
/// This is not a complete EBU Mode meter: it does not expose 3-second
/// short-term loudness, loudness range, scale/UI behavior, or true peak.
///
/// Call process() from the audio callback. Read results via snapshot().
template <typename SampleType = float>
class MultiChannelMeterT {
public:
    void prepare(double sample_rate, int num_channels) {
        prepare(sample_rate, num_channels, nullptr);
    }

    /// Prepare the loudness meter with an explicit speaker layout. Integrated
    /// gating uses a prepared 0.01 LU Fenwick histogram: memory and update
    /// cost remain fixed regardless of measurement duration. Allocation occurs
    /// only here.
    void prepare(double sample_rate, int num_channels,
                 const LoudnessChannelRole* roles) {
        sample_rate_ = sample_rate;
        num_channels_ = std::clamp(num_channels, 0, kMaxMeterChannels);
        loudness_valid_ = std::isfinite(sample_rate) && sample_rate > 0.0;
        loudness_hop_samples_ = loudness_valid_
            ? std::max(1, static_cast<int>(std::llround(sample_rate * 0.1))) : 0;

        configure_channel_roles(roles);
        configure_k_weighting();
        gate_energy_tree_.assign(kGateBinCount + 1, 0.0);
        gate_count_tree_.assign(kGateBinCount + 1, 0);
        gate_node_epoch_.assign(kGateBinCount + 1, 0);
        gate_epoch_ = 0;
        gate_epoch_active_ = true;

        reset_measurement_state(num_channels_);
    }

    /// Process a block of interleaved or deinterleaved audio.
    /// channels: array of channel pointers. num_samples: samples per channel.
    void process(const SampleType* const* channels, int num_channels, int num_samples) {
        num_channels = std::clamp(num_channels, 0, num_channels_);
        if (channels == nullptr || num_samples <= 0) return;

        for (int ch = 0; ch < num_channels; ++ch) {
            if (channels[ch] == nullptr) {
                num_channels = ch;
                break;
            }
        }

        // A topology transition starts a new measurement programme. Reset all
        // windows together so peak/RMS/clip/correlation and loudness never mix
        // samples from different layouts.
        if (num_channels != active_num_channels_)
            reset_measurement_state(num_channels);

        for (int i = 0; i < num_samples; ++i) {
            for (int ch = 0; ch < num_channels; ++ch) {
                // Non-finite samples (NaN/Inf) would poison RMS/LUFS/integrated
                // accumulators irrecoverably (#2695). Treat them as silence.
                const double sd = sanitize_sample(channels[ch][i]);
                double abs_s = std::abs(sd);

                if (abs_s > block_peak_[ch]) block_peak_[ch] = abs_s;
                if (abs_s >= 1.0) block_clipped_[ch] = true;

                block_sum_sq_[ch] += sd * sd;
                if (loudness_valid_) {
                    const double weighted = k_filters_[ch].process(sd);
                    loudness_hop_energy_[ch] += weighted * weighted;
                }
            }

            // Stereo correlation
            if (num_channels >= 2) {
                const double ld = sanitize_sample(channels[0][i]);
                const double rd = sanitize_sample(channels[1][i]);
                correlation_sum_xy_ += ld * rd;
                correlation_sum_xx_ += ld * ld;
                correlation_sum_yy_ += rd * rd;
                ++correlation_samples_;
            }

            ++block_samples_;
            if (loudness_valid_ && ++loudness_hop_position_ == loudness_hop_samples_)
                finish_loudness_hop(num_channels);
        }

        // Emit snapshot when we have enough samples for a meaningful measurement
        // Use ~10ms blocks for responsive metering
        int block_size = static_cast<int>(sample_rate_ * 0.01f);
        if (block_size < 1) block_size = 1;

        if (block_samples_ >= block_size) {
            emit_snapshot(num_channels);
        }
    }

    /// Get the latest metering snapshot.
    const MultiChannelMeterData& snapshot() const { return snapshot_; }

    /// Diagnostics for proving that programme reset is a logical epoch change
    /// rather than a sweep over the prepared histogram storage.
    std::uint64_t loudness_histogram_epoch() const { return gate_epoch_; }
    std::size_t loudness_histogram_nodes_initialized() const {
        return gate_nodes_initialized_;
    }
    std::size_t loudness_histogram_reset_work_units() const {
        return gate_reset_work_units_;
    }
    static constexpr std::size_t loudness_histogram_capacity() {
        return kGateBinCount;
    }

    void reset() {
        reset_measurement_state(0);
    }

private:
    void reset_measurement_state(int active_channels) {
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
        int corr_window = static_cast<int>(sample_rate_ * 0.1f);
        if (correlation_samples_ >= corr_window) {
            reset_correlation_accumulators();
        }

        block_samples_ = 0;
    }

    static constexpr double kAbsoluteGateLufs = -70.0;
    // The input ceiling above bounds any supported 16-channel programme below
    // +140 LUFS, including K-weighting gain and surround weights. 0.01 LU is
    // comfortably inside the 0.1 LU EBU minimum-requirements tolerance while
    // keeping each prepared histogram near 0.5 MiB including epoch stamps.
    static constexpr double kGateMaximumLufs = 140.0;
    static constexpr double kGateBinWidthLu = 0.01;
    static constexpr std::size_t kGateBinCount = 21001;
    static constexpr double kGateMaximumEnergy = 1.0e14;

    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        double process(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() { z1 = z2 = 0.0; }
    };

    struct KWeighting {
        Biquad shelf;
        Biquad high_pass;
        double process(double x) { return high_pass.process(shelf.process(x)); }
        void reset() { shelf.reset(); high_pass.reset(); }
    };

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

    static double role_weight(LoudnessChannelRole role) {
        switch (role) {
            case LoudnessChannelRole::lfe: return 0.0;
            case LoudnessChannelRole::left_surround:
            case LoudnessChannelRole::right_surround: return 1.41;
            default: return 1.0;
        }
    }

    void configure_channel_roles(const LoudnessChannelRole* roles) {
        channel_weights_.fill(1.0);
        std::array<LoudnessChannelRole, 8> defaults{
            LoudnessChannelRole::left, LoudnessChannelRole::right,
            LoudnessChannelRole::center, LoudnessChannelRole::lfe,
            LoudnessChannelRole::left_surround, LoudnessChannelRole::right_surround,
            LoudnessChannelRole::left_rear_surround, LoudnessChannelRole::right_rear_surround};
        for (int ch = 0; ch < num_channels_; ++ch) {
            auto role = roles ? roles[ch] : defaults[static_cast<std::size_t>(std::min(ch, 7))];
            if (!roles && num_channels_ == 4)
                role = ch < 2 ? defaults[ch] : defaults[ch + 2];
            if (!roles && num_channels_ == 5)
                role = ch < 3 ? defaults[ch] : defaults[ch + 1];
            channel_weights_[ch] = role_weight(role);
        }
    }

    void configure_k_weighting() {
        for (auto& filter : k_filters_) filter = {};
        const auto coefficients = k_weighting_coefficients(sample_rate_);
        if (!loudness_valid_ || !coefficients.valid) {
            loudness_valid_ = false;
            return;
        }
        for (auto& filter : k_filters_) {
            const auto& s = coefficients.shelf;
            const auto& h = coefficients.high_pass;
            filter.shelf = {s.b0, s.b1, s.b2, s.a1, s.a2};
            filter.high_pass = {h.b0, h.b1, h.b2, h.a1, h.a2};
        }
    }

    void reset_loudness_state() {
        for (auto& filter : k_filters_) filter.reset();
        loudness_hop_energy_.fill(0.0);
        for (auto& hop : loudness_energy_ring_) hop.fill(0.0);
        channel_momentary_.fill(-std::numeric_limits<float>::infinity());
        loudness_hop_position_ = 0;
        loudness_hops_completed_ = 0;
        loudness_ring_position_ = 0;
        snapshot_.lufs_momentary = -std::numeric_limits<float>::infinity();
        snapshot_.lufs_integrated = -std::numeric_limits<float>::infinity();
        gate_nodes_initialized_ = 0;
        gate_reset_work_units_ = 1;
        // A 64-bit epoch cannot wrap in a practical process lifetime. If it
        // nevertheless exhausts, stop accumulating integrated loudness until
        // the control thread calls prepare(); never bulk-clear on process().
        if (gate_epoch_ == std::numeric_limits<std::uint64_t>::max())
            gate_epoch_active_ = false;
        else
            ++gate_epoch_;
    }

    void finish_loudness_hop(int num_channels) {
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

    double sample_rate_ = 44100.0;
    int num_channels_ = 2;

    // Block accumulators
    double block_peak_[kMaxMeterChannels] = {};
    double block_sum_sq_[kMaxMeterChannels] = {};
    bool block_clipped_[kMaxMeterChannels] = {};
    int block_samples_ = 0;

    bool loudness_valid_ = true;
    int loudness_hop_samples_ = 4410;
    int loudness_hop_position_ = 0;
    std::uint64_t loudness_hops_completed_ = 0;
    std::size_t loudness_ring_position_ = 0;
    int active_num_channels_ = 0;
    std::array<KWeighting, kMaxMeterChannels> k_filters_{};
    std::array<double, kMaxMeterChannels> channel_weights_{};
    std::array<double, kMaxMeterChannels> loudness_hop_energy_{};
    std::array<std::array<double, kMaxMeterChannels>, 4> loudness_energy_ring_{};
    std::array<float, kMaxMeterChannels> channel_momentary_{};

    // Correlation accumulators
    double correlation_sum_xy_ = 0.0;
    double correlation_sum_xx_ = 0.0;
    double correlation_sum_yy_ = 0.0;
    int correlation_samples_ = 0;

    std::vector<double> gate_energy_tree_;
    std::vector<std::uint64_t> gate_count_tree_;
    std::vector<std::uint64_t> gate_node_epoch_;
    std::uint64_t gate_epoch_ = 0;
    std::size_t gate_nodes_initialized_ = 0;
    std::size_t gate_reset_work_units_ = 0;
    bool gate_epoch_active_ = true;

    MultiChannelMeterData snapshot_;
};

using MultiChannelMeter = MultiChannelMeterT<float>;
using MultiChannelMeter64 = MultiChannelMeterT<double>;

} // namespace pulp::signal

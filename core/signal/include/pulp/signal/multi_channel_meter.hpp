#pragma once

/// @file multi_channel_meter.hpp
/// Multi-channel metering: peak, RMS, LUFS (momentary/short-term/integrated),
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
#include <numeric>

namespace pulp::signal {

/// Maximum supported channel count for metering.
static constexpr int kMaxMeterChannels = 16;

/// Per-channel level measurements computed on the audio thread.
struct ChannelLevels {
    float peak = 0.0f;           // True peak (linear, 0–1+)
    float rms = 0.0f;            // RMS (linear, 0–1+)
    float lufs_momentary = -std::numeric_limits<float>::infinity(); // LUFS, 400ms window
    bool clipped = false;        // True if any sample >= 1.0
};

/// Complete metering snapshot for all channels, published lock-free.
struct MultiChannelMeterData {
    std::array<ChannelLevels, kMaxMeterChannels> channels{};
    int num_channels = 0;
    float correlation = 0.0f;    // Stereo correlation (-1 to +1), valid when num_channels >= 2
    float lufs_integrated = -std::numeric_limits<float>::infinity(); // ITU-R BS.1770 integrated
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
    /// tick down invisibly.
    bool update(const MultiChannelMeterData& data, float dt) {
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

/// Audio-thread metering processor. Computes peak, RMS, LUFS momentary,
/// stereo correlation, and clip detection for up to kMaxMeterChannels.
///
/// Call process() from the audio callback. Read results via snapshot().
template <typename SampleType = float>
class MultiChannelMeterT {
public:
    void prepare(double sample_rate, int num_channels) {
        sample_rate_ = sample_rate;
        num_channels_ = std::clamp(num_channels, 0, kMaxMeterChannels);

        // LUFS momentary window: 400ms
        lufs_window_samples_ = static_cast<int>(sample_rate * 0.4);

        // Reset accumulators
        for (int ch = 0; ch < kMaxMeterChannels; ++ch) {
            block_peak_[ch] = 0.0f;
            block_sum_sq_[ch] = 0.0f;
            block_clipped_[ch] = false;
            lufs_sum_sq_[ch] = 0.0f;
        }
        block_samples_ = 0;
        lufs_samples_ = 0;
        correlation_sum_xy_ = 0.0;
        correlation_sum_xx_ = 0.0;
        correlation_sum_yy_ = 0.0;
        correlation_samples_ = 0;

        // Integrated LUFS
        integrated_sum_ = 0.0;
        integrated_blocks_ = 0;

        snapshot_ = {};
        snapshot_.num_channels = num_channels_;
    }

    /// Process a block of interleaved or deinterleaved audio.
    /// channels: array of channel pointers. num_samples: samples per channel.
    void process(const SampleType* const* channels, int num_channels, int num_samples) {
        num_channels = std::clamp(num_channels, 0, num_channels_);
        if (channels == nullptr || num_samples <= 0) return;

        for (int ch = 0; ch < num_channels; ++ch) {
            if (channels[ch] == nullptr) {
                clear_inactive_channel_accumulators(ch, num_channels);
                if (ch < 2) reset_correlation_accumulators();
                num_channels = ch;
                break;
            }
        }

        for (int i = 0; i < num_samples; ++i) {
            for (int ch = 0; ch < num_channels; ++ch) {
                SampleType s = channels[ch][i];
                // Non-finite samples (NaN/Inf) would poison RMS/LUFS/integrated
                // accumulators irrecoverably (#2695). Treat them as silence.
                if (!std::isfinite(static_cast<double>(s))) s = SampleType{0};
                const double sd = static_cast<double>(s);
                double abs_s = std::abs(sd);

                if (abs_s > block_peak_[ch]) block_peak_[ch] = abs_s;
                if (abs_s >= 1.0) block_clipped_[ch] = true;

                block_sum_sq_[ch] += sd * sd;
                lufs_sum_sq_[ch] += sd * sd;
            }

            // Stereo correlation
            if (num_channels >= 2) {
                SampleType l = channels[0][i];
                SampleType r = channels[1][i];
                if (!std::isfinite(static_cast<double>(l))) l = SampleType{0};
                if (!std::isfinite(static_cast<double>(r))) r = SampleType{0};
                const double ld = static_cast<double>(l);
                const double rd = static_cast<double>(r);
                correlation_sum_xy_ += ld * rd;
                correlation_sum_xx_ += ld * ld;
                correlation_sum_yy_ += rd * rd;
                ++correlation_samples_;
            }

            ++block_samples_;
            ++lufs_samples_;
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

    void reset() {
        for (int ch = 0; ch < kMaxMeterChannels; ++ch) {
            block_peak_[ch] = 0.0f;
            block_sum_sq_[ch] = 0.0f;
            lufs_sum_sq_[ch] = 0.0f;
            block_clipped_[ch] = false;
        }
        block_samples_ = 0;
        lufs_samples_ = 0;
        correlation_sum_xy_ = 0.0;
        correlation_sum_xx_ = 0.0;
        correlation_sum_yy_ = 0.0;
        correlation_samples_ = 0;
        integrated_sum_ = 0.0;
        integrated_blocks_ = 0;
        snapshot_ = {};
    }

private:
    void clear_channel_accumulators(int ch) {
        block_peak_[ch] = 0.0f;
        block_sum_sq_[ch] = 0.0;
        block_clipped_[ch] = false;
        lufs_sum_sq_[ch] = 0.0;
        snapshot_.channels[ch] = {};
    }

    void clear_inactive_channel_accumulators(int first_channel, int limit) {
        limit = std::clamp(limit, 0, kMaxMeterChannels);
        for (int ch = first_channel; ch < limit; ++ch)
            clear_channel_accumulators(ch);
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

            // LUFS momentary (simplified ITU-R BS.1770)
            if (lufs_samples_ > 0) {
                float mean_sq = static_cast<float>(lufs_sum_sq_[ch] / lufs_samples_);
                out.lufs_momentary = mean_sq > 1e-10f
                    ? -0.691f + 10.0f * std::log10(mean_sq)
                    : -std::numeric_limits<float>::infinity();
            }

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

        // LUFS integrated (running average of momentary measurements)
        if (num_channels > 0 && lufs_samples_ >= lufs_window_samples_) {
            // Average across channels for integrated LUFS
            double channel_sum = 0.0;
            for (int ch = 0; ch < num_channels; ++ch) {
                channel_sum += lufs_sum_sq_[ch] / lufs_samples_;
            }
            double mean_sq = channel_sum / num_channels;

            if (mean_sq > 1e-10) {
                integrated_sum_ += mean_sq;
                ++integrated_blocks_;
                double avg = integrated_sum_ / integrated_blocks_;
                snapshot_.lufs_integrated = -0.691f + 10.0f * static_cast<float>(std::log10(avg));
            }

            // Reset LUFS window
            for (int ch = 0; ch < kMaxMeterChannels; ++ch)
                lufs_sum_sq_[ch] = 0.0;
            lufs_samples_ = 0;
        }

        // Reset correlation accumulators periodically (every ~100ms)
        int corr_window = static_cast<int>(sample_rate_ * 0.1f);
        if (correlation_samples_ >= corr_window) {
            reset_correlation_accumulators();
        }

        block_samples_ = 0;
    }

    double sample_rate_ = 44100.0;
    int num_channels_ = 2;
    int lufs_window_samples_ = 17640; // 400ms at 44100

    // Block accumulators
    double block_peak_[kMaxMeterChannels] = {};
    double block_sum_sq_[kMaxMeterChannels] = {};
    bool block_clipped_[kMaxMeterChannels] = {};
    int block_samples_ = 0;

    // LUFS accumulators
    double lufs_sum_sq_[kMaxMeterChannels] = {};
    int lufs_samples_ = 0;

    // Correlation accumulators
    double correlation_sum_xy_ = 0.0;
    double correlation_sum_xx_ = 0.0;
    double correlation_sum_yy_ = 0.0;
    int correlation_samples_ = 0;

    // Integrated LUFS
    double integrated_sum_ = 0.0;
    int integrated_blocks_ = 0;

    MultiChannelMeterData snapshot_;
};

using MultiChannelMeter = MultiChannelMeterT<float>;
using MultiChannelMeter64 = MultiChannelMeterT<double>;

} // namespace pulp::signal

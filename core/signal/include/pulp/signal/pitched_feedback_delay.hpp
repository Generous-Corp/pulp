#pragma once

/// @file pitched_feedback_delay.hpp
/// Delay with an optional latency-bearing processor inside the feedback
/// loop (pitch/formant shifters and similar), tempo sync, bounded
/// feedback, and a freeze-aware feedback gate.
///
/// Topology (per channel):
///
///   loop_in[n] = in[n] + feedback * gate * wet[n]
///   wet[n]     = P(ring[n - (D - L_P)])        // P = loop processor
///   out[n]     = wet[n]
///
/// where D is the requested delay and L_P the loop processor's reported
/// latency, so the effective echo spacing is exactly D and the minimum
/// legal delay is `min_delay_samples()` = L_P + 1 — computed from the
/// inserted processor at runtime, never hardcoded from any reference
/// product. Stability comes from clamping |feedback| < 1 (standard
/// feedback-comb bound: J. O. Smith III, *Physical Audio Signal
/// Processing*, W3K; Zölzer (ed.), *DAFX*, delay-effects chapter).
///
/// When the loop processor reports frozen, the feedback injection gate
/// ramps to zero (equal-power, ~50 ms) so frozen sustain does not
/// recirculate, and ramps back on release.
///
/// Delay changes are smoothed (one-pole on the read offset) to avoid
/// clicks. Feedback recursion shorter than a host block is handled by
/// internal sub-chunking down to single samples, so any D >= L_P + 1 is
/// honored exactly. No allocation or locks after prepare().

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// Interface for a processor placed inside the feedback loop.
template <typename SampleType = float>
class FeedbackLoopProcessorT {
public:
    virtual ~FeedbackLoopProcessorT() = default;
    virtual int loop_latency_samples() const = 0;
    virtual bool loop_is_frozen() const { return false; }
    /// Equal-length, RT-safe block processing.
    virtual void loop_process(const SampleType* const* in, SampleType* const* out,
                              int num_samples) = 0;
};

using FeedbackLoopProcessor = FeedbackLoopProcessorT<float>;
using FeedbackLoopProcessor64 = FeedbackLoopProcessorT<double>;

template <typename SampleType = float>
class PitchedFeedbackDelayT {
public:
    struct Config {
        float max_delay_seconds = 4.0f;
        int channels = 1;
        int max_block = 4096;
    };

    /// RT contract: prepare() allocates ring/chunk storage and is not
    /// audio-thread safe. After prepare(), reset(), setters, min_delay_samples(),
    /// and process() are allocation-free for blocks no larger than
    /// Config::max_block. The loop processor callback must also be RT-safe.
    void prepare(double sample_rate, const Config& config) {
        assert(sample_rate > 0.0 && config.channels >= 1);
        config_ = config;
        sample_rate_ = sample_rate;
        ring_size_ = 1;
        while (ring_size_ < static_cast<int>(config.max_delay_seconds * sample_rate) + 8)
            ring_size_ <<= 1;
        ring_mask_ = ring_size_ - 1;
        ring_.assign(static_cast<size_t>(config.channels) * ring_size_, SampleType{0});
        chunk_in_.assign(static_cast<size_t>(config.channels) * kMaxChunk, SampleType{0});
        chunk_out_.assign(static_cast<size_t>(config.channels) * kMaxChunk, SampleType{0});
        in_ptrs_.resize(static_cast<size_t>(config.channels));
        out_ptrs_.resize(static_cast<size_t>(config.channels));
        // ~50 ms gate and ~30 ms delay-glide one-pole coefficients.
        gate_coeff_ = std::exp(-1.0 / (0.05 * sample_rate));
        delay_coeff_ = std::exp(-1.0 / (0.03 * sample_rate));
        reset();
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), SampleType{0});
        write_pos_ = 0;
        delay_current_ = delay_target_;
        gate_ = 1.0;
    }

    void set_delay_ms(float ms) {
        delay_target_ = std::clamp(static_cast<double>(ms) * 0.001 * sample_rate_,
                                   static_cast<double>(min_delay_samples()),
                                   static_cast<double>(ring_size_ - 4));
        // Before the stream starts there is nothing to glide from.
        if (write_pos_ == 0) delay_current_ = delay_target_;
    }

    /// Tempo-synced delay: `beats` at `tempo_bpm` (e.g. 0.5 = eighth note).
    void set_delay_sync(double tempo_bpm, double beats) {
        assert(tempo_bpm > 0.0 && beats > 0.0);
        set_delay_ms(static_cast<float>(beats * 60000.0 / tempo_bpm));
    }

    /// Clamped below unity for stability.
    void set_feedback(float feedback) {
        feedback_ = std::clamp(static_cast<SampleType>(feedback),
                               SampleType{0}, static_cast<SampleType>(0.99));
    }

    void set_loop_processor(FeedbackLoopProcessorT<SampleType>* processor) {
        loop_ = processor;
        // Re-apply the floor in case the new processor's latency raised it.
        delay_target_ = std::max(delay_target_, static_cast<double>(min_delay_samples()));
    }

    /// Minimum legal delay: the in-loop processor's latency plus one.
    int min_delay_samples() const {
        return (loop_ != nullptr ? loop_->loop_latency_samples() : 0) + 1;
    }

    void process(const SampleType* const* in, SampleType* const* out, int num_samples) {
        assert(num_samples <= config_.max_block);
        int done = 0;
        while (done < num_samples) {
            // Sub-chunk below the loop's recursion length so feedback
            // written in this chunk is never read within it.
            const int recursion = std::max(
                1, static_cast<int>(delay_current_) - min_delay_samples() + 1);
            const int chunk =
                std::min({num_samples - done, recursion, kMaxChunk});
            process_chunk(in, out, done, chunk);
            done += chunk;
        }
    }

private:
    void process_chunk(const SampleType* const* in, SampleType* const* out, int offset, int n) {
        const bool frozen = loop_ != nullptr && loop_->loop_is_frozen();
        const double gate_target = frozen ? 0.0 : 1.0;
        const int read_latency = loop_ != nullptr ? loop_->loop_latency_samples() : 0;

        // Read the delayed (pre-processor) signal into chunk_in_.
        for (int ch = 0; ch < config_.channels; ++ch) {
            in_ptrs_[static_cast<size_t>(ch)] =
                chunk_in_.data() + static_cast<size_t>(ch) * kMaxChunk;
            out_ptrs_[static_cast<size_t>(ch)] =
                chunk_out_.data() + static_cast<size_t>(ch) * kMaxChunk;
        }
        double d = delay_current_;
        for (int i = 0; i < n; ++i) {
            d += (1.0 - delay_coeff_) * (delay_target_ - d);
            const double read_at = static_cast<double>(write_pos_ + i)
                                   - (d - static_cast<double>(read_latency));
            const auto i0 = static_cast<std::int64_t>(std::floor(read_at));
            const SampleType frac = static_cast<SampleType>(read_at - static_cast<double>(i0));
            for (int ch = 0; ch < config_.channels; ++ch) {
                const SampleType* ring = ring_.data() + static_cast<size_t>(ch) * ring_size_;
                const SampleType a = ring[static_cast<size_t>(i0 & ring_mask_)];
                const SampleType b = ring[static_cast<size_t>((i0 + 1) & ring_mask_)];
                in_ptrs_[static_cast<size_t>(ch)][i] = a + frac * (b - a);
            }
        }
        delay_current_ = d;

        // Optional in-loop processor (adds back read_latency).
        if (loop_ != nullptr) {
            loop_->loop_process(
                const_cast<const SampleType* const*>(in_ptrs_.data()),
                out_ptrs_.data(), n);
        } else {
            for (int ch = 0; ch < config_.channels; ++ch)
                std::copy(in_ptrs_[static_cast<size_t>(ch)],
                          in_ptrs_[static_cast<size_t>(ch)] + n,
                          out_ptrs_[static_cast<size_t>(ch)]);
        }

        // Write input + gated feedback; emit the wet signal.
        for (int i = 0; i < n; ++i) {
            gate_ += (1.0 - gate_coeff_) * (gate_target - gate_);
            const SampleType g = feedback_ * static_cast<SampleType>(
                std::sin(gate_ * 1.5707963));
            for (int ch = 0; ch < config_.channels; ++ch) {
                const SampleType wet = out_ptrs_[static_cast<size_t>(ch)][i];
                SampleType* ring = ring_.data() + static_cast<size_t>(ch) * ring_size_;
                ring[static_cast<size_t>((write_pos_ + i) & ring_mask_)] =
                    in[ch][offset + i] + g * wet;
                out[ch][offset + i] = wet;
            }
        }
        write_pos_ += n;
    }

    static constexpr int kMaxChunk = 256;

    Config config_;
    double sample_rate_ = 48000.0;
    int ring_size_ = 0;
    int ring_mask_ = 0;

    std::vector<SampleType> ring_;
    std::vector<SampleType> chunk_in_;
    std::vector<SampleType> chunk_out_;
    std::vector<SampleType*> in_ptrs_;
    std::vector<SampleType*> out_ptrs_;

    FeedbackLoopProcessorT<SampleType>* loop_ = nullptr;
    SampleType feedback_ = SampleType{0};
    double delay_target_ = 24000.0;
    double delay_current_ = 24000.0;
    double gate_ = 1.0;
    double gate_coeff_ = 0.0;
    double delay_coeff_ = 0.0;
    std::int64_t write_pos_ = 0;
};

using PitchedFeedbackDelay = PitchedFeedbackDelayT<float>;
using PitchedFeedbackDelay64 = PitchedFeedbackDelayT<double>;

} // namespace pulp::signal

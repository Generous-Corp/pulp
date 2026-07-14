#pragma once

/// @file polyphase_block_oversampler.hpp
/// Block-oriented, multi-channel half-band polyphase IIR oversampler that
/// exposes the intermediate oversampled buffer between the up and down
/// passes.
///
/// `OversamplerT` (oversampling.hpp) is fused and single-channel: one call
/// upsamples, invokes a per-sample callback N times, and decimates, with no
/// way to see the oversampled samples in between. That is the right shape
/// for a single independent channel of DSP, but it cannot express work that
/// needs to look across channels at the SAME oversampled sub-index — for
/// example a dither draw shared by L/R so both channels quantize against
/// the same random value, or a stereo-linked compressor whose envelope
/// follower is driven by `0.5*(|L| + |R|)` computed once per oversampled
/// sample rather than twice independently. Two independent `OversamplerT`
/// instances cannot do this: each one runs its whole N-sample loop to
/// completion before returning, so channel R's oversampled sample at index
/// i does not exist yet while channel L's callback for index i is running.
///
/// `PolyphaseBlockOversamplerT` instead splits the fused call into
/// `process_up()` / `process_down()`, both operating on all channels at
/// once and both taking/returning plain buffers instead of a callback.
/// Callers do their joint-channel work directly on the `BlockView` returned
/// by `process_up()`, indexed by oversampled sample and channel, then call
/// `process_down()` to decimate back. This is deliberately a sibling class
/// rather than new methods on `OversamplerT`: `OversamplerT`'s fused,
/// allocation-free-per-sample callback API is worth keeping intact for the
/// (more common) single-channel case, and folding both shapes into one
/// class would mean every caller — including existing `OversamplerT`
/// users — carries API surface for a use case most of them don't have.
///
/// The filter family is selected at compile time via the `UpStage` /
/// `DownStage` template template-parameters, defaulting to the
/// `polyphase_iir` design (`HalfBandUpsampler2xT` / `HalfBandDownsampler2xT`,
/// halfband_iir.hpp). Passing `EllipticHalfBandUpsampler2xT` /
/// `EllipticHalfBandDownsampler2xT` (elliptic_halfband_iir.hpp) instead
/// selects the Valenzuela-Constantinides elliptic design — see
/// `EllipticPolyphaseBlockOversamplerT` below. Both stage types are
/// default-constructible with a fixed, reasonable single-stage design, so
/// every cascade stage reuses that same design regardless of stage index —
/// the same "one fixed design at every stage" approach `OversamplerT`
/// itself already uses for `Kind::polyphase_iir`. Callers who need a
/// per-stage-relaxed schedule instead (as `OversamplerT`'s
/// `elliptic_polyphase_iir` `Kind` provides for the fused, single-channel
/// case) should configure `stage(n).up(channel)` / `stage(n).down(channel)`
/// directly after `prepare()`.
///
/// RT contract: `prepare()` allocates every stage's per-channel filter
/// state and scratch buffer. `reset()`, `process_up()`, and
/// `process_down()` allocate no memory once prepared.

#include <pulp/signal/elliptic_halfband_iir.hpp>
#include <pulp/signal/halfband_iir.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace pulp::signal {

/// A non-owning view over the oversampled buffer `process_up()` fills in —
/// `num_channels` channels of `num_samples` samples each, at the oversampled
/// rate. Valid until the next `process_up()` / `process_down()` call.
template <typename SampleType> struct OversampledBlockView {
    SampleType* const* channels = nullptr;
    std::size_t num_channels = 0;
    std::size_t num_samples = 0;

    std::size_t get_num_samples() const {
        return num_samples;
    }
    SampleType* channel_pointer(std::size_t channel) const {
        return channels[channel];
    }
};

template <typename SampleType = float, template <typename> class UpStage = HalfBandUpsampler2xT,
         template <typename> class DownStage = HalfBandDownsampler2xT>
class PolyphaseBlockOversamplerT {
  public:
    using View = OversampledBlockView<SampleType>;

    /// One 2x cascade level, holding one filter pair and one scratch buffer
    /// per channel. The up pass fills the buffer (2x the stage's input
    /// length); the down pass reads it and decimates.
    class Stage {
      public:
        void prepare(std::size_t num_channels, std::size_t max_input_samples) {
            up_.assign(num_channels, UpStage<SampleType>{});
            down_.assign(num_channels, DownStage<SampleType>{});
            buffer_.assign(num_channels, std::vector<SampleType>(max_input_samples * 2, SampleType{0}));
            pointers_.assign(num_channels, nullptr);
        }

        void reset() {
            for (auto& stage : up_)
                stage.reset();
            for (auto& stage : down_)
                stage.reset();
            for (auto& channel : buffer_)
                std::fill(channel.begin(), channel.end(), SampleType{0});
        }

        /// Direct access to this stage's per-channel filters, for callers
        /// that want to reconfigure a design that supports `configure()`
        /// (e.g. `EllipticHalfBandUpsampler2xT`) with a non-default
        /// transition width / stopband floor after `prepare()`.
        UpStage<SampleType>& up(std::size_t channel) {
            return up_[channel];
        }
        DownStage<SampleType>& down(std::size_t channel) {
            return down_[channel];
        }

        void process_up(const SampleType* const* input, std::size_t num_channels,
                        std::size_t num_samples) {
            for (std::size_t channel = 0; channel < num_channels; ++channel) {
                SampleType* out = buffer_[channel].data();
                const SampleType* in = input[channel];
                for (std::size_t i = 0; i < num_samples; ++i)
                    up_[channel].process(in[i], out[2 * i], out[2 * i + 1]);
            }
        }

        void process_down(SampleType* const* output, std::size_t num_channels,
                          std::size_t num_samples) {
            for (std::size_t channel = 0; channel < num_channels; ++channel) {
                const SampleType* in = buffer_[channel].data();
                SampleType* out = output[channel];
                for (std::size_t i = 0; i < num_samples; ++i)
                    out[i] = down_[channel].process(in[2 * i], in[2 * i + 1]);
            }
        }

        SampleType* const* channel_pointers(std::size_t num_channels) {
            for (std::size_t channel = 0; channel < num_channels; ++channel)
                pointers_[channel] = buffer_[channel].data();
            return pointers_.data();
        }

      private:
        std::vector<UpStage<SampleType>> up_;
        std::vector<DownStage<SampleType>> down_;
        std::vector<std::vector<SampleType>> buffer_; // [channel][2 * stage input length]
        std::vector<SampleType*> pointers_;
    };

    /// Ceiling on `num_stages`: `oversampling_factor()` returns `1 <<
    /// stages_.size()` as a plain `int`, so anything at or past the width of
    /// `int` is undefined behaviour. This cap sits far below that — no real
    /// oversampling factor is anywhere near `2^24` — purely to turn a
    /// caller's garbage stage count into a clamp instead of UB.
    static constexpr int kMaxStages = 24;

    /// Ceiling on `max_block_size` (and, by extension, on any `num_samples`
    /// `process_up()` is called with): the highest stage's scratch buffer is
    /// `max_block_size * 2^num_stages`, doubled once more inside
    /// `Stage::prepare()`. Bounding `max_block_size` so that growth can't
    /// overflow `std::size_t` even at `kMaxStages` keeps every subsequent
    /// plain `* 2` in `prepare()` / `process_up()` / `process_down()` exact,
    /// instead of scattering a clamp into each doubling.
    static constexpr std::size_t kMaxBlockSize = std::numeric_limits<std::size_t>::max() >>
                                                 (kMaxStages + 1);

    /// `num_stages` is the number of 2x cascade levels (ratio = 2^num_stages) —
    /// it counts *stages*, not the ratio, distinct from
    /// `OversamplerT::Factor`, which encodes the ratio directly.
    /// `num_stages` is clamped to `[0, kMaxStages]` (a negative count would
    /// otherwise wrap to a huge `size_t` below) and `max_block_size` to
    /// `kMaxBlockSize`, so the repeated `* 2` that sizes each stage's scratch
    /// buffer can never overflow `size_t` and undersize it.
    void prepare(std::size_t num_channels, int num_stages, std::size_t max_block_size) {
        num_channels_ = num_channels;
        const auto clamped_stages =
            static_cast<std::size_t>(std::clamp(num_stages, 0, kMaxStages));
        stages_.assign(clamped_stages, Stage{});
        max_block_size_ = std::min(max_block_size, kMaxBlockSize);
        std::size_t stage_input_samples = max_block_size_;
        for (auto& stage : stages_) {
            stage.prepare(num_channels, stage_input_samples);
            stage_input_samples *= 2;
        }
        passthrough_buffer_.assign(num_channels, std::vector<SampleType>(max_block_size_, SampleType{0}));
        passthrough_pointers_.assign(num_channels, nullptr);
    }

    void reset() {
        for (auto& stage : stages_)
            stage.reset();
    }

    /// Number of 2x cascade stages configured by `prepare()`.
    std::size_t num_stages() const {
        return stages_.size();
    }
    /// The overall ratio, `2^num_stages()`.
    int oversampling_factor() const {
        return 1 << stages_.size();
    }

    /// Direct access to stage `n`, e.g. to reconfigure an elliptic design's
    /// transition width per stage. `n` must be `< num_stages()`.
    Stage& stage(std::size_t n) {
        return stages_[n];
    }

    /// Upsample `input` (num_channels x num_samples) through every cascade
    /// stage and return a view over the final, highest-rate stage buffer —
    /// num_channels x (num_samples * oversampling_factor()). The view stays
    /// valid, and may be freely read AND written, until the matching
    /// `process_down()` call (or the next `process_up()`).
    ///
    /// `num_channels` / `num_samples` are clamped to what `prepare()`
    /// configured: every per-channel scratch buffer is sized from
    /// `prepare()`'s `num_channels` / `max_block_size`, so trusting a larger
    /// caller-supplied count here would index and write past it.
    View process_up(const SampleType* const* input, std::size_t num_channels,
                    std::size_t num_samples) {
        num_channels = std::min(num_channels, num_channels_);
        num_samples = std::min(num_samples, max_block_size_);

        if (stages_.empty()) {
            // No cascade stage owns a buffer to expose, but the contract
            // promises a *writable* view a caller can edit jointly across
            // channels before process_down() reads it back — aliasing
            // `input` directly would mean writing through a pointer the
            // caller may have handed us as genuinely const, and silently
            // drop any edit if `output` isn't literally the same buffer as
            // `input`. The owned passthrough scratch makes both round-trip
            // regardless.
            for (std::size_t channel = 0; channel < num_channels; ++channel) {
                std::copy_n(input[channel], num_samples, passthrough_buffer_[channel].data());
                passthrough_pointers_[channel] = passthrough_buffer_[channel].data();
            }
            return {passthrough_pointers_.data(), num_channels, num_samples};
        }

        stages_.front().process_up(input, num_channels, num_samples);
        std::size_t block_samples = num_samples * 2;
        for (std::size_t i = 1; i < stages_.size(); ++i) {
            SampleType* const* previous = stages_[i - 1].channel_pointers(num_channels);
            stages_[i].process_up(previous, num_channels, block_samples);
            block_samples *= 2;
        }
        SampleType* const* last = stages_.back().channel_pointers(num_channels);
        return {last, num_channels, block_samples};
    }

    /// Decimate the (possibly caller-modified) oversampled buffer back down
    /// through every cascade stage into `output` (num_channels x
    /// num_samples). Must follow a `process_up()` call with the same
    /// `num_channels` / `num_samples`.
    void process_down(SampleType* const* output, std::size_t num_channels,
                      std::size_t num_samples) {
        num_channels = std::min(num_channels, num_channels_);
        num_samples = std::min(num_samples, max_block_size_);

        if (stages_.empty()) {
            for (std::size_t channel = 0; channel < num_channels; ++channel)
                std::copy_n(passthrough_buffer_[channel].data(), num_samples, output[channel]);
            return;
        }

        std::size_t current_samples = num_samples;
        for (std::size_t n = 0; n + 1 < stages_.size(); ++n)
            current_samples *= 2;

        for (std::size_t n = stages_.size() - 1; n > 0; --n) {
            SampleType* const* destination = stages_[n - 1].channel_pointers(num_channels);
            stages_[n].process_down(destination, num_channels, current_samples);
            current_samples /= 2;
        }
        stages_.front().process_down(output, num_channels, num_samples);
    }

  private:
    std::vector<Stage> stages_;
    std::size_t num_channels_ = 0;
    std::size_t max_block_size_ = 0;
    std::vector<std::vector<SampleType>> passthrough_buffer_;
    std::vector<SampleType*> passthrough_pointers_;
};

using PolyphaseBlockOversampler = PolyphaseBlockOversamplerT<float>;
using PolyphaseBlockOversampler64 = PolyphaseBlockOversamplerT<double>;

/// The elliptic-design sibling: same block API, Valenzuela-Constantinides
/// elliptic half-band filter (see elliptic_halfband_iir.hpp).
using EllipticPolyphaseBlockOversampler =
    PolyphaseBlockOversamplerT<float, EllipticHalfBandUpsampler2xT, EllipticHalfBandDownsampler2xT>;
using EllipticPolyphaseBlockOversampler64 =
    PolyphaseBlockOversamplerT<double, EllipticHalfBandUpsampler2xT, EllipticHalfBandDownsampler2xT>;

} // namespace pulp::signal

#pragma once

/// @file spectral_mask_processor.hpp
/// Streaming spectral-mask processor with race-free, frame-boundary table
/// publication and latency-aligned dry/wet mixing.
///
/// Control code compiles a `SpectralMaskTableT` directly or publishes a
/// `SpectralBandLayoutT` through `publish_layout()`. The audio owner adopts
/// only the latest complete table at a spectral-frame boundary and interpolates
/// from its current gain curve over the table's requested transition length.
/// No allocation, locks, or table compilation occur in `process()` or
/// `process_frame()` after a successful `prepare()`.

#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/signal/checked_allocation.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
#include <pulp/signal/spectral_band_mask.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace pulp::signal {

inline constexpr std::uint64_t kSpectralMaskProcessorDefaultMaximumRetainedBytes =
    std::uint64_t{256} << 20u;

template <typename SampleType>
struct SpectralMaskProcessorConfigT {
    static_assert(std::is_floating_point_v<SampleType>);

    SpectralFrameEngineConfig frame{};
    SampleType sample_rate = SampleType{48000};
    SampleType initial_mix = SampleType{1};
    int mix_ramp_samples = 0;
    MixCurve mix_curve = MixCurve::Linear;
    std::uint64_t max_retained_bytes =
        kSpectralMaskProcessorDefaultMaximumRetainedBytes;
};

template <typename SampleType = float>
class SpectralMaskProcessorT {
public:
    static_assert(std::is_floating_point_v<SampleType>);

    using Config = SpectralMaskProcessorConfigT<SampleType>;
    using Layout = SpectralBandLayoutT<SampleType>;
    using Table = SpectralMaskTableT<SampleType>;

    /// Prepare a complete replacement state. Failure leaves the prior prepared
    /// state intact. This control-thread operation allocates the STFT and dry
    /// delay storage; all later audio methods stay within those capacities.
    [[nodiscard]] bool prepare(const Config& config) {
        if (!std::isfinite(config.sample_rate)
            || config.sample_rate <= SampleType{0}
            || !std::isfinite(config.initial_mix)
            || config.initial_mix < SampleType{0}
            || config.initial_mix > SampleType{1}
            || config.mix_ramp_samples < 0
            || !valid_mix_curve_(config.mix_curve))
            return false;

        const auto frame_geometry =
            checked_spectral_frame_engine_geometry<SampleType>(config.frame);
        if (!frame_geometry) return false;

        const auto retained = checked_retained_bytes_(config, *frame_geometry);
        if (!retained || *retained > config.max_retained_bytes) return false;

        auto candidate = std::make_unique<PreparedState>();
        candidate->config = config;
        candidate->retained_bytes = *retained;
        candidate->engine.prepare(config.frame);

        Table identity{};
        identity.fft_size = config.frame.fft_size;
        identity.num_bins = frame_geometry->num_bins;
        identity.sample_rate = config.sample_rate;
        identity.effective_min_hz = SampleType{0};
        identity.effective_max_hz = config.sample_rate * SampleType{0.5};
        identity.active_bands = 1;
        identity.band_edges_hz[0] = SampleType{0};
        identity.band_edges_hz[1] = identity.effective_max_hz;
        std::fill_n(identity.gain_linear.begin(), identity.num_bins,
                    SampleType{1});

        candidate->frame_table = identity;
        candidate->target_table = identity;
        candidate->publication =
            std::make_unique<runtime::TripleBuffer<PublishedTable>>(
                PublishedTable{identity, 0});
        candidate->mixer.set_mix(config.initial_mix);
        candidate->mixer.set_curve(config.mix_curve);
        candidate->mixer.set_ramp_samples(config.mix_ramp_samples);
        candidate->mixer.set_wet_latency(candidate->engine.latency_samples());
        candidate->mixer.prepare(config.frame.channels, config.frame.max_block);

        state_ = std::move(candidate);
        next_generation_ = 1;
        return true;
    }

    /// Compile and publish one layout from the prepared geometry. Compilation
    /// and publication are failure-atomic control-thread operations.
    [[nodiscard]] bool publish_layout(const Layout& layout) {
        if (!state_) return false;
        Table table;
        if (!build_spectral_mask(layout, state_->config.frame.fft_size,
                                 state_->config.sample_rate, table))
            return false;
        return publish_table(table);
    }

    /// Publish a complete immutable table through the shared latest-value
    /// three-slot handoff. One control-side writer and one audio-side reader
    /// may operate concurrently.
    [[nodiscard]] bool publish_table(const Table& table) {
        if (!state_ || !valid_table_(table)) return false;
        auto generation = next_generation_++;
        if (generation == 0) generation = next_generation_++;
        state_->publication->write_with([&](PublishedTable& payload) {
            payload.table = table;
            payload.generation = generation;
        });
        state_->published_generation.write(generation);
        return true;
    }

    /// Process one prepared planar block. The dry path is delayed by the exact
    /// WOLA latency before mixing, so any mix below 100% cannot leak an early
    /// unfiltered signal around an isolation mask.
    [[nodiscard]] bool process(const SampleType* const* input,
                               SampleType* const* output,
                               int num_samples) noexcept {
        if (!valid_audio_block_(input, output, num_samples)) {
            zero_output_(output, num_samples);
            return false;
        }

        state_->mixer.push_dry(input, state_->config.frame.channels, num_samples);
        bool frame_ok = true;
        state_->engine.process(input, output, num_samples,
            [this, &frame_ok](std::complex<SampleType>* const* frames,
                              int bins) noexcept {
                if (!process_frame(frames, bins)) frame_ok = false;
            });
        if (!frame_ok) {
            zero_output_(output, num_samples);
            return false;
        }
        state_->mixer.mix_wet(output, state_->config.frame.channels, num_samples);
        return true;
    }

    /// Apply the current publication/transition state to one coherent complex
    /// frame group. This narrow source-independent seam is also usable by a
    /// prepared hold, freeze, delay, or resynthesis owner; it must still be
    /// called by exactly one audio-side consumer in chronological frame order.
    [[nodiscard]] bool process_frame(std::complex<SampleType>* const* frames,
                                     int num_bins) noexcept {
        if (!state_ || num_bins != state_->frame_table.num_bins) {
            zero_frames_(frames, num_bins);
            return false;
        }

        adopt_latest_table_();
        advance_transition_();
        if (!apply_spectral_mask(frames, state_->config.frame.channels,
                                 num_bins, state_->frame_table)) {
            zero_frames_(frames, num_bins);
            return false;
        }
        return true;
    }

    /// Audio-thread-safe dry/wet controls. Mix defaults to fully wet.
    void set_mix(SampleType mix) noexcept {
        if (state_ && std::isfinite(mix)) state_->mixer.set_mix(mix);
    }
    void set_mix_ramp_samples(int samples) noexcept {
        if (state_) state_->mixer.set_ramp_samples(samples);
    }
    void set_mix_curve(MixCurve curve) noexcept {
        if (state_ && valid_mix_curve_(curve)) state_->mixer.set_curve(curve);
    }

    /// Clear analysis/synthesis, dry-delay, and in-flight interpolation state
    /// while preserving the latest adopted mask as the new settled curve.
    void reset() noexcept {
        if (!state_) return;
        state_->engine.reset();
        state_->mixer.reset();
        state_->frame_table = state_->target_table;
        state_->transition_total = 0;
        state_->transition_position = 0;
        state_->target_table = state_->frame_table;
    }

    [[nodiscard]] bool prepared() const noexcept { return state_ != nullptr; }
    [[nodiscard]] int latency_samples() const noexcept {
        return state_ ? state_->engine.latency_samples() : 0;
    }
    /// Conservative flush bound for the fixed-latency WOLA response.
    [[nodiscard]] int maximum_tail_samples() const noexcept {
        return state_ ? latency_samples() + state_->config.frame.fft_size : 0;
    }
    [[nodiscard]] int num_bins() const noexcept {
        return state_ ? state_->frame_table.num_bins : 0;
    }
    [[nodiscard]] int channels() const noexcept {
        return state_ ? state_->config.frame.channels : 0;
    }
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept {
        return state_ ? state_->retained_bytes : 0;
    }
    [[nodiscard]] std::uint64_t active_table_version() const noexcept {
        return state_ ? state_->active_version.read() : 0;
    }
    [[nodiscard]] bool table_publication_pending() const noexcept {
        return state_
            && state_->published_generation.read()
                != state_->active_generation.read();
    }

private:
    struct PublishedTable {
        Table table{};
        std::uint64_t generation = 0;
    };

    struct PreparedState {
        Config config{};
        SpectralFrameEngineT<SampleType> engine;
        DryWetMixerT<SampleType> mixer;
        std::unique_ptr<runtime::TripleBuffer<PublishedTable>> publication;
        Table frame_table{};
        Table target_table{};
        std::array<SampleType, kSpectralBandMaskMaximumBins> transition_start{};
        runtime::SeqLock<std::uint64_t> published_generation;
        runtime::SeqLock<std::uint64_t> active_generation;
        runtime::SeqLock<std::uint64_t> active_version;
        std::uint64_t consumed_generation = 0;
        std::uint64_t retained_bytes = 0;
        std::uint32_t transition_total = 0;
        std::uint32_t transition_position = 0;
    };

    static std::optional<std::uint64_t> checked_retained_bytes_(
        const Config& config,
        const SpectralFrameEngineGeometry& frame) noexcept {
        const auto channels = static_cast<std::uint64_t>(config.frame.channels);
        const auto latency = static_cast<std::uint64_t>(config.frame.fft_size)
                           + static_cast<std::uint64_t>(config.frame.analysis_hop);
        std::uint64_t samples_per_channel = 0;
        std::uint64_t dry_samples = 0;
        if (!checked_capacity_sum(latency,
                                  static_cast<std::uint64_t>(config.frame.max_block),
                                  config.max_retained_bytes, samples_per_channel)
            || !checked_capacity_product(samples_per_channel, channels,
                                         config.max_retained_bytes, dry_samples))
            return std::nullopt;

        CheckedRetainedByteCharge charge(config.max_retained_bytes);
        if (!charge.add_retained_bytes(frame.retained_bytes)
            || !charge.add_retained_bytes(sizeof(PreparedState))
            || !charge.add_retained_bytes(
                   sizeof(runtime::TripleBuffer<PublishedTable>))
            || !charge.add<SampleType>(dry_samples)
            || !charge.add<std::vector<SampleType>>(channels)
            || charge.total() > config.max_retained_bytes)
            return std::nullopt;
        return charge.total();
    }

    static bool valid_mix_curve_(MixCurve curve) noexcept {
        switch (curve) {
            case MixCurve::Linear:
            case MixCurve::EqualPower:
            case MixCurve::Balanced:
            case MixCurve::Sin3dB:
            case MixCurve::Sin4_5dB:
            case MixCurve::Sin6dB:
            case MixCurve::Sqrt3dB:
            case MixCurve::Sqrt4_5dB:
                return true;
        }
        return false;
    }

    bool valid_table_(const Table& table) const noexcept {
        if (table.fft_size != state_->config.frame.fft_size
            || table.num_bins != state_->frame_table.num_bins
            || table.sample_rate != state_->config.sample_rate
            || table.transition_frames
                > kSpectralBandMaskMaximumTransitionFrames)
            return false;
        for (int bin = 0; bin < table.num_bins; ++bin) {
            const auto gain = table.gain_linear[static_cast<std::size_t>(bin)];
            if (!std::isfinite(gain) || gain < SampleType{0}) return false;
        }
        return true;
    }

    bool valid_audio_block_(const SampleType* const* input,
                            SampleType* const* output,
                            int num_samples) const noexcept {
        if (!state_ || !input || !output || num_samples <= 0
            || num_samples > state_->config.frame.max_block)
            return false;
        for (int channel = 0; channel < state_->config.frame.channels; ++channel)
            if (!input[channel] || !output[channel]) return false;
        return true;
    }

    void adopt_latest_table_() noexcept {
        const auto& publication = state_->publication->read();
        if (publication.generation == state_->consumed_generation) return;

        state_->consumed_generation = publication.generation;
        state_->active_generation.write(publication.generation);
        state_->active_version.write(publication.table.version);
        std::copy_n(state_->frame_table.gain_linear.begin(),
                    state_->frame_table.num_bins,
                    state_->transition_start.begin());
        state_->target_table = publication.table;
        state_->transition_total = publication.table.transition_frames;
        state_->transition_position = 0;
        if (state_->transition_total == 0) state_->frame_table = publication.table;
    }

    void advance_transition_() noexcept {
        if (state_->transition_position >= state_->transition_total) return;
        ++state_->transition_position;
        const auto mix = static_cast<SampleType>(state_->transition_position)
                       / static_cast<SampleType>(state_->transition_total);
        state_->frame_table = state_->target_table;
        for (int bin = 0; bin < state_->frame_table.num_bins; ++bin) {
            const auto index = static_cast<std::size_t>(bin);
            state_->frame_table.gain_linear[index] =
                state_->transition_start[index]
                + (state_->target_table.gain_linear[index]
                   - state_->transition_start[index]) * mix;
        }
    }

    void zero_output_(SampleType* const* output, int num_samples) noexcept {
        if (!state_ || !output || num_samples <= 0) return;
        const auto samples = std::min(num_samples, state_->config.frame.max_block);
        for (int channel = 0; channel < state_->config.frame.channels; ++channel)
            if (output[channel]) std::fill_n(output[channel], samples, SampleType{});
    }

    void zero_frames_(std::complex<SampleType>* const* frames,
                      int num_bins) noexcept {
        if (!state_ || !frames || num_bins <= 0) return;
        const auto bins = std::min<std::size_t>(
            static_cast<std::size_t>(num_bins), kSpectralBandMaskMaximumBins);
        for (int channel = 0; channel < state_->config.frame.channels; ++channel)
            if (frames[channel]) std::fill_n(frames[channel], bins,
                                             std::complex<SampleType>{});
    }

    std::unique_ptr<PreparedState> state_;
    std::uint64_t next_generation_ = 1;
};

using SpectralMaskProcessorConfig = SpectralMaskProcessorConfigT<float>;
using SpectralMaskProcessorConfig64 = SpectralMaskProcessorConfigT<double>;
using SpectralMaskProcessor = SpectralMaskProcessorT<float>;
using SpectralMaskProcessor64 = SpectralMaskProcessorT<double>;

} // namespace pulp::signal

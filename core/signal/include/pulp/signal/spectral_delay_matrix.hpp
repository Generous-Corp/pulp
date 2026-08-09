#pragma once

/// @file spectral_delay_matrix.hpp
/// Prepared, frame-quantized spectral delay built on SpectralFrameEngineT.
///
/// RT contract: prepare(), compile_table(), and publish_table() are control-thread
/// operations. After prepare(), process(), reset(), and purge() allocate no memory,
/// take no locks, perform no I/O, and throw no exceptions. Complete tables cross
/// to the audio thread through a three-slot latest-value publication and are
/// adopted only between spectral frames.

#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/signal/checked_allocation.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::signal {

inline constexpr std::size_t kSpectralDelayMinimumRetainedBytes = std::size_t{1} << 20u;
inline constexpr std::size_t kSpectralDelayMaximumRetainedBytes = std::size_t{1} << 29u;

struct SpectralDelayBreakpoint {
    float freq01 = 0.0f;
    float delay_ms = 0.0f;
    float attenuation_db = 0.0f;
};

struct SpectralDelayMatrixRecipe {
    std::uint64_t version = 0;
    std::vector<SpectralDelayBreakpoint> breakpoints;
};

struct SpectralDelayMatrixTable {
    std::uint64_t version = 0;
    int num_bins = 0;
    std::vector<std::int32_t> delay_frames;
    std::vector<float> attenuation_linear;
};

struct SpectralDelayMatrixConfig {
    SpectralFrameEngineConfig frame{};
    double sample_rate = 48000.0;
    double max_delay_ms = 2000.0;
    std::size_t max_retained_bytes = std::size_t{1} << 26u;
};

template <typename SampleType> struct SpectralDelayMatrixGeometry {
    int num_bins = 0;
    int history_frames = 0;
    std::uint64_t retained_bytes = 0;
};

template <typename SampleType>
inline std::optional<SpectralDelayMatrixGeometry<SampleType>>
checked_spectral_delay_matrix_geometry(
    const SpectralDelayMatrixConfig& config,
    std::uint64_t target_max_bytes = kTargetAddressMaximumBytes) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    if (!std::isfinite(config.sample_rate) || config.sample_rate <= 0.0 ||
        !std::isfinite(config.max_delay_ms) || config.max_delay_ms < 0.0 ||
        config.max_delay_ms > 60000.0 ||
        config.max_retained_bytes < kSpectralDelayMinimumRetainedBytes ||
        config.max_retained_bytes > kSpectralDelayMaximumRetainedBytes)
        return std::nullopt;
    const auto engine =
        checked_spectral_frame_engine_geometry<SampleType>(config.frame, target_max_bytes);
    if (!engine)
        return std::nullopt;

    const double frames_exact =
        config.max_delay_ms * config.sample_rate / (1000.0 * config.frame.analysis_hop);
    if (!std::isfinite(frames_exact) ||
        frames_exact > static_cast<double>(std::numeric_limits<int>::max() - 1))
        return std::nullopt;
    const auto history_frames = static_cast<std::uint64_t>(std::floor(frames_exact)) + 1u;
    std::uint64_t channel_frames = 0;
    std::uint64_t history_elements = 0;
    if (!checked_capacity_product(static_cast<std::uint64_t>(config.frame.channels), history_frames,
                                  UINT64_MAX, channel_frames) ||
        !checked_capacity_product(channel_frames, static_cast<std::uint64_t>(engine->num_bins),
                                  UINT64_MAX, history_elements))
        return std::nullopt;

    CheckedRetainedByteCharge charge(target_max_bytes);
    if (!charge.add_retained_bytes(engine->retained_bytes) ||
        !charge.add<std::complex<SampleType>>(history_elements) ||
        !charge.add_repeated<std::int32_t>(static_cast<std::uint64_t>(engine->num_bins), 3) ||
        !charge.add_repeated<float>(static_cast<std::uint64_t>(engine->num_bins), 3))
        return std::nullopt;
    const auto retained = charge.total();
    if (retained > config.max_retained_bytes)
        return std::nullopt;
    return SpectralDelayMatrixGeometry<SampleType>{engine->num_bins,
                                                   static_cast<int>(history_frames), retained};
}

inline bool compile_spectral_delay_matrix_table(const SpectralDelayMatrixRecipe& recipe,
                                                int num_bins, int analysis_hop, double sample_rate,
                                                int maximum_delay_frames,
                                                SpectralDelayMatrixTable& out_table) {
    if (recipe.breakpoints.empty() || recipe.breakpoints.size() > 256 || num_bins < 2 ||
        analysis_hop <= 0 || maximum_delay_frames < 0 || !std::isfinite(sample_rate) ||
        sample_rate <= 0.0)
        return false;
    for (std::size_t i = 0; i < recipe.breakpoints.size(); ++i) {
        const auto& point = recipe.breakpoints[i];
        if (!std::isfinite(point.freq01) || !std::isfinite(point.delay_ms) ||
            !std::isfinite(point.attenuation_db) || point.freq01 < 0.0f || point.freq01 > 1.0f ||
            (i && !(recipe.breakpoints[i - 1].freq01 < point.freq01)))
            return false;
    }

    const double maximum_delay_ms =
        static_cast<double>(maximum_delay_frames) * 1000.0 * analysis_hop / sample_rate;
    SpectralDelayMatrixTable candidate;
    candidate.version = recipe.version;
    candidate.num_bins = num_bins;
    candidate.delay_frames.resize(static_cast<std::size_t>(num_bins));
    candidate.attenuation_linear.resize(static_cast<std::size_t>(num_bins));
    for (int bin = 0; bin < num_bins; ++bin) {
        const float freq01 = static_cast<float>(bin) / static_cast<float>(num_bins - 1);
        auto upper = std::upper_bound(
            recipe.breakpoints.begin(), recipe.breakpoints.end(), freq01,
            [](float value, const SpectralDelayBreakpoint& point) { return value < point.freq01; });
        const SpectralDelayBreakpoint* lo = nullptr;
        const SpectralDelayBreakpoint* hi = nullptr;
        if (upper == recipe.breakpoints.begin())
            lo = hi = &recipe.breakpoints.front();
        else if (upper == recipe.breakpoints.end())
            lo = hi = &recipe.breakpoints.back();
        else {
            hi = &*upper;
            lo = &*(upper - 1);
        }
        const double mix =
            lo == hi ? 0.0 : (freq01 - lo->freq01) / static_cast<double>(hi->freq01 - lo->freq01);
        const double delay_ms =
            std::clamp(lo->delay_ms + mix * (hi->delay_ms - lo->delay_ms), 0.0, maximum_delay_ms);
        const double attenuation_db = std::clamp(
            lo->attenuation_db + mix * (hi->attenuation_db - lo->attenuation_db), -96.0, 24.0);
        const auto delay = std::llround(delay_ms * sample_rate / (1000.0 * analysis_hop));
        candidate.delay_frames[static_cast<std::size_t>(bin)] =
            static_cast<std::int32_t>(std::clamp<long long>(delay, 0, maximum_delay_frames));
        candidate.attenuation_linear[static_cast<std::size_t>(bin)] =
            static_cast<float>(std::pow(10.0, attenuation_db / 20.0));
    }
    out_table = std::move(candidate);
    return true;
}

template <typename SampleType = float> class SpectralDelayMatrixT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);

    [[nodiscard]] bool prepare(const SpectralDelayMatrixConfig& config) {
        const auto geometry = checked_spectral_delay_matrix_geometry<SampleType>(config);
        if (!geometry)
            return false;
        auto candidate = std::make_unique<PreparedState>();
        candidate->config = config;
        candidate->geometry = *geometry;
        candidate->engine.prepare(config.frame);
        candidate->history.assign(static_cast<std::size_t>(config.frame.channels) *
                                      static_cast<std::size_t>(geometry->history_frames) *
                                      static_cast<std::size_t>(geometry->num_bins),
                                  {});
        SpectralDelayMatrixTable identity;
        identity.num_bins = geometry->num_bins;
        identity.delay_frames.assign(static_cast<std::size_t>(geometry->num_bins), 0);
        identity.attenuation_linear.assign(static_cast<std::size_t>(geometry->num_bins), 1.0f);
        candidate->publication =
            std::make_unique<runtime::TripleBuffer<PublishedTable>>(PublishedTable{identity, 0});
        state_ = std::move(candidate);
        next_generation_ = 1;
        return true;
    }

    static bool compile_table(const SpectralDelayMatrixRecipe& recipe, int num_bins,
                              int analysis_hop, double sample_rate, int maximum_delay_frames,
                              SpectralDelayMatrixTable& out_table) {
        return compile_spectral_delay_matrix_table(recipe, num_bins, analysis_hop, sample_rate,
                                                   maximum_delay_frames, out_table);
    }

    [[nodiscard]] bool publish_table(const SpectralDelayMatrixTable& table) {
        if (!state_ || !valid_table(table))
            return false;
        const auto generation = next_generation_++;
        if (next_generation_ == 0)
            ++next_generation_;
        state_->publication->write_with([&](PublishedTable& payload) {
            payload.table.version = table.version;
            payload.table.num_bins = table.num_bins;
            std::copy(table.delay_frames.begin(), table.delay_frames.end(),
                      payload.table.delay_frames.begin());
            std::copy(table.attenuation_linear.begin(), table.attenuation_linear.end(),
                      payload.table.attenuation_linear.begin());
            payload.generation = generation;
        });
        state_->published_generation.write(generation);
        return true;
    }

    void process(const SampleType* const* input, SampleType* const* output,
                 int num_samples) noexcept {
        if (!state_ || num_samples <= 0)
            return;
        state_->engine.process(input, output, num_samples,
                               [this](std::complex<SampleType>* const* frames, int bins) noexcept {
                                   process_frame(frames, bins);
                               });
    }

    /// Clear only delayed spectral content; preserve the engine pipeline and table.
    void purge() noexcept {
        if (!state_)
            return;
        std::fill(state_->history.begin(), state_->history.end(), std::complex<SampleType>{});
        state_->frames_seen = 0;
        state_->write_frame = 0;
    }
    /// Clear delayed content and the underlying analysis/synthesis pipeline.
    void reset() noexcept {
        if (!state_)
            return;
        purge();
        state_->engine.reset();
    }

    /// Fixed engine latency only; per-bin delays are declared content timing.
    [[nodiscard]] int latency_samples() const noexcept {
        return state_ ? state_->engine.latency_samples() : 0;
    }
    [[nodiscard]] int max_delay_frames() const noexcept {
        return state_ ? state_->geometry.history_frames - 1 : 0;
    }
    [[nodiscard]] int num_bins() const noexcept {
        return state_ ? state_->geometry.num_bins : 0;
    }
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept {
        return state_ ? state_->geometry.retained_bytes : 0;
    }
    [[nodiscard]] std::uint64_t maximum_tail_samples() const noexcept {
        return state_ ? static_cast<std::uint64_t>(latency_samples()) +
                            static_cast<std::uint64_t>(max_delay_frames()) *
                                state_->config.frame.analysis_hop
                      : 0;
    }
    [[nodiscard]] std::uint64_t active_table_version() const noexcept {
        return state_ ? state_->active_version.read() : 0;
    }
    [[nodiscard]] bool table_publication_pending() const noexcept {
        return state_ && state_->published_generation.read() != state_->active_generation.read();
    }

  private:
    struct PublishedTable {
        SpectralDelayMatrixTable table;
        std::uint64_t generation = 0;
    };
    struct PreparedState {
        SpectralDelayMatrixConfig config{};
        SpectralDelayMatrixGeometry<SampleType> geometry{};
        SpectralFrameEngineT<SampleType> engine;
        std::vector<std::complex<SampleType>> history;
        std::unique_ptr<runtime::TripleBuffer<PublishedTable>> publication;
        runtime::SeqLock<std::uint64_t> published_generation;
        runtime::SeqLock<std::uint64_t> active_generation;
        runtime::SeqLock<std::uint64_t> active_version;
        std::uint64_t consumed_generation = 0;
        std::uint64_t frames_seen = 0;
        int write_frame = 0;
    };

    bool valid_table(const SpectralDelayMatrixTable& table) const noexcept {
        const auto bins = static_cast<std::size_t>(state_->geometry.num_bins);
        if (table.num_bins != state_->geometry.num_bins || table.delay_frames.size() != bins ||
            table.attenuation_linear.size() != bins)
            return false;
        for (std::size_t i = 0; i < bins; ++i)
            if (table.delay_frames[i] < 0 || table.delay_frames[i] > max_delay_frames() ||
                !std::isfinite(table.attenuation_linear[i]) || table.attenuation_linear[i] < 0.0f)
                return false;
        return true;
    }
    std::size_t offset(int channel, int frame, int bin) const noexcept {
        return (static_cast<std::size_t>(channel) * state_->geometry.history_frames +
                static_cast<std::size_t>(frame)) *
                   state_->geometry.num_bins +
               static_cast<std::size_t>(bin);
    }
    void process_frame(std::complex<SampleType>* const* frames, int bins) noexcept {
        const auto& publication = state_->publication->read();
        if (state_->consumed_generation != publication.generation) {
            state_->consumed_generation = publication.generation;
            state_->active_generation.write(publication.generation);
            state_->active_version.write(publication.table.version);
        }
        const auto& table = publication.table;
        const int history_frames = state_->geometry.history_frames;
        for (int channel = 0; channel < state_->config.frame.channels; ++channel) {
            for (int bin = 0; bin < bins; ++bin) {
                state_->history[offset(channel, state_->write_frame, bin)] = frames[channel][bin];
                const int delay = table.delay_frames[static_cast<std::size_t>(bin)];
                if (static_cast<std::uint64_t>(delay) > state_->frames_seen) {
                    frames[channel][bin] = {};
                } else {
                    const int read =
                        (state_->write_frame - delay + history_frames) % history_frames;
                    frames[channel][bin] =
                        state_->history[offset(channel, read, bin)] *
                        static_cast<SampleType>(
                            table.attenuation_linear[static_cast<std::size_t>(bin)]);
                }
            }
        }
        state_->write_frame = (state_->write_frame + 1) % history_frames;
        ++state_->frames_seen;
    }

    std::unique_ptr<PreparedState> state_;
    std::uint64_t next_generation_ = 1;
};

using SpectralDelayMatrix = SpectralDelayMatrixT<float>;
using SpectralDelayMatrix64 = SpectralDelayMatrixT<double>;

} // namespace pulp::signal

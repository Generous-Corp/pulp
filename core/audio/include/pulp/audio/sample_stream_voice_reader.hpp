#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_asset.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pulp::audio {

inline constexpr std::size_t kSampleStreamVoiceMaxPageDemands = 16;

enum class SampleStreamVoiceSupply : std::uint8_t {
    Ready,
    Starved,
    EndOfSource,
    StaleGeneration,
    InvalidContract,
};

struct SampleStreamVoiceBlockPlan {
    SampleAssetToken asset{};
    SampleStreamSourceToken source{};
    double start_source_frame = 0.0;
    double source_frames_per_output = 0.0;
    std::uint32_t output_frames = 0;
    SampleStreamVoiceSupply supply = SampleStreamVoiceSupply::InvalidContract;
    std::array<SampleStreamPageDemand, kSampleStreamVoiceMaxPageDemands> demands{};
    std::uint32_t demand_count = 0;
};

struct SampleStreamVoiceBlockResult {
    SampleStreamVoiceSupply supply = SampleStreamVoiceSupply::InvalidContract;
    std::uint32_t ready_output_frames = 0;
};

struct SampleStreamVoiceDemandEnqueueResult {
    std::uint32_t demand_count = 0;
    std::uint32_t enqueued = 0;
    bool complete = false;
};

static_assert(std::is_trivially_copyable_v<SampleStreamVoiceBlockPlan>);
static_assert(std::is_trivially_copyable_v<SampleStreamVoiceBlockResult>);

/// Allocation-free linear forward one-shot reader over immutable sample assets.
/// Looping, reverse playback, and crossfade-region planning are separate policies.
class SampleStreamVoiceReader {
public:
    bool prepare(const SampleAssetView& asset,
                 SampleStreamRequesterToken requester) noexcept {
        reset();
        if (!asset.valid() || requester.requester_id == 0 ||
            requester.requester_generation == 0) {
            return false;
        }
        asset_ = asset.asset;
        source_ = asset.source;
        requester_ = requester;
        prepared_ = true;
        return true;
    }

    void reset() noexcept {
        asset_ = {};
        source_ = {};
        requester_ = {};
        source_position_ = 0.0;
        prepared_ = false;
    }

    bool seek(const SampleAssetView& asset, double source_frame) noexcept {
        if (!prepared_ || !same_generation(asset) || !asset.valid() ||
            !std::isfinite(source_frame) || source_frame < 0.0 ||
            source_frame > static_cast<double>(asset.total_frames)) {
            return false;
        }
        source_position_ = source_frame;
        return true;
    }

    double source_position() const noexcept { return source_position_; }

    SampleStreamVoiceBlockPlan plan_block(const SampleAssetView& asset,
                                          std::uint32_t output_frames,
                                          double source_frames_per_output,
                                          double output_sample_rate) const noexcept {
        SampleStreamVoiceBlockPlan plan;
        plan.asset = asset.asset;
        plan.source = asset.source;
        plan.start_source_frame = source_position_;
        plan.source_frames_per_output = source_frames_per_output;
        plan.output_frames = output_frames;

        if (!prepared_ || !asset.valid() || output_frames == 0 ||
            !positive_finite(source_frames_per_output) ||
            !positive_finite(output_sample_rate)) {
            return plan;
        }
        if (!same_generation(asset)) {
            plan.supply = SampleStreamVoiceSupply::StaleGeneration;
            return plan;
        }
        const auto block_advance =
            static_cast<double>(output_frames) * source_frames_per_output;
        if (!std::isfinite(block_advance) ||
            !std::isfinite(source_position_ + block_advance)) {
            return plan;
        }
        if (source_position_ >= static_cast<double>(asset.total_frames)) {
            plan.supply = SampleStreamVoiceSupply::EndOfSource;
            return plan;
        }

        const auto consumption_frames_per_second =
            source_frames_per_output * output_sample_rate;
        if (!positive_finite(consumption_frames_per_second)) return plan;

        for (std::uint32_t output = 0; output < output_frames; ++output) {
            const auto position = source_position_ +
                                  static_cast<double>(output) *
                                      source_frames_per_output;
            if (position >= static_cast<double>(asset.total_frames)) break;
            const auto lower = static_cast<std::uint64_t>(std::floor(position));
            const auto upper = upper_tap(lower, asset.total_frames);
            if (!append_page_demand(plan,
                                    asset,
                                    lower,
                                    position,
                                    consumption_frames_per_second) ||
                !append_page_demand(plan,
                                    asset,
                                    upper,
                                    position,
                                    consumption_frames_per_second)) {
                plan.demand_count = 0;
                plan.supply = SampleStreamVoiceSupply::InvalidContract;
                return plan;
            }
        }

        plan.supply = SampleStreamVoiceSupply::Ready;
        return plan;
    }

    template<std::size_t InboxCapacity>
    SampleStreamVoiceDemandEnqueueResult enqueue_demands(
        const SampleStreamVoiceBlockPlan& plan,
        SampleStreamCommandInbox<InboxCapacity>& inbox) const noexcept {
        SampleStreamVoiceDemandEnqueueResult result;
        result.demand_count = plan.demand_count;
        if (plan.supply != SampleStreamVoiceSupply::Ready ||
            plan.demand_count > plan.demands.size()) {
            return result;
        }
        for (std::uint32_t index = 0; index < plan.demand_count; ++index) {
            if (inbox.demand_page(plan.demands[index]) !=
                SampleStreamCommandPushStatus::Enqueued) {
                return result;
            }
            ++result.enqueued;
        }
        result.complete = true;
        return result;
    }

    SampleStreamVoiceBlockResult render_block(const SampleAssetView& asset,
                                              const SampleStreamVoiceBlockPlan& plan,
                                              BufferView<float> destination) noexcept {
        SampleStreamVoiceBlockResult result;
        if (!prepared_ || !asset.valid() ||
            plan.output_frames == 0 ||
            plan.output_frames > destination.num_samples() ||
            destination.num_channels() != asset.channels ||
            plan.start_source_frame != source_position_ ||
            !positive_finite(plan.source_frames_per_output)) {
            return result;
        }
        if (plan.supply == SampleStreamVoiceSupply::StaleGeneration) {
            result.supply = SampleStreamVoiceSupply::StaleGeneration;
            return result;
        }
        if (plan.supply != SampleStreamVoiceSupply::Ready &&
            plan.supply != SampleStreamVoiceSupply::EndOfSource) {
            return result;
        }
        if (!same_generation(asset) || plan.asset.asset_id != asset.asset.asset_id ||
            plan.asset.asset_generation != asset.asset.asset_generation ||
            plan.source.source_id != asset.source.source_id ||
            plan.source.source_generation != asset.source.source_generation) {
            result.supply = SampleStreamVoiceSupply::StaleGeneration;
            return result;
        }
        for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
            if (destination.channel_ptr(channel) == nullptr) return result;
        }
        if (plan.supply == SampleStreamVoiceSupply::EndOfSource) {
            clear_from(destination, 0, plan.output_frames);
            result.supply = SampleStreamVoiceSupply::EndOfSource;
            return result;
        }

        result.supply = SampleStreamVoiceSupply::Ready;
        for (std::uint32_t output = 0; output < plan.output_frames; ++output) {
            const auto position = plan.start_source_frame +
                                  static_cast<double>(output) *
                                      plan.source_frames_per_output;
            if (position >= static_cast<double>(asset.total_frames)) {
                clear_from(destination, output, plan.output_frames);
                result.supply = SampleStreamVoiceSupply::EndOfSource;
                break;
            }

            const auto lower_frame = static_cast<std::uint64_t>(std::floor(position));
            const auto upper_frame = upper_tap(lower_frame, asset.total_frames);
            const auto lower = locate(asset, lower_frame);
            const auto upper = locate(asset, upper_frame);
            if (!lower.ready || !upper.ready) {
                clear_from(destination, output, plan.output_frames);
                result.supply = SampleStreamVoiceSupply::Starved;
                break;
            }

            const auto fraction = static_cast<float>(position - std::floor(position));
            for (std::uint32_t channel = 0; channel < asset.channels; ++channel) {
                const auto* lower_sample = sample_pointer(asset, lower, channel);
                const auto* upper_sample = sample_pointer(asset, upper, channel);
                if (lower_sample == nullptr || upper_sample == nullptr) {
                    clear_from(destination, output, plan.output_frames);
                    result.supply = SampleStreamVoiceSupply::Starved;
                    advance_timeline(asset, plan);
                    return result;
                }
                destination.channel_ptr(channel)[output] =
                    *lower_sample + (*upper_sample - *lower_sample) * fraction;
            }
            ++result.ready_output_frames;
        }

        advance_timeline(asset, plan);
        return result;
    }

private:
    struct LocatedFrame {
        bool ready = false;
        bool preload = false;
        std::uint64_t source_frame = 0;
        SampleStreamPageView page{};
    };

    static bool positive_finite(double value) noexcept {
        return value > 0.0 && std::isfinite(value);
    }

    static std::uint64_t upper_tap(std::uint64_t lower,
                                   std::uint64_t total_frames) noexcept {
        return lower >= total_frames - 1 ? lower : lower + 1;
    }

    bool same_generation(const SampleAssetView& asset) const noexcept {
        return asset.asset.asset_id == asset_.asset_id &&
               asset.asset.asset_generation == asset_.asset_generation &&
               asset.source.source_id == source_.source_id &&
               asset.source.source_generation == source_.source_generation;
    }

    bool append_page_demand(SampleStreamVoiceBlockPlan& plan,
                            const SampleAssetView& asset,
                            std::uint64_t source_frame,
                            double current_position,
                            double consumption_frames_per_second) const noexcept {
        if (source_frame < asset.preload_frames) return true;
        if (!asset.has_stream_source || asset.stream_source.page_frames == 0) return false;
        const auto page_index = source_frame / asset.stream_source.page_frames;
        for (std::uint32_t index = 0; index < plan.demand_count; ++index) {
            if (plan.demands[index].page_index == page_index) return true;
        }
        if (plan.demand_count >= plan.demands.size()) return false;

        const auto distance = static_cast<double>(source_frame) - current_position;
        const auto max_frames =
            static_cast<double>(std::numeric_limits<std::uint64_t>::max());
        std::uint64_t resident_source_frames = 0;
        if (distance > 0.0) {
            resident_source_frames = distance >= max_frames
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(std::floor(distance));
        }
        plan.demands[plan.demand_count++] = {
            .source = asset.source,
            .requester = requester_,
            .page_index = page_index,
            .resident_source_frames = resident_source_frames,
            .consumption_frames_per_second = consumption_frames_per_second,
            .demand_class = source_position_ == 0.0
                ? SampleStreamDemandClass::Attack
                : SampleStreamDemandClass::Sustain,
        };
        return true;
    }

    static LocatedFrame locate(const SampleAssetView& asset,
                               std::uint64_t source_frame) noexcept {
        LocatedFrame located;
        located.source_frame = source_frame;
        if (source_frame < asset.preload_frames) {
            located.ready = true;
            located.preload = true;
            return located;
        }
        if (!asset.has_stream_source || asset.stream_source.window == nullptr) return located;
        located.page = asset.stream_source.window->ready_page_for_frame(
            asset.source.source_generation, source_frame);
        located.ready = located.page.valid;
        return located;
    }

    static const float* sample_pointer(const SampleAssetView& asset,
                                       const LocatedFrame& located,
                                       std::uint32_t channel) noexcept {
        if (!located.ready || channel >= asset.channels) return nullptr;
        if (located.preload) {
            const auto* data = asset.preload_channel_data(channel);
            return data == nullptr ? nullptr : data + located.source_frame;
        }
        return asset.stream_source.window->ready_channel_data(located.page, channel);
    }

    static void clear_from(BufferView<float> destination,
                           std::uint32_t start,
                           std::uint32_t end) noexcept {
        const auto count = static_cast<std::size_t>(end - start);
        for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
            std::fill_n(destination.channel_ptr(channel) + start, count, 0.0f);
        }
    }

    void advance_timeline(const SampleAssetView& asset,
                          const SampleStreamVoiceBlockPlan& plan) noexcept {
        const auto next = plan.start_source_frame +
                          static_cast<double>(plan.output_frames) *
                              plan.source_frames_per_output;
        source_position_ = std::min(next, static_cast<double>(asset.total_frames));
    }

    SampleAssetToken asset_{};
    SampleStreamSourceToken source_{};
    SampleStreamRequesterToken requester_{};
    double source_position_ = 0.0;
    bool prepared_ = false;
};

}  // namespace pulp::audio

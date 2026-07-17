#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_playback_cursor.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/sample_asset.hpp>
#include <pulp/audio/sample_stream_voice_reader.hpp>
#include <pulp/signal/interpolator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pulp::audio {

inline constexpr std::size_t kSampleStreamLoopMaxPageDemands = 16;

struct SampleStreamLoopBlockPlan {
    SampleAssetToken asset{};
    SampleStreamSourceToken source{};
    LoopPlaybackCursor start_cursor{};
    LoopPlaybackCursor end_cursor{};
    std::uint64_t timeline_serial = 0;
    std::uint32_t output_frames = 0;
    SampleStreamVoiceSupply supply = SampleStreamVoiceSupply::InvalidContract;
    std::array<SampleStreamPageDemand, kSampleStreamLoopMaxPageDemands> demands{};
    std::array<SampleStreamPageView, kSampleStreamLoopMaxPageDemands> ready_pages{};
    std::uint32_t demand_count = 0;
};

static_assert(std::is_trivially_copyable_v<SampleStreamLoopBlockPlan>);

/// Allocation-free paged adapter for LoopPlaybackCursor. Planning snapshots all
/// pages needed by primary, interpolation, and wrap-crossfade reads; rendering
/// then replays the same cursor state and advances the musical timeline even
/// when a page is missing.
class SampleStreamLoopVoiceReader {
public:
    bool prepare(const SampleAssetView& asset,
                 SampleStreamRequesterToken requester,
                 const LoopRegion& region,
                 double playback_rate) noexcept {
        reset();
        if (!asset.valid() || requester.requester_id == 0 ||
            requester.requester_generation == 0 ||
            !positive_finite(playback_rate) ||
            !cursor_.set_region(region, asset.total_frames)) {
            return false;
        }
        cursor_.set_playback_rate(playback_rate);
        cursor_.start();
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
        cursor_ = {};
        timeline_serial_ = 1;
        prepared_ = false;
    }

    bool set_playback_rate(double playback_rate) noexcept {
        if (!prepared_ || !positive_finite(playback_rate)) return false;
        cursor_.set_playback_rate(playback_rate);
        ++timeline_serial_;
        return true;
    }

    const LoopPlaybackCursor& cursor() const noexcept { return cursor_; }
    bool active() const noexcept { return prepared_ && cursor_.active(); }

    bool synchronize_cursor(const SampleAssetView& asset,
                            const LoopPlaybackCursor& cursor) noexcept {
        if (!prepared_ || !same_generation(asset) ||
            !validate_loop_region(cursor.region(), asset.total_frames).ok) {
            return false;
        }
        cursor_ = cursor;
        ++timeline_serial_;
        return true;
    }

    SampleStreamLoopBlockPlan plan_block(
        const SampleAssetView& asset,
        std::uint32_t output_frames,
        double output_sample_rate,
        SampleStreamDemandClass demand_class = SampleStreamDemandClass::Sustain) const noexcept {
        SampleStreamLoopBlockPlan plan;
        plan.asset = asset.asset;
        plan.source = asset.source;
        plan.start_cursor = cursor_;
        plan.end_cursor = cursor_;
        plan.timeline_serial = timeline_serial_;
        plan.output_frames = output_frames;

        if (!prepared_ || !asset.valid() || output_frames == 0 ||
            !positive_finite(output_sample_rate)) {
            return plan;
        }
        if (!same_generation(asset)) {
            plan.supply = SampleStreamVoiceSupply::StaleGeneration;
            return plan;
        }
        const auto step = std::abs(cursor_.step());
        if (!positive_finite(step) || !contract_accepts(asset, output_frames, step,
                                                        output_sample_rate)) {
            return plan;
        }
        if (!cursor_.active()) {
            plan.supply = SampleStreamVoiceSupply::EndOfSource;
            return plan;
        }

        auto scan = cursor_;
        const auto consumption_frames_per_second = step * output_sample_rate;
        for (std::uint32_t output = 0; output < output_frames && scan.active(); ++output) {
            const auto frame_plan = scan.frame_read_plan();
            if (!append_position(plan, asset, scan.region(), frame_plan.read_position,
                                 output, step, consumption_frames_per_second,
                                 demand_class) ||
                (frame_plan.blend &&
                 !append_position(plan, asset, scan.region(), frame_plan.blend_position,
                                  output, step, consumption_frames_per_second,
                                  demand_class))) {
                plan.demand_count = 0;
                plan.supply = SampleStreamVoiceSupply::InvalidContract;
                return plan;
            }
            (void) scan.advance();
        }
        plan.end_cursor = scan;
        plan.supply = SampleStreamVoiceSupply::Ready;
        return plan;
    }

    template<std::size_t InboxCapacity>
    SampleStreamVoiceDemandEnqueueResult enqueue_demands(
        const SampleStreamLoopBlockPlan& plan,
        SampleStreamCommandInbox<InboxCapacity>& inbox,
        std::uint32_t start_index = 0,
        std::uint64_t resident_source_frame_offset = 0,
        std::uint32_t end_index =
            std::numeric_limits<std::uint32_t>::max()) const noexcept {
        SampleStreamVoiceDemandEnqueueResult result;
        result.demand_count = plan.demand_count;
        result.next_demand_index = start_index;
        const auto range_end = std::min(end_index, plan.demand_count);
        if (plan.supply != SampleStreamVoiceSupply::Ready ||
            plan.demand_count > plan.demands.size() || start_index > range_end) {
            return result;
        }
        for (std::uint32_t index = start_index; index < range_end; ++index) {
            auto demand = plan.demands[index];
            demand.resident_source_frames =
                resident_source_frame_offset >
                    std::numeric_limits<std::uint64_t>::max() -
                        demand.resident_source_frames
                ? std::numeric_limits<std::uint64_t>::max()
                : resident_source_frame_offset + demand.resident_source_frames;
            if (inbox.demand_page(demand) !=
                SampleStreamCommandPushStatus::Enqueued) {
                return result;
            }
            ++result.enqueued;
            ++result.next_demand_index;
        }
        result.complete = result.next_demand_index == range_end;
        return result;
    }

    bool commit_planned_timeline(const SampleAssetView& asset,
                                 const SampleStreamLoopBlockPlan& plan) noexcept {
        if (!prepared_ || !same_plan_generation(asset, plan) ||
            plan.supply != SampleStreamVoiceSupply::Ready ||
            plan.timeline_serial != timeline_serial_ ||
            plan.start_cursor.position() != cursor_.position() ||
            plan.start_cursor.active() != cursor_.active()) {
            return false;
        }
        cursor_ = plan.end_cursor;
        ++timeline_serial_;
        return true;
    }

    SampleStreamVoiceBlockResult render_block(const SampleAssetView& asset,
                                              const SampleStreamLoopBlockPlan& plan,
                                              BufferView<float> destination) noexcept {
        SampleStreamVoiceBlockResult result;
        if (plan.output_frames == 0 || plan.output_frames > destination.num_samples())
            return result;
        for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
            if (destination.channel_ptr(channel) == nullptr) return result;
            std::fill_n(destination.channel_ptr(channel), plan.output_frames, 0.0f);
        }
        if (!prepared_ || !asset.valid() || destination.num_channels() != asset.channels) {
            return result;
        }
        if (!same_plan_generation(asset, plan)) {
            result.supply = SampleStreamVoiceSupply::StaleGeneration;
            return result;
        }
        if (plan.timeline_serial != timeline_serial_ ||
            plan.start_cursor.position() != cursor_.position() ||
            plan.start_cursor.active() != cursor_.active()) {
            return result;
        }
        if (plan.supply == SampleStreamVoiceSupply::EndOfSource) {
            result.supply = SampleStreamVoiceSupply::EndOfSource;
            return result;
        }
        if (plan.supply != SampleStreamVoiceSupply::Ready) return result;

        auto replay = plan.start_cursor;
        result.supply = SampleStreamVoiceSupply::Ready;
        for (std::uint32_t output = 0; output < plan.output_frames && replay.active(); ++output) {
            const auto frame_plan = replay.frame_read_plan();
            ResolvedRead primary;
            ResolvedRead blend;
            const bool primary_ready = resolve_read(plan, asset, replay.region(),
                                                    frame_plan.read_position, primary);
            const bool blend_ready = !frame_plan.blend ||
                resolve_read(plan, asset, replay.region(), frame_plan.blend_position, blend);
            if (!primary_ready || !blend_ready) {
                result.supply = SampleStreamVoiceSupply::Starved;
                break;
            }
            for (std::uint32_t channel = 0; channel < asset.channels; ++channel) {
                const auto a = interpolate(asset, primary, channel);
                if (!a.ready) {
                    result.supply = SampleStreamVoiceSupply::Starved;
                    break;
                }
                double sample = a.sample;
                if (frame_plan.blend) {
                    const auto b = interpolate(asset, blend, channel);
                    if (!b.ready) {
                        result.supply = SampleStreamVoiceSupply::Starved;
                        break;
                    }
                    sample = sample * frame_plan.primary_gain +
                             static_cast<double>(b.sample) * frame_plan.blend_gain;
                }
                destination.channel_ptr(channel)[output] = static_cast<float>(sample);
            }
            if (result.supply == SampleStreamVoiceSupply::Starved) break;
            ++result.ready_output_frames;
            (void) replay.advance();
        }

        cursor_ = plan.end_cursor;
        ++timeline_serial_;
        if (result.supply == SampleStreamVoiceSupply::Ready && !cursor_.active())
            result.supply = SampleStreamVoiceSupply::EndOfSource;
        return result;
    }

private:
    struct LocatedFrame {
        std::uint64_t source_frame = 0;
        SampleStreamPageView page{};
        bool ready = false;
        bool preload = false;
    };

    struct ResolvedRead {
        std::array<LocatedFrame, 4> taps{};
        float fraction = 0.0f;
        LoopInterpolationMode interpolation = LoopInterpolationMode::None;
        std::uint32_t tap_count = 0;
    };

    struct InterpolatedSample { float sample = 0.0f; bool ready = false; };

    static bool positive_finite(double value) noexcept {
        return value > 0.0 && std::isfinite(value);
    }

    bool same_generation(const SampleAssetView& asset) const noexcept {
        return asset.asset.asset_id == asset_.asset_id &&
               asset.asset.asset_generation == asset_.asset_generation &&
               asset.source.source_id == source_.source_id &&
               asset.source.source_generation == source_.source_generation;
    }

    bool same_plan_generation(const SampleAssetView& asset,
                              const SampleStreamLoopBlockPlan& plan) const noexcept {
        return same_generation(asset) && plan.asset.asset_id == asset.asset.asset_id &&
               plan.asset.asset_generation == asset.asset.asset_generation &&
               plan.source.source_id == asset.source.source_id &&
               plan.source.source_generation == asset.source.source_generation;
    }

    static bool contract_accepts(const SampleAssetView& asset,
                                 std::uint32_t output_frames,
                                 double step,
                                 double output_sample_rate) noexcept {
        if (asset.fully_resident()) return true;
        if (!asset.has_preload_contract) return false;
        const auto& contract = asset.preload_contract;
        const auto maximum_step = static_cast<double>(asset.sample_rate) /
                                  output_sample_rate * contract.maximum_playback_ratio;
        return output_frames <= contract.maximum_host_block_frames &&
               output_sample_rate == contract.host_sample_rate && step <= maximum_step;
    }

    static std::uint32_t tap_count(LoopInterpolationMode mode) noexcept {
        return mode == LoopInterpolationMode::Cubic ? 4u
             : mode == LoopInterpolationMode::Linear ? 2u : 1u;
    }

    static std::int64_t tap_offset(LoopInterpolationMode mode,
                                   std::uint32_t tap) noexcept {
        if (mode == LoopInterpolationMode::Cubic) return static_cast<std::int64_t>(tap) - 1;
        return static_cast<std::int64_t>(tap);
    }

    bool append_position(SampleStreamLoopBlockPlan& plan,
                         const SampleAssetView& asset,
                         const LoopRegion& region,
                         double position,
                         std::uint32_t first_output,
                         double step,
                         double consumption_frames_per_second,
                         SampleStreamDemandClass demand_class) const noexcept {
        const auto normalized = LoopReader::normalize_position(region, position);
        const auto base = static_cast<std::int64_t>(std::floor(normalized));
        for (std::uint32_t tap = 0; tap < tap_count(region.interpolation); ++tap) {
            const auto frame = LoopReader::source_frame_for_tap(
                region, asset.total_frames, base + tap_offset(region.interpolation, tap));
            if (!append_page(plan, asset, frame, first_output, step,
                             consumption_frames_per_second, demand_class)) {
                return false;
            }
        }
        return true;
    }

    bool append_page(SampleStreamLoopBlockPlan& plan,
                     const SampleAssetView& asset,
                     std::uint64_t source_frame,
                     std::uint32_t first_output,
                     double step,
                     double consumption_frames_per_second,
                     SampleStreamDemandClass demand_class) const noexcept {
        if (source_frame < asset.preload_frames) return true;
        if (!asset.has_stream_source || asset.stream_source.page_frames == 0 ||
            asset.stream_source.window == nullptr) return false;
        const auto page_index = source_frame / asset.stream_source.page_frames;
        const auto distance = static_cast<double>(first_output) * step;
        const auto resident = distance >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(std::floor(distance));
        for (std::uint32_t index = 0; index < plan.demand_count; ++index) {
            if (plan.demands[index].page_index != page_index) continue;
            plan.demands[index].resident_source_frames =
                std::min(plan.demands[index].resident_source_frames, resident);
            if (demand_class == SampleStreamDemandClass::Attack)
                plan.demands[index].demand_class = demand_class;
            return true;
        }
        if (plan.demand_count >= plan.demands.size()) return false;
        plan.ready_pages[plan.demand_count] =
            asset.stream_source.window->ready_page_for_frame(
                asset.source.source_generation, source_frame);
        plan.demands[plan.demand_count++] = {
            .source = asset.source,
            .requester = requester_,
            .page_index = page_index,
            .resident_source_frames = resident,
            .consumption_frames_per_second = consumption_frames_per_second,
            .demand_class = demand_class,
        };
        return true;
    }

    static LocatedFrame locate(const SampleStreamLoopBlockPlan& plan,
                               const SampleAssetView& asset,
                               std::uint64_t frame) noexcept {
        LocatedFrame located{.source_frame = frame};
        if (frame < asset.preload_frames) {
            located.ready = true;
            located.preload = true;
            return located;
        }
        if (!asset.has_stream_source || asset.stream_source.window == nullptr ||
            asset.stream_source.page_frames == 0) return located;
        const auto page_index = frame / asset.stream_source.page_frames;
        for (std::uint32_t index = 0; index < plan.demand_count; ++index) {
            if (plan.demands[index].page_index != page_index) continue;
            located.page = plan.ready_pages[index];
            if (located.page.valid && frame >= located.page.start_frame)
                located.page.local_offset = frame - located.page.start_frame;
            located.ready = located.page.valid;
            break;
        }
        return located;
    }

    static bool resolve_read(const SampleStreamLoopBlockPlan& plan,
                             const SampleAssetView& asset,
                             const LoopRegion& region,
                             double position,
                             ResolvedRead& resolved) noexcept {
        const auto normalized = LoopReader::normalize_position(region, position);
        const auto base = static_cast<std::int64_t>(std::floor(normalized));
        resolved.fraction = static_cast<float>(normalized - static_cast<double>(base));
        resolved.interpolation = region.interpolation;
        resolved.tap_count = tap_count(region.interpolation);
        for (std::uint32_t tap = 0; tap < resolved.tap_count; ++tap) {
            const auto frame = LoopReader::source_frame_for_tap(
                region, asset.total_frames, base + tap_offset(region.interpolation, tap));
            resolved.taps[tap] = locate(plan, asset, frame);
            if (!resolved.taps[tap].ready) return false;
        }
        return true;
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

    static InterpolatedSample interpolate(const SampleAssetView& asset,
                                          const ResolvedRead& read,
                                          std::uint32_t channel) noexcept {
        std::array<float, 4> samples{};
        for (std::uint32_t tap = 0; tap < read.tap_count; ++tap) {
            const auto* sample = sample_pointer(asset, read.taps[tap], channel);
            if (sample == nullptr) return {};
            samples[tap] = *sample;
        }
        if (read.interpolation == LoopInterpolationMode::Cubic) {
            return {signal::Interpolator::hermite(
                        read.fraction, samples[0], samples[1], samples[2], samples[3]), true};
        }
        if (read.interpolation == LoopInterpolationMode::Linear) {
            return {signal::Interpolator::linear(read.fraction, samples[0], samples[1]), true};
        }
        return {samples[0], true};
    }

    SampleAssetToken asset_{};
    SampleStreamSourceToken source_{};
    SampleStreamRequesterToken requester_{};
    LoopPlaybackCursor cursor_{};
    std::uint64_t timeline_serial_ = 1;
    bool prepared_ = false;
};

}  // namespace pulp::audio

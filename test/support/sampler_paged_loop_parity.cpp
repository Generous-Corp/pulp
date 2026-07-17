#include "sampler_paged_loop_parity.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::test::sampler_paged_loop_parity {

namespace {

std::uint64_t map_source_frame(const audio::LoopRegion& region,
                               std::int64_t frame) noexcept {
    const auto start = static_cast<std::int64_t>(region.start_frame);
    const auto end = static_cast<std::int64_t>(region.end_frame);
    if (region.playback_mode == audio::LoopPlaybackMode::OneShot ||
        region.playback_mode == audio::LoopPlaybackMode::ReverseOnce) {
        return static_cast<std::uint64_t>(std::clamp(frame, start, end - 1));
    }

    if (region.playback_mode == audio::LoopPlaybackMode::PingPong) {
        const auto span = end - start - 1;
        if (span == 0) return static_cast<std::uint64_t>(start);
        const auto distance = frame >= start ? frame - start : start - frame;
        const auto quotient = distance / span;
        const auto remainder = distance % span;
        return static_cast<std::uint64_t>(
            start + ((quotient & 1) == 0 ? remainder : span - remainder));
    }

    const auto length = end - start;
    auto relative = (frame - start) % length;
    if (relative < 0) relative += length;
    return static_cast<std::uint64_t>(start + relative);
}

void append_unique(LoopInterpolationTaps& taps,
                   std::uint64_t source_frame) noexcept {
    for (std::uint32_t index = 0; index < taps.count; ++index) {
        if (taps.source_frames[index] == source_frame) return;
    }
    taps.source_frames[taps.count++] = source_frame;
}

void append_page_first_use(PagedLoopOraclePlan& plan,
                           const PagedLoopOracleConfig& config,
                           std::uint64_t output_frame,
                           std::uint64_t source_frame,
                           std::uint64_t use_order,
                           LoopTapRole role) {
    if (source_frame < config.resident_frames) return;
    const auto page_index = source_frame / config.page_frames;
    const auto existing = std::find_if(
        plan.page_first_uses.begin(),
        plan.page_first_uses.end(),
        [page_index](const LoopPageFirstUse& use) noexcept {
            return use.page_index == page_index;
        });
    if (existing != plan.page_first_uses.end()) return;
    plan.page_first_uses.push_back({
        .page_index = page_index,
        .first_use_output_frame = output_frame,
        .first_use_order = use_order,
        .first_source_frame = source_frame,
        .role = role,
    });
}

void append_tap_pages(PagedLoopOraclePlan& plan,
                      const PagedLoopOracleConfig& config,
                      std::uint64_t output_frame,
                      const LoopInterpolationTaps& taps,
                      LoopTapRole role,
                      std::uint64_t& use_order) {
    for (std::uint32_t index = 0; index < taps.count; ++index) {
        append_page_first_use(plan,
                              config,
                              output_frame,
                              taps.source_frames[index],
                              use_order++,
                              role);
    }
}

}  // namespace

std::optional<LoopInterpolationTaps> make_interpolation_taps(
    const audio::LoopRegion& region,
    double position) noexcept {
    constexpr auto maximum_safe_end =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 2);
    if (region.start_frame >= region.end_frame ||
        region.end_frame > maximum_safe_end || !std::isfinite(position) ||
        position < static_cast<double>(region.start_frame) ||
        position > static_cast<double>(region.end_frame)) {
        return std::nullopt;
    }

    const auto base = static_cast<std::int64_t>(std::floor(position));
    LoopInterpolationTaps taps;
    switch (region.interpolation) {
        case audio::LoopInterpolationMode::None:
            append_unique(taps, map_source_frame(region, base));
            break;
        case audio::LoopInterpolationMode::Linear:
            append_unique(taps, map_source_frame(region, base));
            append_unique(taps, map_source_frame(region, base + 1));
            break;
        case audio::LoopInterpolationMode::Cubic:
            append_unique(taps, map_source_frame(region, base - 1));
            append_unique(taps, map_source_frame(region, base));
            append_unique(taps, map_source_frame(region, base + 1));
            append_unique(taps, map_source_frame(region, base + 2));
            break;
    }
    return taps;
}

std::optional<PagedLoopOraclePlan> make_paged_loop_oracle_plan(
    const PagedLoopOracleConfig& config) {
    const auto& region = config.loop.region;
    if (config.total_source_frames == 0 || config.page_frames == 0 ||
        config.resident_frames > config.total_source_frames ||
        region.end_frame > config.total_source_frames) {
        return std::nullopt;
    }

    const auto schedule = sampler_loop_parity::make_loop_oracle_schedule(config.loop);
    if (!schedule) return std::nullopt;

    PagedLoopOraclePlan plan;
    plan.frames.reserve(schedule->size());
    std::uint64_t use_order = 0;
    for (const auto& oracle_tap : *schedule) {
        LoopFrameTapPlan frame;
        frame.output_frame = oracle_tap.output_frame;
        frame.active = oracle_tap.active;
        frame.has_blend = oracle_tap.blend;
        if (oracle_tap.active) {
            const auto primary = make_interpolation_taps(
                region, oracle_tap.primary_position);
            if (!primary) return std::nullopt;
            frame.primary = *primary;
            append_tap_pages(plan,
                             config,
                             frame.output_frame,
                             frame.primary,
                             LoopTapRole::Primary,
                             use_order);

            if (oracle_tap.blend) {
                const auto blend = make_interpolation_taps(
                    region, oracle_tap.blend_position);
                if (!blend) return std::nullopt;
                frame.blend = *blend;
                append_tap_pages(plan,
                                 config,
                                 frame.output_frame,
                                 frame.blend,
                                 LoopTapRole::Blend,
                                 use_order);
            }
        }
        plan.frames.push_back(frame);
    }

    plan.required_page_demands = plan.page_first_uses.size();
    plan.within_demand_capacity =
        plan.required_page_demands <= config.page_demand_capacity;
    return plan;
}

}  // namespace pulp::test::sampler_paged_loop_parity

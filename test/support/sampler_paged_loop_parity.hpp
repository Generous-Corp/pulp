#pragma once

#include "sampler_loop_parity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::test::sampler_paged_loop_parity {

inline constexpr std::size_t kDefaultPageDemandCapacity = 16;

enum class LoopTapRole : std::uint8_t {
    Primary,
    Blend,
};

struct LoopInterpolationTaps {
    std::array<std::uint64_t, 4> source_frames{};
    std::uint32_t count = 0;
};

struct LoopFrameTapPlan {
    std::uint64_t output_frame = 0;
    LoopInterpolationTaps primary;
    LoopInterpolationTaps blend;
    bool active = false;
    bool has_blend = false;
};

struct LoopPageFirstUse {
    std::uint64_t page_index = 0;
    std::uint64_t first_use_output_frame = 0;
    std::uint64_t first_use_order = 0;
    std::uint64_t first_source_frame = 0;
    LoopTapRole role = LoopTapRole::Primary;
};

struct PagedLoopOracleConfig {
    sampler_loop_parity::LoopOracleConfig loop;
    std::uint64_t total_source_frames = 0;
    std::uint64_t page_frames = 0;
    std::uint64_t resident_frames = 0;
    std::size_t page_demand_capacity = kDefaultPageDemandCapacity;
};

struct PagedLoopOraclePlan {
    std::vector<LoopFrameTapPlan> frames;
    std::vector<LoopPageFirstUse> page_first_uses;
    std::size_t required_page_demands = 0;
    bool within_demand_capacity = false;
};

std::optional<LoopInterpolationTaps> make_interpolation_taps(
    const audio::LoopRegion& region,
    double position) noexcept;

std::optional<PagedLoopOraclePlan> make_paged_loop_oracle_plan(
    const PagedLoopOracleConfig& config);

}  // namespace pulp::test::sampler_paged_loop_parity

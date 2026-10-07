#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::format {

enum class ProjectionSurface : std::uint8_t { clap, vst3, lv2, wam, wclap, au };
enum class ProjectionStatus : std::uint8_t { supported, unsupported };

struct ProjectionResult {
    ProjectionStatus status;
    std::string_view reason;
    constexpr bool supported() const noexcept {
        return status == ProjectionStatus::supported;
    }
};

/// Maps the smallest DSPX-07 projection slice. Graph-owned features must use
/// a baked Processor descriptor before an adapter can advertise support.
constexpr ProjectionResult projection_capability(ProjectionSurface surface, bool baked_processor,
                                                 bool bounded_descriptor) noexcept {
    if (!baked_processor)
        return {ProjectionStatus::unsupported,
                "graph-only descriptor has no baked Processor projection"};
    if (!bounded_descriptor)
        return {ProjectionStatus::unsupported,
                "descriptor bounds are missing or exceed adapter limits"};
    switch (surface) {
    case ProjectionSurface::clap:
    case ProjectionSurface::vst3:
    case ProjectionSurface::lv2:
    case ProjectionSurface::wam:
    case ProjectionSurface::wclap:
        return {ProjectionStatus::supported, {}};
    case ProjectionSurface::au:
        return {ProjectionStatus::unsupported,
                "AU projection is deferred to a separate adapter owner"};
    }
    return {ProjectionStatus::unsupported, "unknown projection surface"};
}

} // namespace pulp::format

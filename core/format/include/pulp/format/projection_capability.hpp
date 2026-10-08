#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::format {

/// Product surfaces that can consume a baked Processor projection.
///
/// The enum is intentionally independent of adapter implementation types so an
/// installed SDK consumer can make the same decision before selecting a plugin
/// format or browser bridge.
enum class ProjectionSurface : std::uint8_t { clap, vst3, lv2, wam, wclap, au };

enum class ProjectionStatus : std::uint8_t { supported, unsupported };

struct ProjectionResult {
    ProjectionStatus status;
    std::string_view reason;

    constexpr bool supported() const noexcept {
        return status == ProjectionStatus::supported;
    }
};

/// Return the bounded DSPX-07 projection decision for one target surface.
///
/// A graph descriptor is not itself a plugin ABI. It must first be lowered to a
/// self-contained, bounded Processor descriptor. Unknown or unbounded input is
/// refused so callers cannot accidentally advertise a graph feature that an
/// adapter or browser host cannot represent.
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

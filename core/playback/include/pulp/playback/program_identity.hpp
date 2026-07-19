#pragma once

#include <pulp/timeline/item_id.hpp>

#include <compare>
#include <cstdint>

namespace pulp::playback {

using ProgramGeneration = std::uint64_t;

struct RendererProgramKey {
    timeline::ItemId item_id;
    ProgramGeneration generation = 0;
    constexpr auto operator<=>(const RendererProgramKey&) const = default;
};

constexpr bool is_monotonic_renderer_adoption(RendererProgramKey active,
                                              RendererProgramKey candidate) noexcept {
    return candidate.item_id.valid() && candidate.generation != 0 &&
           (active.generation == 0 ||
            (candidate.item_id == active.item_id && candidate.generation > active.generation));
}

} // namespace pulp::playback

#pragma once

#include <cstdint>

namespace pulp::gpu_audio::detail {

struct StreamingBlockStamp {
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;

    friend constexpr bool operator==(StreamingBlockStamp, StreamingBlockStamp) noexcept = default;
};

} // namespace pulp::gpu_audio::detail

#pragma once

#include <pulp/timeline/assets.hpp>

namespace pulp::timeline::detail {

bool validate_and_canonicalize(AudioLoopInfo& loop, std::uint64_t frame_count);

} // namespace pulp::timeline::detail

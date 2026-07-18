#include <pulp/audio/sample_heritage_src.hpp>

#include <type_traits>

namespace pulp::audio {

static_assert(!std::is_copy_constructible_v<SampleHeritageCausalSrc>);
static_assert(!std::is_move_constructible_v<SampleHeritageCausalSrc>);

}  // namespace pulp::audio

#include <pulp/audio/sample_heritage_pitch.hpp>

#include <type_traits>

namespace pulp::audio {

static_assert(!std::is_copy_constructible_v<SampleHeritagePitchProcessor>);
static_assert(!std::is_move_constructible_v<SampleHeritagePitchProcessor>);

}  // namespace pulp::audio

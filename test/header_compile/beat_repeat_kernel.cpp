#include <pulp/signal/beat_repeat_kernel.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_move_constructible_v<pulp::signal::BeatRepeatKernel>);
static_assert(!std::is_copy_constructible_v<pulp::signal::BeatRepeatKernel>);
static_assert(noexcept(std::declval<pulp::signal::BeatRepeatKernel&>().reset()));

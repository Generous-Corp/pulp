#include <pulp/signal/explicit_q_resonator_bank.hpp>

#include <type_traits>
#include <utility>

static_assert(pulp::signal::ExplicitQResonatorBank::kMaximumBands == 64);
static_assert(
    std::is_same_v<decltype(std::declval<pulp::signal::ExplicitQResonatorBank&>().process(0.0f)),
                   float>);

#define PULP_SIGNAL_FAST_LADDER_TANH 1
#include <pulp/signal/fast_math.hpp>

// Build-time check of how the ladder saturator macros select the default.
static_assert(pulp::signal::kDefaultLadderSaturation ==
              pulp::signal::LadderSaturation::realtime_efficient);

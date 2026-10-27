#define PULP_SIGNAL_LADDER_SATURATION 0
#include <pulp/signal/fast_math.hpp>

// Build-time check of how the ladder saturator macros select the default.
static_assert(pulp::signal::kDefaultLadderSaturation == pulp::signal::LadderSaturation::exact);

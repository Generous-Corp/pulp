#include "examples/faust-gain/generated_gain.hpp"
#undef FAUSTCLASS
#include "examples/faust-filter/generated_filter.hpp"
#undef FAUSTCLASS
#include "examples/faust-tremolo/generated_tremolo.hpp"
#undef FAUSTCLASS

static_assert(sizeof(FaustGainDsp) > 0);
static_assert(sizeof(FaustFilterDsp) > 0);
static_assert(sizeof(FaustTremoloDsp) > 0);

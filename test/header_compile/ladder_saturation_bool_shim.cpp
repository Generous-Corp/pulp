#include <pulp/signal/fast_math.hpp>

// The deprecated boolean form still compiles and keeps its original meaning:
// false is exact std::tanh, true the Padé FastMath::tanh.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(disable : 4996)
#endif

float ladder_saturation_bool_shim_exact(float x) {
    return pulp::signal::ladder_tanh_profiled<false>(x);
}

float ladder_saturation_bool_shim_fast(float x) {
    return pulp::signal::ladder_tanh_profiled<true>(x);
}

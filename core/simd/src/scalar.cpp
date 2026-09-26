// pulp::simd kernels, scalar reference backend. The loops live in
// <pulp/simd/scalar_kernels.hpp> so the header fallback shares them.

#include <pulp/simd/scalar_kernels.hpp>
#include <pulp/simd/simd.hpp>

namespace pulp::simd::backend::scalar {

PULP_SIMD_SCALAR_DEFINE(, float)
PULP_SIMD_SCALAR_DEFINE(, double)

} // namespace pulp::simd::backend::scalar

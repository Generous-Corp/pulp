// A translation unit built without pulp-simd's usage requirements (only its
// include directory, no library, no backend definitions) still resolves every
// pulp::simd kernel, through the inline scalar reference. Hand-listed source
// builds (wasm, VCV Rack) that compile the signal headers directly rely on it.

#include <catch2/catch_test_macros.hpp>

#include <pulp/simd/simd.hpp>

#include <array>
#include <cstddef>
#include <string>

#if defined(PULP_SIMD_DEFAULT_BACKEND_ACCELERATE) || defined(PULP_SIMD_DEFAULT_BACKEND_HIGHWAY) || \
    defined(PULP_SIMD_DEFAULT_BACKEND_SCALAR)
#error "this test must be compiled without pulp-simd's backend definitions"
#endif

TEST_CASE("pulp::simd resolves to the inline scalar kernels without the library",
          "[simd][header-fallback]") {
    CHECK(std::string(pulp::simd::active_backend_name) == "inline-scalar");

    const std::array<float, 5> x{1.0f, -2.0f, 3.0f, -4.0f, 5.0f};
    const std::array<float, 2> h{0.5f, 0.25f};
    std::array<float, 4> y{};
    CHECK(pulp::simd::sum(x.data(), x.size()) == 3.0f);
    CHECK(pulp::simd::dot(x.data(), x.data(), x.size()) == 55.0f);
    CHECK(pulp::simd::sum_squares(x.data(), x.size()) == 55.0f);
    CHECK(pulp::simd::max_abs(x.data(), x.size()) == 5.0f);
    pulp::simd::correlate(x.data(), h.data(), y.data(), 4, 2);
    CHECK(y == std::array<float, 4>{0.0f, -0.25f, 0.5f, -0.75f});

    const std::array<double, 3> xd{0.5, 0.25, -1.0};
    CHECK(pulp::simd::sum(xd.data(), xd.size()) == -0.25);
}

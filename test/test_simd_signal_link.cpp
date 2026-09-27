// pulp::signal consumers reach the pulp::simd kernels without linking
// pulp-runtime. This binary links only pulp::signal; the root CMakeLists.txt
// also walks its link closure at configure time and fails if pulp-runtime or
// mbedTLS appear in it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/simd/simd.hpp>

#include <array>
#include <cstddef>

TEST_CASE("pulp::simd kernels are callable through pulp::signal alone", "[simd][link]") {
    std::array<float, 37> a{};
    std::array<float, 37> b{};
    std::array<float, 37> out{};
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = float(i) * 0.25f - 3.0f;
        b[i] = 1.5f;
    }

    pulp::simd::add(a.data(), b.data(), out.data(), a.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE(out[i] == a[i] + b[i]);

    // 0.25 * (0 + ... + 36) - 3 * 37 = 166.5 - 111 = 55.5, exact in float.
    REQUIRE(pulp::simd::sum(a.data(), a.size()) == 55.5f);
    REQUIRE(pulp::simd::maximum(a.data(), a.size()) == 6.0f);
    REQUIRE(pulp::simd::minimum(a.data(), a.size()) == -3.0f);
    REQUIRE(pulp::simd::float_lanes() >= 1);
}

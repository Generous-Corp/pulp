// The plain-struct lanes (detail::lanes::portable), which compilers without
// the GCC/Clang vector extension use, against the vector lanes, operation by
// operation: every lane of every result must be bit-identical, including for
// NaN, signed zero, infinities and subnormals. The kernels built on these
// lanes are additionally run end to end in portable mode by the
// pulp-test-fdn-reverb-portable-lanes suite.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/detail/simd_lanes.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#if (defined(__GNUC__) || defined(__clang__)) && !defined(PULP_SIMD_LANES_PORTABLE)

namespace lanes = pulp::signal::detail::lanes;
namespace portable = pulp::signal::detail::lanes::portable;

namespace {

std::uint32_t bits(float x) {
    std::uint32_t b;
    std::memcpy(&b, &x, sizeof b);
    return b;
}
std::uint64_t bits(double x) {
    std::uint64_t b;
    std::memcpy(&b, &x, sizeof b);
    return b;
}

// Compare every lane of a native and a portable float vector bit for bit.
bool same(lanes::F4 a, portable::F4 b) {
    for (int i = 0; i < 4; ++i)
        if (bits(lanes::lane(a, i)) != bits(portable::lane(b, i)))
            return false;
    return true;
}
bool same(lanes::D2 a, portable::D2 b) {
    for (int i = 0; i < 2; ++i)
        if (bits(lanes::lane(a, i)) != bits(portable::lane(b, i)))
            return false;
    return true;
}
// A native mask and a portable mask agree when they select the same lanes.
bool same_mask(lanes::M4 a, portable::M4 b) {
    const lanes::F4 one = lanes::splat(1.0f), zero = lanes::splat(0.0f);
    return same(lanes::select(a, one, zero),
                portable::select(b, portable::splat(1.0f), portable::splat(0.0f)));
}
bool same_mask(lanes::M2 a, portable::M2 b) {
    return same(lanes::select(a, lanes::splat(1.0), lanes::splat(0.0)),
                portable::select(b, portable::splat(1.0), portable::splat(0.0)));
}

std::vector<float> float_values() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    std::vector<float> v = {0.0f,
                            -0.0f,
                            1.0f,
                            -1.0f,
                            0.5f,
                            -2.75f,
                            1e-40f,
                            -1e-40f,
                            3.4e38f,
                            -3.4e38f,
                            inf,
                            -inf,
                            std::numeric_limits<float>::quiet_NaN(),
                            1e-7f,
                            123456.789f};
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1000.0f, 1000.0f);
    for (int i = 0; i < 49; ++i)
        v.push_back(u(rng));
    return v;
}

} // namespace

TEST_CASE("portable lanes compute exactly what the vector lanes compute",
          "[signal][simd][lanes][parity]") {
    const auto values = float_values();
    const std::size_t n = values.size();
    std::size_t checked = 0;
    for (std::size_t i = 0; i + 8 <= n; ++i) {
        const float* p = values.data() + i;
        const float* q = values.data() + (n - 8 - i);
        const lanes::F4 a = lanes::load(p), b = lanes::load(q);
        const portable::F4 pa = portable::load(p), pb = portable::load(q);
        REQUIRE(same(a, pa));
        CHECK(same(a + b, pa + pb));
        CHECK(same(a - b, pa - pb));
        CHECK(same(a * b, pa * pb));
        CHECK(same(a / b, pa / pb));
        CHECK(same(-a, -pa));
        CHECK(same(lanes::abs(a), portable::abs(pa)));
        CHECK(same_mask(lanes::less(a, b), portable::less(pa, pb)));
        CHECK(same_mask(lanes::less_equal(a, b), portable::less_equal(pa, pb)));
        CHECK(same_mask(lanes::greater(a, b), portable::greater(pa, pb)));
        CHECK(lanes::all(lanes::less(a, b)) == portable::all(portable::less(pa, pb)));
        CHECK(same_mask(
            lanes::both(lanes::less(a, b), lanes::greater(a, lanes::splat(0.0f))),
            portable::both(portable::less(pa, pb), portable::greater(pa, portable::splat(0.0f)))));
        CHECK(same(lanes::select(lanes::less(a, b), a, b),
                   portable::select(portable::less(pa, pb), pa, pb)));
        CHECK(same(lanes::min(a, b), portable::min(pa, pb)));
        CHECK(same(lanes::max(a, b), portable::max(pa, pb)));
        const lanes::F4 lo = lanes::splat(-1.5f), hi = lanes::splat(2.0f);
        CHECK(same(lanes::clamp(a, lo, hi),
                   portable::clamp(pa, portable::splat(-1.5f), portable::splat(2.0f))));
        CHECK(same(lanes::hadamard4(a), portable::hadamard4(pa)));
        CHECK(same(lanes::swap_pairs(a), portable::swap_pairs(pa)));
        CHECK(same(lanes::swap_halves(a), portable::swap_halves(pa)));
        CHECK(same(lanes::even_odd(a, b), portable::even_odd(pa, pb)));
        CHECK(same(lanes::low_low(a, b), portable::low_low(pa, pb)));

        lanes::F4 t0 = a, t1 = b, t2 = a * b, t3 = a - b;
        portable::F4 u0 = pa, u1 = pb, u2 = pa * pb, u3 = pa - pb;
        lanes::transpose(t0, t1, t2, t3);
        portable::transpose(u0, u1, u2, u3);
        CHECK((same(t0, u0) && same(t1, u1) && same(t2, u2) && same(t3, u3)));

        const lanes::D2 lw = lanes::widen_low(a), hw = lanes::widen_high(a);
        const portable::D2 plw = portable::widen_low(pa), phw = portable::widen_high(pa);
        CHECK(same(lw, plw));
        CHECK(same(hw, phw));
        CHECK(same(lanes::narrow(lw * hw, lw), portable::narrow(plw * phw, plw)));
        CHECK(same(lanes::load_widen2(p), portable::load_widen2(p)));
        float out_native[2], out_portable[2];
        lanes::store_narrow2(out_native, lw + hw);
        portable::store_narrow2(out_portable, plw + phw);
        CHECK((bits(out_native[0]) == bits(out_portable[0]) &&
               bits(out_native[1]) == bits(out_portable[1])));
        float stored_native[4], stored_portable[4];
        lanes::store(stored_native, a * b);
        portable::store(stored_portable, pa * pb);
        CHECK(std::memcmp(stored_native, stored_portable, sizeof stored_native) == 0);

        CHECK(same(lw + hw, plw + phw));
        CHECK(same(lw - hw, plw - phw));
        CHECK(same(lw * hw, plw * phw));
        CHECK(same_mask(lanes::less(lw, hw), portable::less(plw, phw)));
        CHECK(same_mask(lanes::greater(lw, hw), portable::greater(plw, phw)));
        CHECK(same(lanes::select(lanes::less(lw, hw), lw, hw),
                   portable::select(portable::less(plw, phw), plw, phw)));
        CHECK(same(lanes::clamp(lw, lanes::splat(-3.0), lanes::splat(4.0)),
                   portable::clamp(plw, portable::splat(-3.0), portable::splat(4.0))));
        ++checked;
    }
    CHECK(checked > 50);

    // Integer round trip within the representable range.
    for (const double x : {0.0, -0.0, 1.9, -1.9, 12345.75, -98765.5}) {
        const lanes::D2 v = lanes::make(x, -x);
        const portable::D2 pv = portable::make(x, -x);
        CHECK(lanes::lane(lanes::to_int(v), 0) == portable::lane(portable::to_int(pv), 0));
        CHECK(lanes::lane(lanes::to_int(v), 1) == portable::lane(portable::to_int(pv), 1));
        CHECK(same(lanes::to_double(lanes::to_int(v)), portable::to_double(portable::to_int(pv))));
        CHECK(lanes::lane_d(v, 1) == portable::lane_d(pv, 1));
    }
    CHECK(same_mask(lanes::mask_of(true, false, true, false),
                    portable::mask_of(true, false, true, false)));
    CHECK(same(lanes::make(1.0f, 2.0f, 3.0f, 4.0f), portable::make(1.0f, 2.0f, 3.0f, 4.0f)));
}

#endif

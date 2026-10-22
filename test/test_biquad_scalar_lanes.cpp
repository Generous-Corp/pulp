// Built in its own binary with PULP_SIMD_DISABLE_VECTOR_LANES, the path a
// compiler without GNU vector extensions (MSVC) takes: the stereo Biquad and
// cascade entry points run each channel as a scalar instead of in vector
// lanes. Each channel must still be bit-identical to processing it alone,
// which the vector-lane build asserts of its lanes too (test_biquad.cpp,
// test_sos_cascade.cpp), so the two builds agree with each other.

#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/biquad.hpp>
#include <pulp/signal/sos_cascade.hpp>
#include <pulp/simd/lanes.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

static_assert(!pulp::simd::kHasVectorLanes,
              "this suite must be compiled with PULP_SIMD_DISABLE_VECTOR_LANES");

namespace {

template <typename T> std::vector<T> busy(std::size_t n, std::uint32_t seed) {
    std::vector<T> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        v[i] = static_cast<T>(0.5 * std::sin(0.0137 * static_cast<double>(i) * (1 + seed % 3)) +
                              0.2 * (static_cast<double>(seed >> 8) / 16777216.0 - 0.5));
    }
    return v;
}

template <typename T> bool same_bits(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

// Sections near the unit circle (low cutoff, high Q) take the sample-order
// form; the rest take the faster forms, so both kinds of pass are covered.
template <typename T>
void design(pulp::signal::BiquadT<T>& b, std::size_t k, bool near_unit_circle, int channel) {
    using Type = typename pulp::signal::BiquadT<T>::Type;
    constexpr Type kTypes[] = {Type::lowpass, Type::highpass, Type::bandpass,  Type::notch,
                               Type::allpass, Type::peaking,  Type::low_shelf, Type::high_shelf};
    const double f = near_unit_circle ? 20.0 + 3.0 * static_cast<double>(k)
                                      : 300.0 + 650.0 * static_cast<double>(k) + 90.0 * channel;
    b.set_coefficients(kTypes[(k + static_cast<std::size_t>(channel)) % 8], static_cast<T>(f),
                       static_cast<T>(near_unit_circle ? 12.0 : 0.9), static_cast<T>(48000),
                       static_cast<T>(channel ? -4 : 4));
}

template <typename T> void check_chains() {
    using B = pulp::signal::BiquadT<T>;
    for (std::size_t count = 1; count <= 20; ++count) {
        for (const bool near : {false, true}) {
            INFO("sections " << count << " near unit circle " << near);
            std::vector<B> left(count), right(count);
            for (std::size_t k = 0; k < count; ++k) {
                design(left[k], k, near, 0);
                design(right[k], k, near, 1);
            }
            std::vector<B> left_alone = left, right_alone = right;
            auto l = busy<T>(3000, static_cast<std::uint32_t>(count * 7 + near));
            auto r = busy<T>(3000, static_cast<std::uint32_t>(count * 11 + near));
            auto l_alone = l, r_alone = r;
            for (std::size_t start = 0; start < l.size(); start += 700) {
                const std::size_t n = std::min<std::size_t>(700, l.size() - start);
                B::process_chain_stereo(left.data(), right.data(), count, l.data() + start,
                                        r.data() + start, n);
                B::process_chain(left_alone.data(), count, l_alone.data() + start, n);
                B::process_chain(right_alone.data(), count, r_alone.data() + start, n);
            }
            CHECK(same_bits(l, l_alone));
            CHECK(same_bits(r, r_alone));
        }
    }
}

template <typename T> void check_single() {
    using B = pulp::signal::BiquadT<T>;
    for (const bool near : {false, true}) {
        B left, right;
        design(left, 2, near, 0);
        design(right, 5, near, 1);
        B left_alone = left, right_alone = right;
        auto l = busy<T>(4096, 3u), r = busy<T>(4096, 5u);
        auto l_alone = l, r_alone = r;
        // Calls below and above the length the vector build runs in lanes.
        for (const std::size_t n :
             {std::size_t{17}, std::size_t{64}, std::size_t{1000}, std::size_t{3015}}) {
            const std::size_t start = n == 17 ? 0 : n == 64 ? 17 : n == 1000 ? 81 : 1081;
            B::process_stereo(left, right, l.data() + start, r.data() + start, n);
            left_alone.process(l_alone.data() + start, static_cast<int>(n));
            right_alone.process(r_alone.data() + start, static_cast<int>(n));
        }
        CHECK(same_bits(l, l_alone));
        CHECK(same_bits(r, r_alone));
    }
}

} // namespace

TEST_CASE("Biquad stereo paths without vector lanes match each channel alone",
          "[signal][biquad][parity]") {
    check_single<float>();
    check_single<double>();
}

TEST_CASE("Biquad chain stereo path without vector lanes matches each chain alone",
          "[signal][biquad][parity]") {
    check_chains<float>();
    check_chains<double>();
}

TEST_CASE("SosCascade stereo path without vector lanes matches each cascade alone",
          "[signal][sos][parity]") {
    pulp::signal::SosCascadeT<float, 8> left, right, left_alone, right_alone;
    for (auto* c : {&left, &right, &left_alone, &right_alone})
        REQUIRE(c->prepare(8));
    std::vector<pulp::signal::BiquadCoefficients> a, b;
    for (std::size_t k = 0; k < 6; ++k) {
        pulp::signal::Biquad d;
        design(d, k, false, 0);
        a.push_back(d.coefficients());
        design(d, k, false, 1);
        b.push_back(d.coefficients());
    }
    REQUIRE(left.set_coefficients(std::span(std::as_const(a))) ==
            pulp::signal::SosCascadeInstallStatus::installed);
    REQUIRE(left_alone.set_coefficients(std::span(std::as_const(a))) ==
            pulp::signal::SosCascadeInstallStatus::installed);
    REQUIRE(right.set_coefficients(std::span(std::as_const(b))) ==
            pulp::signal::SosCascadeInstallStatus::installed);
    REQUIRE(right_alone.set_coefficients(std::span(std::as_const(b))) ==
            pulp::signal::SosCascadeInstallStatus::installed);
    auto l = busy<float>(2048, 9u), r = busy<float>(2048, 13u);
    auto l_alone = l, r_alone = r;
    pulp::signal::SosCascadeT<float, 8>::process_stereo(left, right, l.data(), r.data(), l.size());
    left_alone.process(l_alone.data(), l_alone.size());
    right_alone.process(r_alone.data(), r_alone.size());
    CHECK(same_bits(l, l_alone));
    CHECK(same_bits(r, r_alone));
}

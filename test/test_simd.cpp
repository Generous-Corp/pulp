#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/runtime/simd.hpp>
#include <pulp/signal/simd_buffer.hpp>
#include <vector>
#include <cmath>
#include <numeric>

using namespace pulp::runtime;
using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ── SIMD operations ─────────────────────────────────────────────────────

TEST_CASE("simd_float_lanes returns positive value", "[simd]") {
    REQUIRE(simd_float_lanes() >= 1);
}

TEST_CASE("simd_add matches scalar add", "[simd]") {
    const size_t N = 1023;  // non-power-of-two to test tail handling
    std::vector<float> a(N), b(N), dst(N), expected(N);

    for (size_t i = 0; i < N; ++i) {
        a[i] = static_cast<float>(i) * 0.1f;
        b[i] = static_cast<float>(N - i) * 0.2f;
        expected[i] = a[i] + b[i];
    }

    simd_add(a.data(), b.data(), dst.data(), N);

    for (size_t i = 0; i < N; ++i) {
        REQUIRE_THAT(dst[i], WithinAbs(expected[i], 1e-5));
    }
}

TEST_CASE("simd_mul matches scalar mul", "[simd]") {
    const size_t N = 500;
    std::vector<float> a(N), b(N), dst(N);

    for (size_t i = 0; i < N; ++i) {
        a[i] = static_cast<float>(i) * 0.01f;
        b[i] = 2.0f;
    }

    simd_mul(a.data(), b.data(), dst.data(), N);

    for (size_t i = 0; i < N; ++i) {
        REQUIRE_THAT(dst[i], WithinAbs(a[i] * 2.0f, 1e-5));
    }
}

TEST_CASE("simd_fma matches scalar fma", "[simd]") {
    const size_t N = 256;
    std::vector<float> a(N), b(N), c(N), dst(N);

    for (size_t i = 0; i < N; ++i) {
        a[i] = static_cast<float>(i);
        b[i] = 0.5f;
        c[i] = 1.0f;
    }

    simd_fma(a.data(), b.data(), c.data(), dst.data(), N);

    for (size_t i = 0; i < N; ++i) {
        REQUIRE_THAT(dst[i], WithinAbs(a[i] * 0.5f + 1.0f, 1e-4));
    }
}

TEST_CASE("simd_set fills buffer", "[simd]") {
    const size_t N = 100;
    std::vector<float> dst(N, -1.0f);

    simd_set(3.14f, dst.data(), N);

    for (size_t i = 0; i < N; ++i) {
        REQUIRE_THAT(dst[i], WithinAbs(3.14f, 1e-6));
    }
}

TEST_CASE("simd_scale multiplies by scalar", "[simd]") {
    const size_t N = 200;
    std::vector<float> a(N), dst(N);

    for (size_t i = 0; i < N; ++i)
        a[i] = static_cast<float>(i);

    simd_scale(a.data(), 0.5f, dst.data(), N);

    for (size_t i = 0; i < N; ++i) {
        REQUIRE_THAT(dst[i], WithinAbs(static_cast<float>(i) * 0.5f, 1e-5));
    }
}

TEST_CASE("simd_reduce_add sums elements", "[simd]") {
    const size_t N = 1000;
    std::vector<float> data(N);

    for (size_t i = 0; i < N; ++i)
        data[i] = 1.0f;

    float sum = simd_reduce_add(data.data(), N);
    REQUIRE_THAT(sum, WithinAbs(1000.0f, 0.1f));
}

TEST_CASE("simd_reduce_max finds maximum", "[simd]") {
    const size_t N = 512;
    std::vector<float> data(N);

    for (size_t i = 0; i < N; ++i)
        data[i] = static_cast<float>(i) - 256.0f;

    float mx = simd_reduce_max(data.data(), N);
    REQUIRE_THAT(mx, WithinAbs(255.0f, 1e-5));
}

TEST_CASE("simd_reduce_min finds minimum", "[simd]") {
    const size_t N = 512;
    std::vector<float> data(N);

    for (size_t i = 0; i < N; ++i)
        data[i] = static_cast<float>(i) - 256.0f;

    float mn = simd_reduce_min(data.data(), N);
    REQUIRE_THAT(mn, WithinAbs(-256.0f, 1e-5));
}

TEST_CASE("simd_abs computes absolute value", "[simd]") {
    const size_t N = 100;
    std::vector<float> a(N), dst(N);

    for (size_t i = 0; i < N; ++i)
        a[i] = (i % 2 == 0) ? static_cast<float>(i) : -static_cast<float>(i);

    simd_abs(a.data(), dst.data(), N);

    for (size_t i = 0; i < N; ++i) {
        REQUIRE_THAT(dst[i], WithinAbs(static_cast<float>(i), 1e-6));
    }
}

TEST_CASE("simd_clamp clamps values", "[simd]") {
    const size_t N = 100;
    std::vector<float> a(N), dst(N);

    for (size_t i = 0; i < N; ++i)
        a[i] = static_cast<float>(i) - 50.0f;

    simd_clamp(a.data(), -10.0f, 10.0f, dst.data(), N);

    for (size_t i = 0; i < N; ++i) {
        float expected = std::clamp(a[i], -10.0f, 10.0f);
        REQUIRE_THAT(dst[i], WithinAbs(expected, 1e-6));
    }
}

TEST_CASE("simd operations handle empty input", "[simd]") {
    float dummy = 0.0f;
    simd_add(&dummy, &dummy, &dummy, 0);
    simd_mul(&dummy, &dummy, &dummy, 0);
    REQUIRE(simd_reduce_add(&dummy, 0) == 0.0f);
}

TEST_CASE("simd operations handle count=1", "[simd]") {
    float a = 3.0f, b = 4.0f, dst = 0.0f;
    simd_add(&a, &b, &dst, 1);
    REQUIRE_THAT(dst, WithinAbs(7.0f, 1e-6));
}

// ── AlignedBuffer ───────────────────────────────────────────────────────

TEST_CASE("AlignedBuffer default construction", "[simd][aligned_buffer]") {
    AlignedBuffer buf;
    REQUIRE(buf.empty());
    REQUIRE(buf.size() == 0);
    REQUIRE(buf.data() == nullptr);
}

TEST_CASE("AlignedBuffer allocates and zeros", "[simd][aligned_buffer]") {
    AlignedBuffer buf(256);
    REQUIRE(buf.size() == 256);
    REQUIRE(buf.data() != nullptr);

    // Check alignment
    REQUIRE(reinterpret_cast<uintptr_t>(buf.data()) % kSimdAlignment == 0);

    // Check zeroed
    for (size_t i = 0; i < 256; ++i)
        REQUIRE(buf[i] == 0.0f);
}

TEST_CASE("AlignedBuffer move semantics", "[simd][aligned_buffer]") {
    AlignedBuffer a(100);
    a[0] = 42.0f;

    AlignedBuffer b = std::move(a);
    REQUIRE(a.data() == nullptr);
    REQUIRE(a.size() == 0);
    REQUIRE(b.size() == 100);
    REQUIRE(b[0] == 42.0f);
}

TEST_CASE("AlignedBuffer move assignment releases prior storage",
          "[simd][aligned_buffer][issue-645]") {
    AlignedBuffer source(3);
    source[0] = 1.0f;
    source[1] = 2.0f;
    source[2] = 3.0f;

    AlignedBuffer dest(2);
    dest[0] = -1.0f;
    dest = std::move(source);

    REQUIRE(source.data() == nullptr);
    REQUIRE(source.empty());
    REQUIRE(dest.size() == 3);
    REQUIRE_THAT(dest[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(dest[1], WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(dest[2], WithinAbs(3.0f, 1e-6f));
}

TEST_CASE("AlignedBuffer resize", "[simd][aligned_buffer]") {
    AlignedBuffer buf(50);
    buf[0] = 1.0f;

    buf.resize(200);
    REQUIRE(buf.size() == 200);
    REQUIRE(reinterpret_cast<uintptr_t>(buf.data()) % kSimdAlignment == 0);

    // Data not preserved after resize
    buf.resize(0);
    REQUIRE(buf.empty());
}

TEST_CASE("AlignedBuffer resize same size and empty clear are no-ops",
          "[simd][aligned_buffer][issue-645]") {
    AlignedBuffer empty;
    empty.clear();
    empty.resize(0);
    REQUIRE(empty.empty());

    AlignedBuffer buf(4);
    auto* original = buf.data();
    buf[0] = 9.0f;
    buf.resize(4);
    REQUIRE(buf.data() == original);
    REQUIRE_THAT(buf[0], WithinAbs(9.0f, 1e-6f));
}

TEST_CASE("AlignedBuffer clear", "[simd][aligned_buffer]") {
    AlignedBuffer buf(100);
    for (size_t i = 0; i < 100; ++i)
        buf[i] = static_cast<float>(i);

    buf.clear();

    for (size_t i = 0; i < 100; ++i)
        REQUIRE(buf[i] == 0.0f);
}

TEST_CASE("AlignedBuffer copy_from", "[simd][aligned_buffer]") {
    float src[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    AlignedBuffer buf(5);
    buf.copy_from(src, 5);

    for (size_t i = 0; i < 5; ++i)
        REQUIRE(buf[i] == src[i]);
}

TEST_CASE("AlignedBuffer copy_from truncates to destination size",
          "[simd][aligned_buffer][issue-645]") {
    const float src[] = {10.0f, 20.0f, 30.0f, 40.0f};
    AlignedBuffer buf(2);
    buf.copy_from(src, 4);

    REQUIRE_THAT(buf[0], WithinAbs(10.0f, 1e-6f));
    REQUIRE_THAT(buf[1], WithinAbs(20.0f, 1e-6f));

    const AlignedBuffer& const_buf = buf;
    REQUIRE(const_buf.begin() + 2 == const_buf.end());
}

TEST_CASE("AlignedBuffer works with simd operations", "[simd][aligned_buffer]") {
    AlignedBuffer a(256), b(256), dst(256);

    for (size_t i = 0; i < 256; ++i) {
        a[i] = static_cast<float>(i);
        b[i] = 1.0f;
    }

    simd_add(a.data(), b.data(), dst.data(), 256);

    for (size_t i = 0; i < 256; ++i) {
        REQUIRE_THAT(dst[i], WithinAbs(static_cast<float>(i) + 1.0f, 1e-5));
    }
}

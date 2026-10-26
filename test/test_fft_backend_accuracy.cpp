// FftT accuracy and scaling against a double-precision reference, for the
// backend this binary is built with. The same source is compiled once per
// backend (native: vDSP on Apple, muFFT elsewhere; muFFT forced; built-in
// radix-2 forced), so every backend that builds on a host is held to the same
// contract: forward bins, inverse scaling, the real half-spectrum and packed
// split layouts, in-place use, and no allocation per transform.
#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/fft.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

using namespace pulp::signal;

namespace {

using cd = std::complex<double>;

// Radix-2 FFT in double with long-double twiddles: about 1e-15 relative
// error, far below either precision under test.
std::vector<cd> reference_dft(std::vector<cd> a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        for (std::size_t k = 0; k < len / 2; ++k) {
            const long double angle = (inverse ? 2.0L : -2.0L) *
                                      3.141592653589793238462643383279502884L *
                                      static_cast<long double>(k) / static_cast<long double>(len);
            const cd w(static_cast<double>(std::cos(angle)), static_cast<double>(std::sin(angle)));
            for (std::size_t i = 0; i < n; i += len) {
                const cd u = a[i + k];
                const cd v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
    return a;
}

template <typename T> std::vector<T> test_signal(int count, std::uint32_t seed) {
    std::vector<T> x(static_cast<std::size_t>(count));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<T>(static_cast<double>(static_cast<std::int32_t>(seed) >> 8) / 8388608.0);
    }
    return x;
}

// max |got - want| / max |want|.
template <typename Got> double relative_error(const Got& got, const std::vector<cd>& want) {
    double err = 0.0;
    double peak = 0.0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        err = std::max(err, std::abs(cd(got[i]) - want[i]));
        peak = std::max(peak, std::abs(want[i]));
    }
    return err / peak;
}

template <typename T> constexpr double tolerance() {
    // Float transforms land at -124 to -144 dB of the largest bin on every
    // backend measured; 2e-6 (-114 dB) leaves headroom without hiding a
    // scaling, ordering or twiddle mistake, all of which cost O(1).
    return std::is_same_v<T, float> ? 2.0e-6 : 1.0e-12;
}

template <typename T> void check_complex(int n) {
    INFO("complex N=" << n);
    FftT<T> fft(n);
    REQUIRE(fft.ready());
    const auto raw = test_signal<T>(2 * n, 17u + static_cast<std::uint32_t>(n));
    std::vector<std::complex<T>> data(static_cast<std::size_t>(n));
    std::vector<cd> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        data[static_cast<std::size_t>(i)] = {raw[2 * static_cast<std::size_t>(i)],
                                             raw[2 * static_cast<std::size_t>(i) + 1]};
        x[static_cast<std::size_t>(i)] = cd(data[static_cast<std::size_t>(i)]);
    }
    const auto spectrum = reference_dft(x, false);

    auto y = data;
    fft.forward(y.data());
    CHECK(relative_error(y, spectrum) < tolerance<T>());

    // Inverse of the exact spectrum, normalised by 1/N, recovers x.
    std::vector<std::complex<T>> s(static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < s.size(); ++i)
        s[i] = std::complex<T>(spectrum[i]);
    fft.inverse(s.data());
    CHECK(relative_error(s, x) < tolerance<T>());

    fft.inverse(y.data());
    CHECK(relative_error(y, x) < 2.0 * tolerance<T>());
}

template <typename T> void check_real(int n) {
    INFO("real N=" << n);
    FftT<T> fft(n);
    REQUIRE(fft.ready());
    const int half = n / 2;
    const auto input = test_signal<T>(n, 91u + static_cast<std::uint32_t>(n));
    std::vector<cd> x(input.begin(), input.end());
    const auto full = reference_dft(x, false);
    const std::vector<cd> bins(full.begin(), full.begin() + half + 1);

    std::vector<std::complex<T>> out(static_cast<std::size_t>(n));
    fft.forward_real(input.data(), out.data());
    CHECK(relative_error(out, full) < tolerance<T>());

    const T scale = T(0.25);
    std::vector<std::complex<T>> half_bins(static_cast<std::size_t>(half + 1), {T(9), T(9)});
    fft.forward_real_half(input.data(), half_bins.data(), scale);
    std::vector<cd> scaled_bins(bins);
    for (auto& b : scaled_bins)
        b *= double(scale);
    CHECK(relative_error(half_bins, scaled_bins) < tolerance<T>());
    CHECK(half_bins.front().imag() == T(0));
    CHECK(half_bins.back().imag() == T(0));

    // Default inverse scale 1/N recovers x; a custom scale multiplies the
    // unnormalised inverse DFT.
    std::vector<std::complex<T>> exact(static_cast<std::size_t>(half + 1));
    for (std::size_t k = 0; k < exact.size(); ++k)
        exact[k] = std::complex<T>(bins[k]);
    std::vector<T> back(static_cast<std::size_t>(n));
    fft.inverse_real_half(exact.data(), back.data());
    CHECK(relative_error(back, x) < tolerance<T>());
    fft.inverse_real_half(exact.data(), back.data(), T(2));
    std::vector<cd> scaled_x(x);
    for (auto& v : scaled_x)
        v *= 2.0 * n;
    CHECK(relative_error(back, scaled_x) < tolerance<T>());

    // In place: a complex buffer of N/2 + 1 bins read back as N reals.
    if (n >= 4) {
        std::vector<std::complex<T>> in_place(exact);
        fft.inverse_real_half(in_place.data(), reinterpret_cast<T*>(in_place.data()));
        CHECK(relative_error(reinterpret_cast<const T*>(in_place.data()), x) < tolerance<T>());
    }

    // Packed split layout: re[0] = DC, im[0] = Nyquist, then bins 1..N/2-1.
    std::vector<T> re(static_cast<std::size_t>(half)), im(static_cast<std::size_t>(half));
    detail::FftRealSplit::forward(fft, input.data(), re.data(), im.data(), scale);
    std::vector<cd> packed(static_cast<std::size_t>(half));
    std::vector<cd> got(static_cast<std::size_t>(half));
    for (int k = 0; k < half; ++k) {
        packed[static_cast<std::size_t>(k)] = scaled_bins[static_cast<std::size_t>(k)];
        got[static_cast<std::size_t>(k)] = {double(re[static_cast<std::size_t>(k)]),
                                            double(im[static_cast<std::size_t>(k)])};
    }
    packed[0] = {scaled_bins[0].real(), scaled_bins[static_cast<std::size_t>(half)].real()};
    CHECK(relative_error(got, packed) < tolerance<T>());
    std::vector<T> split_back(static_cast<std::size_t>(n));
    detail::FftRealSplit::inverse(fft, re.data(), im.data(), split_back.data(),
                                  T(1) / (scale * static_cast<T>(n)));
    CHECK(relative_error(split_back, x) < 2.0 * tolerance<T>());
}

std::vector<int> sizes() {
    std::vector<int> n;
    for (int s = 2; s <= 16384; s *= 2)
        n.push_back(s);
    return n;
}

} // namespace

TEST_CASE("FftT float matches a double reference at every power-of-two size",
          "[signal][fft][accuracy]") {
    for (int n : sizes()) {
        check_complex<float>(n);
        check_real<float>(n);
    }
}

TEST_CASE("FftT double matches a double reference at every power-of-two size",
          "[signal][fft][accuracy]") {
    for (int n : sizes()) {
        check_complex<double>(n);
        check_real<double>(n);
    }
}

TEST_CASE("FftT backend charge and per-transform allocation", "[signal][fft][rt-safety]") {
    std::uint64_t bytes = 1;
    REQUIRE(checked_fft_platform_setup_bytes<float>(1024, UINT64_MAX, bytes));
#if PULP_FFT_HAS_VDSP
    CHECK(bytes == 1024u * kVdspFftSetupChargeBytesPerPoint);
#elif PULP_FFT_USE_MUFFT
    // The plan exists for planned sizes, and its charge covers what it holds.
    CHECK(bytes == pulp_mufft_retained_bytes(1024));
    CHECK(bytes > 1024u * 8u);
    CHECK(pulp_mufft_retained_bytes(8) == 0);
    pulp_mufft_plan* plan = pulp_mufft_create(1024);
    REQUIRE(plan != nullptr);
    pulp_mufft_destroy(plan);
    CHECK(pulp_mufft_create(8) == nullptr);
    CHECK(pulp_mufft_create(1000) == nullptr);
#else
    CHECK(bytes == 0);
#endif

    // Every float transform, through aligned buffers (the direct path) and
    // buffers one float off alignment (the staged path). Neither may allocate:
    // the probe sees C++ allocation, and the backend's own counter sees every
    // malloc/calloc inside the shim and muFFT.
    Fft fft(1024);
    std::vector<float> pool(8 * 1024 + 64);
    float* base = pool.data() +
                  (64 - reinterpret_cast<std::uintptr_t>(pool.data()) % 64) % 64 / sizeof(float);
    for (const std::size_t skew : {std::size_t{0}, std::size_t{1}}) {
        INFO("buffer offset " << skew << " floats from a 64-byte boundary");
        auto* c = reinterpret_cast<std::complex<float>*>(base + skew);
        float* r = base + 2 * 1024 + 16 + skew;
        float* re = base + 4 * 1024 + 32 + skew;
        float* im = base + 5 * 1024 + 48 + skew;
        for (int i = 0; i < 1024; ++i)
            r[i] = std::sin(0.01f * static_cast<float>(i));
#if PULP_FFT_USE_MUFFT
        const auto backend_allocations = pulp_mufft_allocations();
#endif
        pulp::test::RtAllocationProbe probe;
        fft.forward(c);
        fft.inverse(c);
        fft.forward_real(r, c);
        fft.forward_real_half(r, c, 1.0f);
        fft.inverse_real_half(c, r);
        fft.forward_real_half(r, c, 0.5f);
        fft.inverse_real_half(c, r, 2.0f / 1024.0f);
        detail::FftRealSplit::forward(fft, r, re, im, 1.0f);
        detail::FftRealSplit::inverse(fft, re, im, r, 1.0f / 1024.0f);
        CHECK_FALSE(probe.saw_allocation());
#if PULP_FFT_USE_MUFFT
        CHECK(pulp_mufft_allocations() == backend_allocations);
#endif
    }
}

TEST_CASE("FftT gives identical results through aligned and unaligned buffers",
          "[signal][fft][accuracy]") {
    // On muFFT, aligned buffers go straight to the kernels and unaligned
    // ones are staged; the arithmetic must be the same either way.
    constexpr int n = 2048;
    Fft fft(n);
    std::vector<float> pool(8 * n + 64);
    float* base = pool.data() +
                  (64 - reinterpret_cast<std::uintptr_t>(pool.data()) % 64) % 64 / sizeof(float);
    const auto x = test_signal<float>(2 * n, 5u);
    std::vector<float> results[2][4];
    for (int skew = 0; skew < 2; ++skew) {
        auto* c = reinterpret_cast<std::complex<float>*>(base + skew);
        float* r = base + 4 * n + skew;
        std::copy(x.begin(), x.end(), reinterpret_cast<float*>(c));
        fft.forward(c);
        results[skew][0].assign(reinterpret_cast<float*>(c), reinterpret_cast<float*>(c) + 2 * n);
        fft.inverse(c);
        results[skew][1].assign(reinterpret_cast<float*>(c), reinterpret_cast<float*>(c) + 2 * n);
        std::copy(x.begin(), x.begin() + n, r);
        fft.forward_real_half(r, c, 0.5f);
        results[skew][2].assign(reinterpret_cast<float*>(c), reinterpret_cast<float*>(c) + n + 2);
        fft.inverse_real_half(c, r);
        results[skew][3].assign(r, r + n);
    }
    for (int k = 0; k < 4; ++k) {
        INFO("transform " << k);
        CHECK(results[0][k] == results[1][k]);
    }
}

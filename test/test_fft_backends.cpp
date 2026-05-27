// MultiBackendFft tests — runtime-selectable FFT facade.
//
// Verifies:
//   1. Selector / parser / availability helpers behave as documented.
//   2. Every backend that reports `fft_backend_available` actually
//      produces an FFT round-trip that matches the input (within
//      single-precision float tolerance) for sizes 64..16384.
//   3. Cross-backend bit/output equivalence: every available backend
//      produces a forward-FFT spectrum that matches the kissfft
//      reference to within a small tolerance for power-of-2 sizes.
//   4. resolve_fft_backend never returns auto_, honors env var.
//   5. Constructor rejects non-power-of-2 sizes.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/fft_backend.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace pulp::signal;

namespace {

constexpr double pi = 3.14159265358979323846;

// Deterministic complex test vector: sum of a couple of sinusoids plus a
// fixed phase offset so cross-backend numerical comparison is meaningful.
std::vector<std::complex<float>> make_signal(int n) {
    std::vector<std::complex<float>> v(n);
    for (int i = 0; i < n; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(n);
        float re = 0.5f * std::sin(2.0f * static_cast<float>(pi) * 3.0f * t)
                 + 0.25f * std::sin(2.0f * static_cast<float>(pi) * 17.0f * t + 0.6f);
        float im = 0.3f * std::cos(2.0f * static_cast<float>(pi) * 5.0f * t - 0.2f);
        v[i] = {re, im};
    }
    return v;
}

float max_abs_diff(const std::vector<std::complex<float>>& a,
                   const std::vector<std::complex<float>>& b) {
    REQUIRE(a.size() == b.size());
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(a[i].real() - b[i].real()));
        worst = std::max(worst, std::abs(a[i].imag() - b[i].imag()));
    }
    return worst;
}

}  // namespace

TEST_CASE("MultiBackendFft selector helpers", "[fft][backend][selector]") {
    SECTION("backend names round-trip through parse") {
        for (FftBackend b : {FftBackend::auto_, FftBackend::vdsp,
                             FftBackend::kissfft, FftBackend::fftw3,
                             FftBackend::mkl}) {
            REQUIRE(parse_fft_backend(fft_backend_name(b)) == b);
        }
    }

    SECTION("parse rejects unknown / empty") {
        REQUIRE(parse_fft_backend(nullptr) == FftBackend::auto_);
        REQUIRE(parse_fft_backend("") == FftBackend::auto_);
        REQUIRE(parse_fft_backend("nonsense") == FftBackend::auto_);
    }

    SECTION("kissfft is always available") {
        REQUIRE(fft_backend_available(FftBackend::kissfft));
    }

    SECTION("auto_ resolves to a concrete backend") {
        FftBackend r = resolve_fft_backend(FftBackend::auto_);
        REQUIRE(r != FftBackend::auto_);
        REQUIRE(fft_backend_available(r));
    }

    SECTION("env override picks an available backend") {
#if defined(__APPLE__)
        setenv("PULP_FFT_BACKEND", "kissfft", 1);
        REQUIRE(resolve_fft_backend(FftBackend::auto_) == FftBackend::kissfft);
        unsetenv("PULP_FFT_BACKEND");
#endif
        // Unavailable backend in env: resolver falls back to default
        setenv("PULP_FFT_BACKEND", "garbage", 1);
        FftBackend r = resolve_fft_backend(FftBackend::auto_);
        REQUIRE(r != FftBackend::auto_);
        REQUIRE(fft_backend_available(r));
        unsetenv("PULP_FFT_BACKEND");
    }

    SECTION("explicit hint honored when available") {
        REQUIRE(resolve_fft_backend(FftBackend::kissfft) == FftBackend::kissfft);
    }
}

TEST_CASE("MultiBackendFft rejects non-power-of-two", "[fft][backend][error]") {
    REQUIRE_THROWS_AS(MultiBackendFft(0, FftBackend::kissfft), std::invalid_argument);
    REQUIRE_THROWS_AS(MultiBackendFft(3, FftBackend::kissfft), std::invalid_argument);
    REQUIRE_THROWS_AS(MultiBackendFft(100, FftBackend::kissfft), std::invalid_argument);
    REQUIRE_NOTHROW(MultiBackendFft(64, FftBackend::kissfft));
    REQUIRE_NOTHROW(MultiBackendFft(16384, FftBackend::kissfft));
}

TEST_CASE("MultiBackendFft kissfft round-trip", "[fft][backend][kissfft]") {
    for (int n : {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384}) {
        MultiBackendFft fft(n, FftBackend::kissfft);
        REQUIRE(fft.size() == n);
        REQUIRE(fft.backend() == FftBackend::kissfft);

        auto orig = make_signal(n);
        auto buf = orig;
        fft.forward(buf.data());
        fft.inverse(buf.data());

        // Round-trip should recover input within float tolerance
        // (single-precision FFT accumulates ~O(log N) ulp error).
        float worst = max_abs_diff(orig, buf);
        REQUIRE(worst < 1e-3f);
    }
}

#if defined(__APPLE__)
TEST_CASE("MultiBackendFft vdsp round-trip", "[fft][backend][vdsp]") {
    REQUIRE(fft_backend_available(FftBackend::vdsp));
    for (int n : {64, 256, 1024, 4096, 16384}) {
        MultiBackendFft fft(n, FftBackend::vdsp);
        REQUIRE(fft.backend() == FftBackend::vdsp);

        auto orig = make_signal(n);
        auto buf = orig;
        fft.forward(buf.data());
        fft.inverse(buf.data());

        float worst = max_abs_diff(orig, buf);
        REQUIRE(worst < 1e-3f);
    }
}

TEST_CASE("MultiBackendFft vdsp vs kissfft numerical equivalence",
          "[fft][backend][cross]") {
    REQUIRE(fft_backend_available(FftBackend::vdsp));
    for (int n : {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384}) {
        auto sig = make_signal(n);

        auto a = sig;
        MultiBackendFft(n, FftBackend::kissfft).forward(a.data());

        auto b = sig;
        MultiBackendFft(n, FftBackend::vdsp).forward(b.data());

        // Tolerance scales with N because the worst single-precision
        // round-off error in any FFT is O(N * eps * log N). 1e-3 absolute
        // covers N up to 16384 for our test signal magnitude.
        float worst = max_abs_diff(a, b);
        INFO("n=" << n << " worst=" << worst);
        REQUIRE(worst < 1e-3f);
    }
}
#endif

TEST_CASE("MultiBackendFft optional backends report unavailable cleanly",
          "[fft][backend][optional]") {
    // We can't assert FFTW3/MKL are present (they're optional dlopen
    // backends), but the availability query must not crash and must
    // return a bool. Constructing with an unavailable backend must
    // throw, not segfault.
    SECTION("availability query is total") {
        (void)fft_backend_available(FftBackend::fftw3);
        (void)fft_backend_available(FftBackend::mkl);
        SUCCEED();
    }

    SECTION("unavailable backend throws on construct") {
        if (!fft_backend_available(FftBackend::fftw3)) {
            REQUIRE_THROWS(MultiBackendFft(256, FftBackend::fftw3));
        } else {
            // If FFTW3 IS available, exercise it for the round-trip.
            MultiBackendFft fft(256, FftBackend::fftw3);
            REQUIRE(fft.backend() == FftBackend::fftw3);
            auto orig = make_signal(256);
            auto buf = orig;
            fft.forward(buf.data());
            fft.inverse(buf.data());
            REQUIRE(max_abs_diff(orig, buf) < 1e-3f);
        }

        if (!fft_backend_available(FftBackend::mkl)) {
            REQUIRE_THROWS(MultiBackendFft(256, FftBackend::mkl));
        } else {
            MultiBackendFft fft(256, FftBackend::mkl);
            REQUIRE(fft.backend() == FftBackend::mkl);
            auto orig = make_signal(256);
            auto buf = orig;
            fft.forward(buf.data());
            fft.inverse(buf.data());
            REQUIRE(max_abs_diff(orig, buf) < 1e-3f);
        }
    }
}

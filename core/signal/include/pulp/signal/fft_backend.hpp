#pragma once

// Multi-backend FFT — runtime-selectable wrapper around pulp::signal::Fft.
//
// Backends:
//   - vdsp     : Apple Accelerate.framework (always available on Apple)
//   - kissfft  : built-in radix-2 fallback (always available, header-only)
//                pulp-doc: "kissfft" is the historical name in the gap-doc
//                multi-engine FFT row; our shipping fallback is the hand-rolled
//                radix-2 Cooley-Tukey in pulp::signal::Fft, exposed under this
//                stable alias so callers don't have to track implementation
//                changes.
//   - fftw3   : libfftw3f loaded via dlopen at runtime (optional)
//   - mkl     : Intel MKL libmkl_rt loaded via dlopen at runtime (optional)
//
// Both fftw3 and mkl are loaded lazily — no link-time dependency. If the
// shared library is not installed on the runtime machine, the selector
// falls back to vDSP (on Apple) or kissfft.
//
// Backend selection precedence (highest wins):
//   1. Explicit MultiBackendFft(size, FftBackend::X) constructor
//   2. env var PULP_FFT_BACKEND={auto,vdsp,kissfft,fftw3,mkl}
//   3. compile-time default: vDSP on Apple, kissfft elsewhere
//
// Numerical equivalence: cross-backend output matches to within a small
// float tolerance (single-precision round-off accumulates with size). The
// test suite asserts max-abs-error < ~1e-3 across sizes 64..16384 for
// power-of-2 transforms.

#include <complex>
#include <memory>
#include <string>

namespace pulp::signal {

enum class FftBackend {
    auto_,    ///< Pick best available at runtime
    vdsp,     ///< Apple Accelerate vDSP (Apple only)
    kissfft,  ///< Built-in radix-2 fallback (always available)
    fftw3,    ///< FFTW3 single-precision (runtime-dlopen)
    mkl,      ///< Intel MKL DFTI (runtime-dlopen)
};

/// Stringify a backend tag for diagnostics / env-var parsing.
const char* fft_backend_name(FftBackend b) noexcept;

/// Parse a backend tag from a string. Unknown / empty → auto_.
FftBackend parse_fft_backend(const char* s) noexcept;

/// True iff the backend can be instantiated on this machine right now.
/// vdsp is only available on Apple; fftw3/mkl require the shared lib
/// to be loadable via dlopen at runtime.
bool fft_backend_available(FftBackend b) noexcept;

/// Resolve auto_ → best available backend on this machine, honoring the
/// PULP_FFT_BACKEND env var if set to a concrete backend that is
/// available. Never returns FftBackend::auto_.
FftBackend resolve_fft_backend(FftBackend hint = FftBackend::auto_) noexcept;

/// Multi-backend FFT. Pinned at construction to a single backend so the
/// audio thread never has to branch on backend selection during process().
class MultiBackendFft {
public:
    /// Construct for a given power-of-2 size. backend=auto_ picks via
    /// resolve_fft_backend(). Throws std::invalid_argument if size is
    /// not a power of two, or std::runtime_error if the requested
    /// backend cannot be initialised.
    explicit MultiBackendFft(int size, FftBackend backend = FftBackend::auto_);
    ~MultiBackendFft();

    MultiBackendFft(const MultiBackendFft&) = delete;
    MultiBackendFft& operator=(const MultiBackendFft&) = delete;
    MultiBackendFft(MultiBackendFft&&) noexcept;
    MultiBackendFft& operator=(MultiBackendFft&&) noexcept;

    int size() const noexcept;
    FftBackend backend() const noexcept;

    /// Forward complex FFT (in-place).
    void forward(std::complex<float>* data) const;

    /// Inverse complex FFT (in-place), normalised by 1/N.
    void inverse(std::complex<float>* data) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::signal

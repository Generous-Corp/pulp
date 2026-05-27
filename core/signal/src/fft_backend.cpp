#include "pulp/signal/fft_backend.hpp"

#include "pulp/signal/fft.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#define PULP_FFT_BACKEND_HAS_VDSP 1
#endif

namespace pulp::signal {

// ── Helpers ────────────────────────────────────────────────────────────────

namespace {

bool is_power_of_two(int n) noexcept {
    return n > 0 && (n & (n - 1)) == 0;
}

int log2_int(int n) noexcept {
    int k = 0;
    while ((1 << k) < n) ++k;
    return k;
}

// dlopen wrapper that returns nullptr on every failure path. Tries each
// candidate name in order. We hold the handle for the process lifetime —
// once a backend is opened, leave it open (no dlclose).
void* try_dlopen(const std::vector<const char*>& names) noexcept {
#if defined(_WIN32)
    (void)names;
    return nullptr;  // backend dlopen not wired on Windows yet
#else
    for (const char* n : names) {
        if (void* h = dlopen(n, RTLD_LAZY | RTLD_LOCAL)) return h;
    }
    return nullptr;
#endif
}

template <typename Fn>
Fn dlsym_cast(void* handle, const char* name) noexcept {
#if defined(_WIN32)
    (void)handle; (void)name;
    return nullptr;
#else
    return reinterpret_cast<Fn>(dlsym(handle, name));
#endif
}

}  // namespace

// ── FFTW3 dynamic binding ──────────────────────────────────────────────────
//
// We bind libfftw3f (single-precision) at runtime. The public FFTW3 API is
// stable across versions back to 3.0 for the functions we use. Constants
// FFTW_FORWARD = -1, FFTW_BACKWARD = +1, FFTW_ESTIMATE = 1u<<6 are part of
// the ABI and stable.

namespace fftw3_dyn {

using plan_t = void*;
using fftwf_complex = float[2];

using fn_plan_dft_1d = plan_t (*)(int, fftwf_complex*, fftwf_complex*, int, unsigned);
using fn_execute     = void (*)(plan_t);
using fn_destroy     = void (*)(plan_t);
using fn_malloc      = void* (*)(size_t);
using fn_free        = void (*)(void*);

struct Bindings {
    void* handle = nullptr;
    fn_plan_dft_1d plan_dft_1d = nullptr;
    fn_execute     execute     = nullptr;
    fn_destroy     destroy     = nullptr;
    fn_malloc      malloc_     = nullptr;
    fn_free        free_       = nullptr;
    bool ok = false;
};

static const Bindings& get() noexcept {
    static const Bindings bindings = [] {
        Bindings b;
        b.handle = try_dlopen({
            "libfftw3f.so.3",
            "libfftw3f.so",
            "libfftw3f.3.dylib",
            "libfftw3f.dylib",
        });
        if (!b.handle) return b;
        b.plan_dft_1d = dlsym_cast<fn_plan_dft_1d>(b.handle, "fftwf_plan_dft_1d");
        b.execute     = dlsym_cast<fn_execute>(b.handle, "fftwf_execute");
        b.destroy     = dlsym_cast<fn_destroy>(b.handle, "fftwf_destroy_plan");
        b.malloc_     = dlsym_cast<fn_malloc>(b.handle, "fftwf_malloc");
        b.free_       = dlsym_cast<fn_free>(b.handle, "fftwf_free");
        b.ok = b.plan_dft_1d && b.execute && b.destroy && b.malloc_ && b.free_;
        return b;
    }();
    return bindings;
}

}  // namespace fftw3_dyn

// ── Intel MKL dynamic binding ──────────────────────────────────────────────
//
// We bind libmkl_rt (the single-DLL "runtime" wrapper that auto-dispatches
// to the appropriate ILP/LP64 layer + thread layer). The DFTI API we use is
// stable across all MKL 2018+ releases.

namespace mkl_dyn {

using DFTI_DESCRIPTOR_HANDLE = void*;
using MKL_LONG = long;

// DFTI enum values from mkl_dfti.h — stable ABI. Only the ones we
// actually pass to DftiCreateDescriptor / DftiSetValue are referenced;
// the others (DFTI_FORWARD_DOMAIN / DFTI_PRECISION / DFTI_PLACEMENT /
// DFTI_INPLACE) are listed in MKL's header for completeness but our
// in-place complex-to-complex single-precision use already matches the
// MKL defaults so we don't need to set them.
constexpr int DFTI_BACKWARD_SCALE = 5;
constexpr int DFTI_SINGLE         = 35;
constexpr int DFTI_COMPLEX        = 32;

using fn_create  = MKL_LONG (*)(DFTI_DESCRIPTOR_HANDLE*, int, int, MKL_LONG, MKL_LONG);
// DftiSetValue is a variadic MKL API. Calling a variadic function through a
// fixed-signature function pointer is undefined behavior per C/C++ standard
// (mismatched ABI, no default-argument promotion guarantees). We MUST keep
// the variadic prototype on the function pointer we call through, and let
// the compiler handle argument promotion at the call site (float → double).
// Regression: Codex PR #3021 review.
using fn_set     = MKL_LONG (*)(DFTI_DESCRIPTOR_HANDLE, int, ...);
using fn_commit  = MKL_LONG (*)(DFTI_DESCRIPTOR_HANDLE);
using fn_compute = MKL_LONG (*)(DFTI_DESCRIPTOR_HANDLE, void*);
using fn_free    = MKL_LONG (*)(DFTI_DESCRIPTOR_HANDLE*);

struct Bindings {
    void* handle = nullptr;
    fn_create  create  = nullptr;
    fn_set     set     = nullptr;
    fn_commit  commit  = nullptr;
    fn_compute fwd     = nullptr;
    fn_compute bwd     = nullptr;
    fn_free    free_   = nullptr;
    bool ok = false;
};

static const Bindings& get() noexcept {
    static const Bindings bindings = [] {
        Bindings b;
        b.handle = try_dlopen({
            "libmkl_rt.so.2",
            "libmkl_rt.so.1",
            "libmkl_rt.so",
            "libmkl_rt.2.dylib",
            "libmkl_rt.1.dylib",
            "libmkl_rt.dylib",
        });
        if (!b.handle) return b;
        b.create  = dlsym_cast<fn_create>(b.handle, "DftiCreateDescriptor");
        b.set     = dlsym_cast<fn_set>(b.handle, "DftiSetValue");
        b.commit  = dlsym_cast<fn_commit>(b.handle, "DftiCommitDescriptor");
        b.fwd     = dlsym_cast<fn_compute>(b.handle, "DftiComputeForward");
        b.bwd     = dlsym_cast<fn_compute>(b.handle, "DftiComputeBackward");
        b.free_   = dlsym_cast<fn_free>(b.handle, "DftiFreeDescriptor");
        b.ok = b.create && b.set && b.commit && b.fwd && b.bwd && b.free_;
        return b;
    }();
    return bindings;
}

}  // namespace mkl_dyn

// ── Public selector helpers ────────────────────────────────────────────────

const char* fft_backend_name(FftBackend b) noexcept {
    switch (b) {
        case FftBackend::auto_:   return "auto";
        case FftBackend::vdsp:    return "vdsp";
        case FftBackend::kissfft: return "kissfft";
        case FftBackend::fftw3:   return "fftw3";
        case FftBackend::mkl:     return "mkl";
    }
    return "auto";
}

FftBackend parse_fft_backend(const char* s) noexcept {
    if (!s || !*s) return FftBackend::auto_;
    if (std::strcmp(s, "auto") == 0)    return FftBackend::auto_;
    if (std::strcmp(s, "vdsp") == 0)    return FftBackend::vdsp;
    if (std::strcmp(s, "kissfft") == 0) return FftBackend::kissfft;
    if (std::strcmp(s, "fftw3") == 0)   return FftBackend::fftw3;
    if (std::strcmp(s, "mkl") == 0)     return FftBackend::mkl;
    return FftBackend::auto_;
}

bool fft_backend_available(FftBackend b) noexcept {
    switch (b) {
        case FftBackend::auto_:   return true;
        case FftBackend::kissfft: return true;
        case FftBackend::vdsp:
#if PULP_FFT_BACKEND_HAS_VDSP
            return true;
#else
            return false;
#endif
        case FftBackend::fftw3:   return fftw3_dyn::get().ok;
        case FftBackend::mkl:     return mkl_dyn::get().ok;
    }
    return false;
}

FftBackend resolve_fft_backend(FftBackend hint) noexcept {
    // Env var overrides hint when hint is auto_
    if (hint == FftBackend::auto_) {
        if (const char* env = std::getenv("PULP_FFT_BACKEND")) {
            FftBackend e = parse_fft_backend(env);
            if (e != FftBackend::auto_ && fft_backend_available(e)) return e;
        }
    } else if (fft_backend_available(hint)) {
        return hint;
    }

    // Default precedence: vDSP > kissfft (FFTW/MKL never auto-picked,
    // because requiring an opt-in lets us keep zero runtime dep risk)
#if PULP_FFT_BACKEND_HAS_VDSP
    return FftBackend::vdsp;
#else
    return FftBackend::kissfft;
#endif
}

// ── Per-backend impls ──────────────────────────────────────────────────────

struct MultiBackendFft::Impl {
    int size = 0;
    FftBackend backend = FftBackend::kissfft;

    // kissfft (== built-in pulp::signal::Fft) and vdsp both delegate to
    // the existing Fft class. That class already picks vdsp on Apple and
    // the radix-2 fallback elsewhere — but we want to force one or the
    // other based on the user's selection. To do that we use Fft for
    // vdsp on Apple, and a tiny inline radix-2 wrapper for kissfft. The
    // simplest way to force the fallback on Apple (when the user asks
    // for kissfft) is to keep our own minimal twiddle/bit-reverse pair
    // — but that duplicates code. Instead, for the vdsp branch on Apple
    // we use Fft directly (since Fft already uses vdsp on Apple), and
    // for the kissfft branch we use a forced-fallback path implemented
    // in this file. That keeps the round-trip identity exact for both.
    std::unique_ptr<Fft> fft;  // used for vdsp branch
    std::vector<std::complex<float>> twiddles;  // used for kissfft branch

    // FFTW3 state
    void* fftw_plan_fwd = nullptr;
    void* fftw_plan_bwd = nullptr;
    void* fftw_buf      = nullptr;  // fftwf_malloc'd

    // MKL state
    void* mkl_desc = nullptr;

    ~Impl() {
        if (backend == FftBackend::fftw3) {
            const auto& b = fftw3_dyn::get();
            if (b.ok) {
                if (fftw_plan_fwd) b.destroy(fftw_plan_fwd);
                if (fftw_plan_bwd) b.destroy(fftw_plan_bwd);
                if (fftw_buf) b.free_(fftw_buf);
            }
        } else if (backend == FftBackend::mkl) {
            const auto& b = mkl_dyn::get();
            if (b.ok && mkl_desc) {
                b.free_(&mkl_desc);
            }
        }
    }

    void init_kissfft() {
        twiddles.resize(size / 2);
        constexpr double pi = 3.14159265358979323846;
        for (int i = 0; i < size / 2; ++i) {
            double angle = -2.0 * pi * i / size;
            twiddles[i] = {static_cast<float>(std::cos(angle)),
                           static_cast<float>(std::sin(angle))};
        }
    }

    void init_vdsp() {
#if PULP_FFT_BACKEND_HAS_VDSP
        fft = std::make_unique<Fft>(size);
#else
        throw std::runtime_error("MultiBackendFft: vdsp backend not available on this platform");
#endif
    }

    void init_fftw3() {
        const auto& b = fftw3_dyn::get();
        if (!b.ok) throw std::runtime_error("MultiBackendFft: fftw3 runtime library not loadable");
        fftw_buf = b.malloc_(sizeof(float) * 2 * static_cast<size_t>(size));
        if (!fftw_buf) throw std::runtime_error("MultiBackendFft: fftwf_malloc failed");
        using cx = fftw3_dyn::fftwf_complex;
        cx* buf = static_cast<cx*>(fftw_buf);
        // FFTW_FORWARD=-1, FFTW_BACKWARD=+1, FFTW_ESTIMATE=1<<6
        fftw_plan_fwd = b.plan_dft_1d(size, buf, buf, -1, 1u << 6);
        fftw_plan_bwd = b.plan_dft_1d(size, buf, buf,  1, 1u << 6);
        if (!fftw_plan_fwd || !fftw_plan_bwd)
            throw std::runtime_error("MultiBackendFft: fftwf_plan_dft_1d failed");
    }

    void init_mkl() {
        const auto& b = mkl_dyn::get();
        if (!b.ok) throw std::runtime_error("MultiBackendFft: mkl runtime library not loadable");
        if (b.create(&mkl_desc, mkl_dyn::DFTI_SINGLE, mkl_dyn::DFTI_COMPLEX,
                     1, static_cast<mkl_dyn::MKL_LONG>(size)) != 0)
            throw std::runtime_error("MultiBackendFft: DftiCreateDescriptor failed");
        // Inverse normalisation: 1/N scale.
        //
        // DftiSetValue is variadic — for a single-precision descriptor the
        // expected trailing argument is `float`. We pass the float through
        // the variadic interface; the compiler emits the right ABI dance
        // (varargs promote float → double, MKL recovers it via va_arg). The
        // standalone `fn_set_f` typed alias that previously aliased the
        // same dlsym handle was undefined behavior (Codex PR #3021 review).
        b.set(mkl_desc, mkl_dyn::DFTI_BACKWARD_SCALE,
              1.0f / static_cast<float>(size));
        if (b.commit(mkl_desc) != 0)
            throw std::runtime_error("MultiBackendFft: DftiCommitDescriptor failed");
    }

    void forward(std::complex<float>* data) const {
        switch (backend) {
            case FftBackend::vdsp:
                fft->forward(data);
                return;
            case FftBackend::kissfft:
                forward_kissfft(data);
                return;
            case FftBackend::fftw3: {
                const auto& b = fftw3_dyn::get();
                std::memcpy(fftw_buf, data, sizeof(std::complex<float>) * size);
                b.execute(fftw_plan_fwd);
                std::memcpy(data, fftw_buf, sizeof(std::complex<float>) * size);
                return;
            }
            case FftBackend::mkl: {
                const auto& b = mkl_dyn::get();
                b.fwd(mkl_desc, data);
                return;
            }
            case FftBackend::auto_:
                return;
        }
    }

    void inverse(std::complex<float>* data) const {
        switch (backend) {
            case FftBackend::vdsp:
                fft->inverse(data);
                return;
            case FftBackend::kissfft:
                inverse_kissfft(data);
                return;
            case FftBackend::fftw3: {
                const auto& b = fftw3_dyn::get();
                std::memcpy(fftw_buf, data, sizeof(std::complex<float>) * size);
                b.execute(fftw_plan_bwd);
                std::memcpy(data, fftw_buf, sizeof(std::complex<float>) * size);
                // FFTW backward is unnormalised — scale by 1/N
                float scale = 1.0f / static_cast<float>(size);
                for (int i = 0; i < size; ++i) data[i] *= scale;
                return;
            }
            case FftBackend::mkl: {
                const auto& b = mkl_dyn::get();
                b.bwd(mkl_desc, data);
                return;
            }
            case FftBackend::auto_:
                return;
        }
    }

    void forward_kissfft(std::complex<float>* data) const {
        bit_reverse(data);
        for (int len = 2; len <= size; len <<= 1) {
            int half = len / 2;
            int step = size / len;
            for (int i = 0; i < size; i += len) {
                for (int j = 0; j < half; ++j) {
                    auto w = twiddles[j * step];
                    auto u = data[i + j];
                    auto v = data[i + j + half] * w;
                    data[i + j] = u + v;
                    data[i + j + half] = u - v;
                }
            }
        }
    }

    void inverse_kissfft(std::complex<float>* data) const {
        for (int i = 0; i < size; ++i) data[i] = std::conj(data[i]);
        forward_kissfft(data);
        float scale = 1.0f / static_cast<float>(size);
        for (int i = 0; i < size; ++i) data[i] = std::conj(data[i]) * scale;
    }

    void bit_reverse(std::complex<float>* data) const {
        int bits = log2_int(size);
        for (int i = 0; i < size; ++i) {
            int j = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) j |= 1 << (bits - 1 - b);
            if (i < j) std::swap(data[i], data[j]);
        }
    }
};

// ── MultiBackendFft public API ─────────────────────────────────────────────

MultiBackendFft::MultiBackendFft(int size, FftBackend backend)
    : impl_(std::make_unique<Impl>())
{
    if (!is_power_of_two(size))
        throw std::invalid_argument("MultiBackendFft: size must be a power of two");

    impl_->size = size;

    // resolve_fft_backend is used only for the auto_ → concrete mapping.
    // An explicit unavailable backend MUST throw, not silently substitute,
    // because callers depending on a specific backend's perf/numerical
    // characteristics need the failure signal to fall back themselves.
    if (backend == FftBackend::auto_) {
        impl_->backend = resolve_fft_backend(backend);
    } else {
        if (!fft_backend_available(backend))
            throw std::runtime_error(std::string("MultiBackendFft: backend not available: ")
                                     + fft_backend_name(backend));
        impl_->backend = backend;
    }

    switch (impl_->backend) {
        case FftBackend::vdsp:    impl_->init_vdsp();    break;
        case FftBackend::kissfft: impl_->init_kissfft(); break;
        case FftBackend::fftw3:   impl_->init_fftw3();   break;
        case FftBackend::mkl:     impl_->init_mkl();     break;
        case FftBackend::auto_:   /* unreachable after resolve */     break;
    }
}

MultiBackendFft::~MultiBackendFft() = default;
MultiBackendFft::MultiBackendFft(MultiBackendFft&&) noexcept = default;
MultiBackendFft& MultiBackendFft::operator=(MultiBackendFft&&) noexcept = default;

int MultiBackendFft::size() const noexcept { return impl_ ? impl_->size : 0; }
FftBackend MultiBackendFft::backend() const noexcept {
    return impl_ ? impl_->backend : FftBackend::auto_;
}

void MultiBackendFft::forward(std::complex<float>* data) const { impl_->forward(data); }
void MultiBackendFft::inverse(std::complex<float>* data) const { impl_->inverse(data); }

}  // namespace pulp::signal

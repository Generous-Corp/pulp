// pulp::simd kernels, Apple Accelerate backend (vDSP).
//
// Every entry point is a vDSP execution call. None of them allocate: the
// setup-object APIs that do (FFT, biquad) are not used here. vDSP runs
// single-threaded on the calling thread, so these are safe on the audio
// thread. Results match the scalar reference to rounding only; Accelerate
// is updated with the OS, so its exact bits can change between OS releases.

#include <pulp/simd/simd.hpp>

#include <Accelerate/Accelerate.h>

#include <cstddef>

namespace pulp::simd::backend::accelerate {

namespace {

inline vDSP_Length len(std::size_t n) noexcept {
    return static_cast<vDSP_Length>(n);
}

} // namespace

// ── float ────────────────────────────────────────────────────────────────────

void add(const float* a, const float* b, float* dst, std::size_t n) noexcept {
    vDSP_vadd(a, 1, b, 1, dst, 1, len(n));
}
void mul(const float* a, const float* b, float* dst, std::size_t n) noexcept {
    vDSP_vmul(a, 1, b, 1, dst, 1, len(n));
}
void fma(const float* a, const float* b, const float* c, float* dst, std::size_t n) noexcept {
    vDSP_vma(a, 1, b, 1, c, 1, dst, 1, len(n));
}
void set(float value, float* dst, std::size_t n) noexcept {
    vDSP_vfill(&value, dst, 1, len(n));
}
void scale(const float* a, float s, float* dst, std::size_t n) noexcept {
    vDSP_vsmul(a, 1, &s, dst, 1, len(n));
}
void add_scaled(const float* a, float s, float* dst, std::size_t n) noexcept {
    vDSP_vsma(a, 1, &s, dst, 1, dst, 1, len(n));
}
float sum(const float* x, std::size_t n) noexcept {
    float r = 0.0f;
    if (n)
        vDSP_sve(x, 1, &r, len(n));
    return r;
}
float maximum(const float* x, std::size_t n) noexcept {
    float r = 0.0f;
    if (n)
        vDSP_maxv(x, 1, &r, len(n));
    return r;
}
float minimum(const float* x, std::size_t n) noexcept {
    float r = 0.0f;
    if (n)
        vDSP_minv(x, 1, &r, len(n));
    return r;
}
void abs(const float* a, float* dst, std::size_t n) noexcept {
    vDSP_vabs(a, 1, dst, 1, len(n));
}
void clamp(const float* a, float lo, float hi, float* dst, std::size_t n) noexcept {
    vDSP_vclip(a, 1, &lo, &hi, dst, 1, len(n));
}
float dot(const float* a, const float* b, std::size_t n) noexcept {
    float r = 0.0f;
    if (n)
        vDSP_dotpr(a, 1, b, 1, &r, len(n));
    return r;
}
float sum_squares(const float* x, std::size_t n) noexcept {
    float r = 0.0f;
    if (n)
        vDSP_svesq(x, 1, &r, len(n));
    return r;
}
float max_abs(const float* x, std::size_t n) noexcept {
    float r = 0.0f;
    if (n)
        vDSP_maxmgv(x, 1, &r, len(n));
    return r;
}
void ramp_mul(const float* x, float start, float step, float* dst, std::size_t n) noexcept {
    // vDSP_vrampmul advances its gain by repeated addition; the kernel's
    // contract is start + i * step, which the parity suite holds it to.
    vDSP_vrampmul(x, 1, &start, &step, dst, 1, len(n));
}
void correlate(const float* x, const float* h, float* y, std::size_t n_out,
               std::size_t taps) noexcept {
    if (n_out == 0)
        return;
    if (taps == 0) {
        vDSP_vclr(y, 1, len(n_out));
        return;
    }
    vDSP_conv(x, 1, h, 1, y, 1, len(n_out), len(taps));
}
void decimate2(const float* x, const float* h, float* y, std::size_t n_out,
               std::size_t taps) noexcept {
    if (n_out == 0)
        return;
    if (taps == 0) {
        vDSP_vclr(y, 1, len(n_out));
        return;
    }
    vDSP_desamp(x, 2, h, y, len(n_out), len(taps));
}

// ── double ───────────────────────────────────────────────────────────────────

void add(const double* a, const double* b, double* dst, std::size_t n) noexcept {
    vDSP_vaddD(a, 1, b, 1, dst, 1, len(n));
}
void mul(const double* a, const double* b, double* dst, std::size_t n) noexcept {
    vDSP_vmulD(a, 1, b, 1, dst, 1, len(n));
}
void fma(const double* a, const double* b, const double* c, double* dst, std::size_t n) noexcept {
    vDSP_vmaD(a, 1, b, 1, c, 1, dst, 1, len(n));
}
void set(double value, double* dst, std::size_t n) noexcept {
    vDSP_vfillD(&value, dst, 1, len(n));
}
void scale(const double* a, double s, double* dst, std::size_t n) noexcept {
    vDSP_vsmulD(a, 1, &s, dst, 1, len(n));
}
void add_scaled(const double* a, double s, double* dst, std::size_t n) noexcept {
    vDSP_vsmaD(a, 1, &s, dst, 1, dst, 1, len(n));
}
double sum(const double* x, std::size_t n) noexcept {
    double r = 0.0;
    if (n)
        vDSP_sveD(x, 1, &r, len(n));
    return r;
}
double maximum(const double* x, std::size_t n) noexcept {
    double r = 0.0;
    if (n)
        vDSP_maxvD(x, 1, &r, len(n));
    return r;
}
double minimum(const double* x, std::size_t n) noexcept {
    double r = 0.0;
    if (n)
        vDSP_minvD(x, 1, &r, len(n));
    return r;
}
void abs(const double* a, double* dst, std::size_t n) noexcept {
    vDSP_vabsD(a, 1, dst, 1, len(n));
}
void clamp(const double* a, double lo, double hi, double* dst, std::size_t n) noexcept {
    vDSP_vclipD(a, 1, &lo, &hi, dst, 1, len(n));
}
double dot(const double* a, const double* b, std::size_t n) noexcept {
    double r = 0.0;
    if (n)
        vDSP_dotprD(a, 1, b, 1, &r, len(n));
    return r;
}
double sum_squares(const double* x, std::size_t n) noexcept {
    double r = 0.0;
    if (n)
        vDSP_svesqD(x, 1, &r, len(n));
    return r;
}
double max_abs(const double* x, std::size_t n) noexcept {
    double r = 0.0;
    if (n)
        vDSP_maxmgvD(x, 1, &r, len(n));
    return r;
}
void ramp_mul(const double* x, double start, double step, double* dst, std::size_t n) noexcept {
    vDSP_vrampmulD(x, 1, &start, &step, dst, 1, len(n));
}
void correlate(const double* x, const double* h, double* y, std::size_t n_out,
               std::size_t taps) noexcept {
    if (n_out == 0)
        return;
    if (taps == 0) {
        vDSP_vclrD(y, 1, len(n_out));
        return;
    }
    vDSP_convD(x, 1, h, 1, y, 1, len(n_out), len(taps));
}
void decimate2(const double* x, const double* h, double* y, std::size_t n_out,
               std::size_t taps) noexcept {
    if (n_out == 0)
        return;
    if (taps == 0) {
        vDSP_vclrD(y, 1, len(n_out));
        return;
    }
    vDSP_desampD(x, 2, h, y, len(n_out), len(taps));
}

} // namespace pulp::simd::backend::accelerate

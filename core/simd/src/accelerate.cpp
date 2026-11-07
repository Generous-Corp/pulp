// pulp::simd kernels, Apple Accelerate backend (vDSP).
//
// Entry points are vDSP execution calls (plus strided BLAS copies for
// interleave/deinterleave, and plain loops where vDSP has no equivalent). None
// of them allocate: the
// setup-object APIs that do (FFT, biquad) are not used here. vDSP runs
// single-threaded on the calling thread, so these are safe on the audio
// thread. Results match the scalar reference to rounding only; Accelerate
// is updated with the OS, so its exact bits can change between OS releases.

// Use the current (non-deprecated) CBLAS declarations.
#define ACCELERATE_NEW_LAPACK 1

#include <pulp/simd/scalar_kernels.hpp>
#include <pulp/simd/simd.hpp>

#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace pulp::simd::backend::accelerate {

namespace {

inline vDSP_Length len(std::size_t n) noexcept {
    return static_cast<vDSP_Length>(n);
}

constexpr std::size_t kConvertChunk = 512;

/// vDSP's float -> integer conversions do not define NaN; send it to 0 first
/// so every backend agrees.
template <typename T> inline void scrub_nan(T* x, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        if (x[i] != x[i])
            x[i] = T(0);
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

void sub(const float* a, const float* b, float* dst, std::size_t n) noexcept {
    vDSP_vsub(b, 1, a, 1, dst, 1, len(n)); // vDSP computes (second operand) - (first)
}
void div(const float* a, const float* b, float* dst, std::size_t n) noexcept {
    vDSP_vdiv(b, 1, a, 1, dst, 1, len(n)); // (second operand) / (first)
}
void negate(const float* a, float* dst, std::size_t n) noexcept {
    vDSP_vneg(a, 1, dst, 1, len(n));
}
float mean(const float* x, std::size_t n) noexcept {
    float r = 0;
    if (n)
        vDSP_meanv(x, 1, &r, len(n));
    return r;
}
float rms(const float* x, std::size_t n) noexcept {
    float r = 0;
    if (n)
        vDSP_rmsqv(x, 1, &r, len(n));
    return r;
}
float find_maximum(const float* x, std::size_t n, std::size_t* index) noexcept {
    const float m = maximum(x, n);
    *index = detail::first_index_of(x, n, m);
    return m;
}
float find_minimum(const float* x, std::size_t n, std::size_t* index) noexcept {
    const float m = minimum(x, n);
    *index = detail::first_index_of(x, n, m);
    return m;
}
// vDSP has no compare-and-replace; the plain loop vectorises under the
// compiler and keeps the scalar reference's NaN behaviour.
void replace_below(const float* a, float threshold, float value, float* dst, std::size_t n) noexcept {
    detail::replace_below_impl(a, threshold, value, dst, n);
}
void replace_above(const float* a, float threshold, float value, float* dst, std::size_t n) noexcept {
    detail::replace_above_impl(a, threshold, value, dst, n);
}
void from_int16(const std::int16_t* src, float scale, float* dst, std::size_t n) noexcept {
    vDSP_vflt16(src, 1, dst, 1, len(n));
    if (scale != float(1))
        vDSP_vsmul(dst, 1, &scale, dst, 1, len(n));
}
void from_int32(const std::int32_t* src, float scale, float* dst, std::size_t n) noexcept {
    vDSP_vflt32(src, 1, dst, 1, len(n));
    if (scale != float(1))
        vDSP_vsmul(dst, 1, &scale, dst, 1, len(n));
}
void to_int16(const float* src, float scale, std::int16_t* dst, std::size_t n) noexcept {
    const float lo = -32768;
    const float hi = 32767;
    float chunk[kConvertChunk];
    for (std::size_t i = 0; i < n; i += kConvertChunk) {
        const std::size_t m = std::min(kConvertChunk, n - i);
        vDSP_vsmul(src + i, 1, &scale, chunk, 1, len(m));
        scrub_nan(chunk, m);
        vDSP_vclip(chunk, 1, &lo, &hi, chunk, 1, len(m));
        vDSP_vfixr16(chunk, 1, dst + i, 1, len(m)); // rounds half to even
    }
}
void to_int32(const float* src, float scale, std::int32_t* dst, std::size_t n) noexcept {
    const float lo = -2147483648.0;
    const float hi = detail::int_upper_bound<float, std::int32_t>();
    float chunk[kConvertChunk];
    for (std::size_t i = 0; i < n; i += kConvertChunk) {
        const std::size_t m = std::min(kConvertChunk, n - i);
        vDSP_vsmul(src + i, 1, &scale, chunk, 1, len(m));
        scrub_nan(chunk, m);
        vDSP_vclip(chunk, 1, &lo, &hi, chunk, 1, len(m));
        vDSP_vfixr32(chunk, 1, dst + i, 1, len(m));
    }
}
// Strided BLAS copies move whole elements, so the result is exact.
void interleave2(const float* l, const float* r, float* dst, std::size_t n) noexcept {
    cblas_scopy(static_cast<int>(n), l, 1, dst, 2);
    cblas_scopy(static_cast<int>(n), r, 1, dst + 1, 2);
}
void deinterleave2(const float* src, float* l, float* r, std::size_t n) noexcept {
    cblas_scopy(static_cast<int>(n), src, 2, l, 1);
    cblas_scopy(static_cast<int>(n), src + 1, 2, r, 1);
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

void sub(const double* a, const double* b, double* dst, std::size_t n) noexcept {
    vDSP_vsubD(b, 1, a, 1, dst, 1, len(n)); // vDSP computes (second operand) - (first)
}
// vDSP_vdivD is not correctly rounded (it differs from IEEE division in the
// last bit), so the double kernel uses the hardware divide through a plain
// loop and stays bit-identical to the scalar reference.
void div(const double* a, const double* b, double* dst, std::size_t n) noexcept {
    detail::div_impl(a, b, dst, n);
}
void negate(const double* a, double* dst, std::size_t n) noexcept {
    vDSP_vnegD(a, 1, dst, 1, len(n));
}
double mean(const double* x, std::size_t n) noexcept {
    double r = 0;
    if (n)
        vDSP_meanvD(x, 1, &r, len(n));
    return r;
}
double rms(const double* x, std::size_t n) noexcept {
    double r = 0;
    if (n)
        vDSP_rmsqvD(x, 1, &r, len(n));
    return r;
}
double find_maximum(const double* x, std::size_t n, std::size_t* index) noexcept {
    const double m = maximum(x, n);
    *index = detail::first_index_of(x, n, m);
    return m;
}
double find_minimum(const double* x, std::size_t n, std::size_t* index) noexcept {
    const double m = minimum(x, n);
    *index = detail::first_index_of(x, n, m);
    return m;
}
// vDSP has no compare-and-replace; the plain loop vectorises under the
// compiler and keeps the scalar reference's NaN behaviour.
void replace_below(const double* a, double threshold, double value, double* dst, std::size_t n) noexcept {
    detail::replace_below_impl(a, threshold, value, dst, n);
}
void replace_above(const double* a, double threshold, double value, double* dst, std::size_t n) noexcept {
    detail::replace_above_impl(a, threshold, value, dst, n);
}
void from_int16(const std::int16_t* src, double scale, double* dst, std::size_t n) noexcept {
    vDSP_vflt16D(src, 1, dst, 1, len(n));
    if (scale != double(1))
        vDSP_vsmulD(dst, 1, &scale, dst, 1, len(n));
}
void from_int32(const std::int32_t* src, double scale, double* dst, std::size_t n) noexcept {
    vDSP_vflt32D(src, 1, dst, 1, len(n));
    if (scale != double(1))
        vDSP_vsmulD(dst, 1, &scale, dst, 1, len(n));
}
void to_int16(const double* src, double scale, std::int16_t* dst, std::size_t n) noexcept {
    const double lo = -32768;
    const double hi = 32767;
    double chunk[kConvertChunk];
    for (std::size_t i = 0; i < n; i += kConvertChunk) {
        const std::size_t m = std::min(kConvertChunk, n - i);
        vDSP_vsmulD(src + i, 1, &scale, chunk, 1, len(m));
        scrub_nan(chunk, m);
        vDSP_vclipD(chunk, 1, &lo, &hi, chunk, 1, len(m));
        vDSP_vfixr16D(chunk, 1, dst + i, 1, len(m)); // rounds half to even
    }
}
void to_int32(const double* src, double scale, std::int32_t* dst, std::size_t n) noexcept {
    const double lo = -2147483648.0;
    const double hi = detail::int_upper_bound<double, std::int32_t>();
    double chunk[kConvertChunk];
    for (std::size_t i = 0; i < n; i += kConvertChunk) {
        const std::size_t m = std::min(kConvertChunk, n - i);
        vDSP_vsmulD(src + i, 1, &scale, chunk, 1, len(m));
        scrub_nan(chunk, m);
        vDSP_vclipD(chunk, 1, &lo, &hi, chunk, 1, len(m));
        vDSP_vfixr32D(chunk, 1, dst + i, 1, len(m));
    }
}
// Strided BLAS copies move whole elements, so the result is exact.
void interleave2(const double* l, const double* r, double* dst, std::size_t n) noexcept {
    cblas_dcopy(static_cast<int>(n), l, 1, dst, 2);
    cblas_dcopy(static_cast<int>(n), r, 1, dst + 1, 2);
}
void deinterleave2(const double* src, double* l, double* r, std::size_t n) noexcept {
    cblas_dcopy(static_cast<int>(n), src, 2, l, 1);
    cblas_dcopy(static_cast<int>(n), src + 1, 2, r, 1);
}

} // namespace pulp::simd::backend::accelerate

#pragma once

#include <pulp/signal/checked_allocation.hpp>
#include <pulp/signal/rt_work_counter.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#define PULP_FFT_HAS_VDSP 1
#endif

namespace pulp::signal {

namespace detail {

inline constexpr std::size_t fft_twiddle_index(int phase_index, int stride) noexcept {
    const auto index = static_cast<std::size_t>(phase_index);
    const auto step = static_cast<std::size_t>(stride);
    return index * step;
}

} // namespace detail

/// Conservative policy charge for Accelerate's opaque float FFT setup. Apple
/// does not expose its allocation size, so admission treats the setup as one
/// allocation bounded at 64 bytes per transform point. Non-vDSP transforms do
/// not own this allocation and therefore charge zero.
inline constexpr std::uint64_t kVdspFftSetupChargeBytesPerPoint = 64;

/// Floats a float FftT's vDSP split buffer holds beyond its two N-point
/// halves: up to 15 to reach a 64-byte boundary plus a 4-float (16-byte)
/// step off it, which is how the halves' placement is pinned (see FftT).
inline constexpr std::uint64_t kVdspSplitPaddingFloats = 20;

template <typename SampleType>
inline bool checked_fft_platform_setup_bytes(std::uint64_t fft_size,
                                             std::uint64_t target_max_bytes,
                                             std::uint64_t& bytes) noexcept {
#if PULP_FFT_HAS_VDSP
    if constexpr (std::is_same_v<SampleType, float>)
        return checked_capacity_product(fft_size, kVdspFftSetupChargeBytesPerPoint,
                                        target_max_bytes, bytes);
#endif
    bytes = 0;
    return true;
}

/// Complete retained-storage contract for one prepared FftT. Keeping this
/// beside FftT makes callers compose the transform's allocation policy instead
/// of duplicating its platform-specific backing stores.
template <typename SampleType>
inline bool checked_fft_retained_bytes(std::uint64_t fft_size,
                                       std::uint64_t target_max_bytes,
                                       std::uint64_t& bytes) noexcept {
    CheckedRetainedByteCharge charge(target_max_bytes);
    std::uint64_t platform_setup_bytes = 0;
    if (!charge.add<std::complex<double>>(fft_size / 2u)
        || !checked_fft_platform_setup_bytes<SampleType>(fft_size, target_max_bytes,
                                                         platform_setup_bytes)
        || !charge.add_retained_bytes(platform_setup_bytes))
        return false;
#if PULP_FFT_HAS_VDSP
    if constexpr (std::is_same_v<SampleType, float>) {
        if (fft_size > (UINT64_MAX - kVdspSplitPaddingFloats) / 2u
            || !charge.add<float>(2u * fft_size + kVdspSplitPaddingFloats))
            return false;
    }
#endif
    bytes = charge.total();
    return true;
}

// Radix-2 FFT — in-place, decimation-in-time.
// Size must be a power of 2.
// On Apple platforms, uses vDSP for significantly faster large transforms.
//
// Real-input signals have Hermitian spectra, so `forward_real_half()` and
// `inverse_real_half()` move only the N/2 + 1 independent bins (DC through
// Nyquist) and run an N/2-point complex transform underneath; a caller-chosen
// scale is folded into the pass that packs the result, so no separate
// normalisation sweep runs. `forward_real()` keeps its full mirrored N-bin
// output for callers that index the upper half.
//
// RT contract: construction/destruction allocate or release twiddle/vDSP
// storage and are prepare/control-thread work. forward(), inverse(),
// forward_real(), forward_real_half(), inverse_real_half(), magnitude(),
// magnitude_db(), and size() are allocation-free after construction when
// callers provide valid input/output buffers.
template <typename SampleType = float>
class FftT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "FftT requires a floating-point sample type");
public:
    FftT() = default;

    // Non-copyable (vDSP_setup handle), movable
    FftT(const FftT&) = delete;
    FftT& operator=(const FftT&) = delete;

    FftT(FftT&& other) noexcept
        : size_(other.size_), twiddles_(std::move(other.twiddles_))
#if PULP_FFT_HAS_VDSP
        , log2n_(other.log2n_), vdsp_setup_(other.vdsp_setup_)
        , split_(std::move(other.split_)), split_base_(other.split_base_)
#endif
    {
#if PULP_FFT_HAS_VDSP
        other.vdsp_setup_ = nullptr;  // Prevent double-free
#endif
        other.size_ = 0;
    }

    FftT& operator=(FftT&& other) noexcept {
        if (this != &other) {
#if PULP_FFT_HAS_VDSP
            if (vdsp_setup_) vDSP_destroy_fftsetup(vdsp_setup_);
            vdsp_setup_ = other.vdsp_setup_;
            other.vdsp_setup_ = nullptr;
            log2n_ = other.log2n_;
            split_ = std::move(other.split_);
            split_base_ = other.split_base_;
#endif
            size_ = other.size_;
            other.size_ = 0;
            twiddles_ = std::move(other.twiddles_);
        }
        return *this;
    }

    explicit FftT(int size) : size_(size) {
#if PULP_FFT_HAS_VDSP
        // Use vDSP for FFT — much faster for large sizes
        if constexpr (std::is_same_v<SampleType, float>) {
            log2n_ = 0;
            for (int n = size; n > 1; n >>= 1) ++log2n_;
            split_.resize(2 * static_cast<std::size_t>(size) + kVdspSplitPaddingFloats);
            // First element on a 64-byte boundary; vector storage moves with
            // the vector, so the offset stays valid across moves.
            const auto address = reinterpret_cast<std::uintptr_t>(split_.data());
            split_base_ = ((64u - address % 64u) % 64u) / sizeof(float);
        }
#endif
        // Pre-compute twiddle factors (used as fallback on non-Apple)
        twiddles_.resize(size / 2);
        for (int i = 0; i < size / 2; ++i) {
            double angle = -2.0 * pi * i / size;
            twiddles_[i] = {std::cos(angle), std::sin(angle)};
        }
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            // Keep raw setup acquisition last: every vector allocation and
            // twiddle population that can throw has completed, so constructor
            // unwinding cannot bypass destruction of an acquired setup.
            vdsp_setup_ = vDSP_create_fftsetup(log2n_, kFFTRadix2);
        }
#endif
    }

    ~FftT() {
#if PULP_FFT_HAS_VDSP
        if (vdsp_setup_) vDSP_destroy_fftsetup(vdsp_setup_);
#endif
    }

    int size() const { return size_; }

    /// True when every backend resource required by forward()/inverse() is
    /// available. Construction can produce a non-ready float FFT if Apple's
    /// vDSP setup allocation returns null without throwing.
    [[nodiscard]] bool ready() const noexcept {
        if (size_ <= 0 || twiddles_.size() != static_cast<std::size_t>(size_ / 2))
            return false;
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            return vdsp_setup_ != nullptr
                && split_.size() == 2 * static_cast<std::size_t>(size_)
                                        + kVdspSplitPaddingFloats;
        }
#endif
        return true;
    }

    // Forward FFT (time → frequency) — complex in-place
    void forward(std::complex<SampleType>* data) const {
        rt::count_fft();
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            forward_vdsp(data);
        } else {
            forward_fallback(data);
        }
#else
        forward_fallback(data);
#endif
    }

    // Inverse FFT (frequency → time) — complex in-place
    void inverse(std::complex<SampleType>* data) const {
        rt::count_fft();
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            inverse_vdsp(data);
        } else {
            inverse_fallback(data);
        }
#else
        inverse_fallback(data);
#endif
    }

    // Real-valued forward FFT: real input → complex output
    void forward_real(const SampleType* input, std::complex<SampleType>* output) const {
        rt::count_fft();
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            forward_real_vdsp(input, output);
        } else {
            forward_real_fallback(input, output);
        }
#else
        forward_real_fallback(input, output);
#endif
    }

    /// Independent bins of a real transform of size(): DC through Nyquist.
    int real_bins() const { return size_ / 2 + 1; }

    /// Real-input forward transform. Reads size() real samples and writes
    /// real_bins() bins, X[k] = scale * sum_n x[n] e^(-2 pi i k n / N) for
    /// k = 0..N/2. DC and Nyquist are written with zero imaginary part and
    /// nothing past the Nyquist bin is touched. `output` must not overlap
    /// `input`. Requires size() >= 2.
    void forward_real_half(const SampleType* input, std::complex<SampleType>* output,
                           SampleType scale = SampleType{1}) const {
        rt::count_fft();
#if PULP_FFT_HAS_VDSP
        // vDSP's packed real transform needs at least two complex points.
        if constexpr (std::is_same_v<SampleType, float>) {
            if (size_ >= 4) {
                forward_real_half_vdsp(input, output, scale);
                return;
            }
        }
#endif
        forward_real_half_fallback(input, output, scale);
    }

    /// Inverse of `forward_real_half()`. Reads real_bins() bins (the imaginary
    /// parts of DC and Nyquist are ignored, as a real signal cannot carry
    /// them) and writes size() real samples equal to `scale` times the
    /// unnormalised inverse DFT of the Hermitian spectrum they define; the
    /// default scale 1/N recovers the forward transform's input. `output` may
    /// occupy the same storage as `input` (a complex buffer reinterpreted as
    /// real samples), which lets a caller transform a spectrum in place.
    /// Requires size() >= 2.
    void inverse_real_half(const std::complex<SampleType>* input, SampleType* output,
                           SampleType scale) const {
        rt::count_fft();
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            if (size_ >= 4) {
                inverse_real_half_vdsp(input, output, scale);
                return;
            }
        }
#endif
        inverse_real_half_fallback(input, output, scale);
    }

    void inverse_real_half(const std::complex<SampleType>* input, SampleType* output) const {
        inverse_real_half(input, output, SampleType{1} / static_cast<SampleType>(size_));
    }

    // Compute magnitude spectrum in dB
    void magnitude_db(const std::complex<SampleType>* freq,
                      SampleType* out,
                      int num_bins) const {
        for (int i = 0; i < num_bins; ++i) {
            SampleType mag = std::abs(freq[i]);
            out[i] = SampleType{20.0f} *
                     std::log10(std::max(mag, SampleType{1e-10f}));
        }
    }

    // Compute magnitude spectrum (linear)
    void magnitude(const std::complex<SampleType>* freq,
                   SampleType* out,
                   int num_bins) const {
        for (int i = 0; i < num_bins; ++i) {
            out[i] = std::abs(freq[i]);
        }
    }

private:
    static constexpr double pi = 3.14159265358979323846;
    int size_ = 0;
    std::vector<std::complex<double>> twiddles_;
#if PULP_FFT_HAS_VDSP
    int log2n_ = 0;
    FFTSetup vdsp_setup_ = nullptr;
    // One buffer holds both split halves so their placement can be chosen.
    // vDSP takes a separate code path when both halves of a split complex
    // array sit on 64-byte boundaries, and which path is faster depends on the
    // transform and the chip. Measured with vDSP_fft_zrip / vDSP_fft_zip:
    //   * real, 512-2048 points: off-boundary is 1.4-1.9x faster on M5
    //     Ultra and M5 Max; M3 Ultra prefers the boundary from 1024 points
    //     (up to 1.3x at 2048). Off-boundary is used: it is the newer chips'
    //     win and the larger one.
    //   * real, 4096 points and up: on-boundary, 1.3-1.6x faster on all three.
    //   * complex, 512 points and up: on-boundary, up to 1.6x faster on all
    //     three; below 512 off-boundary, up to 1.6x faster on M5.
    // Two separate allocations left the placement to the allocator, so the
    // same transform's speed changed with where the buffers landed.
    mutable std::vector<float> split_;
    std::size_t split_base_ = 0;
    static_assert(kVdspSplitPaddingFloats >= 64 / sizeof(float) - 1 + 4,
                  "split padding must cover the 64-byte alignment slack plus the 4-float step");
#endif

#if PULP_FFT_HAS_VDSP
    // N-point halves for complex transforms: on 64-byte boundaries from 512
    // points up, 16 bytes past one below that.
    DSPSplitComplex split() const {
        const std::size_t offset = size_ >= 512 ? 0 : 4;
        float* base = split_.data() + split_base_ + offset;
        return {base, base + size_};
    }

    // N/2-point halves for real transforms: on 64-byte boundaries from 4096
    // points up, 16 bytes past one below that.
    DSPSplitComplex real_split() const {
        const std::size_t offset = size_ >= 4096 ? 0 : 4;
        float* base = split_.data() + split_base_ + offset;
        return {base, base + size_ / 2};
    }

    void forward_vdsp(std::complex<float>* data) const {
        DSPSplitComplex z = split();
        const auto n = static_cast<vDSP_Length>(size_);
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(data), 2, &z, 1, n);
        vDSP_fft_zip(vdsp_setup_, &z, 1, log2n_, kFFTDirection_Forward);
        vDSP_ztoc(&z, 1, reinterpret_cast<DSPComplex*>(data), 2, n);
    }

    void inverse_vdsp(std::complex<float>* data) const {
        DSPSplitComplex z = split();
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(data), 2, &z, 1,
                  static_cast<vDSP_Length>(size_));
        vDSP_fft_zip(vdsp_setup_, &z, 1, log2n_, kFFTDirection_Inverse);
        // vDSP's inverse is unnormalised; fold 1/N into the interleave.
        interleave_scaled(z.realp, z.imagp, reinterpret_cast<float*>(data), size_,
                          1.0f / static_cast<float>(size_));
    }

    // out[2k] = scale * re[k], out[2k + 1] = scale * im[k]: one pass, which
    // the compiler turns into interleaving vector stores.
    static void interleave_scaled(const float* re, const float* im, float* out, int count,
                                  float scale) {
        for (int k = 0; k < count; ++k) {
            out[2 * k] = scale * re[k];
            out[2 * k + 1] = scale * im[k];
        }
    }

    // Packs the real input as N/2 complex pairs (even samples real, odd
    // imaginary) and runs vDSP's real transform, whose result is twice the
    // DFT with Nyquist's real part parked in imagp[0].
    void forward_real_packed_vdsp(const float* input) const {
        DSPSplitComplex z = real_split();
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(input), 2, &z, 1,
                  static_cast<vDSP_Length>(size_ / 2));
        vDSP_fft_zrip(vdsp_setup_, &z, 1, log2n_, kFFTDirection_Forward);
    }

    void forward_real_half_vdsp(const float* input, std::complex<float>* output,
                                float scale) const {
        forward_real_packed_vdsp(input);
        const int half = size_ / 2;
        const float s = 0.5f * scale;
        auto* out = reinterpret_cast<float*>(output);
        const DSPSplitComplex z = real_split();
        interleave_scaled(z.realp, z.imagp, out, half, s);
        out[2 * half] = s * z.imagp[0]; // Nyquist
        out[2 * half + 1] = 0.0f;
        out[1] = 0.0f;                      // DC has no imaginary part
    }

    void inverse_real_half_vdsp(const std::complex<float>* input, float* output,
                                float scale) const {
        const int half = size_ / 2;
        const auto* in = reinterpret_cast<const float*>(input);
        DSPSplitComplex z = real_split();
        float* re = z.realp;
        float* im = z.imagp;
        for (int k = 0; k < half; ++k) {
            re[k] = in[2 * k];
            im[k] = in[2 * k + 1];
        }
        im[0] = in[2 * half]; // Nyquist's real part rides in imagp[0]
        vDSP_fft_zrip(vdsp_setup_, &z, 1, log2n_, kFFTDirection_Inverse);
        // The packed inverse of the DFT is N times the signal, split as even
        // samples in realp and odd samples in imagp.
        interleave_scaled(re, im, output, half, scale);
    }

    void forward_real_vdsp(const float* input, std::complex<float>* output) const {
        forward_real_packed_vdsp(input);
        // vDSP_fft_zrip packs result: realp[0] = DC, imagp[0] = Nyquist, and
        // carries an implicit 2x scale. Unpack, mirror, and scale in one pass.
        const int half = size_ / 2;
        const DSPSplitComplex z = real_split();
        output[0] = {z.realp[0] * 0.5f, 0.0f};
        output[half] = {z.imagp[0] * 0.5f, 0.0f};
        for (int i = 1; i < half; ++i) {
            const float re = z.realp[i] * 0.5f;
            const float im = z.imagp[i] * 0.5f;
            output[i] = {re, im};
            // Conjugate symmetry: X[N-k] = conj(X[k])
            output[size_ - i] = {re, -im};
        }
    }
#endif

    void forward_fallback(std::complex<SampleType>* data) const {
        transform_fallback(data, size_);
    }

    // In-place radix-2 transform of `n` points (n divides size_) using the
    // size_-point twiddle table at stride size_ / n.
    void transform_fallback(std::complex<SampleType>* data, int n) const {
        bit_reverse(data, n);
        for (int len = 2; len <= n; len <<= 1) {
            int half = len / 2;
            int step = size_ / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; ++j) {
                    auto w = std::complex<SampleType>(
                        twiddles_[detail::fft_twiddle_index(j, step)]);
                    auto u = data[i + j];
                    auto v = data[i + j + half] * w;
                    data[i + j] = u + v;
                    data[i + j + half] = u - v;
                }
            }
        }
    }

    void inverse_fallback(std::complex<SampleType>* data) const {
        for (int i = 0; i < size_; ++i) data[i] = std::conj(data[i]);
        forward_fallback(data);
        SampleType scale = SampleType{1.0f} / static_cast<SampleType>(size_);
        for (int i = 0; i < size_; ++i) data[i] = std::conj(data[i]) * scale;
    }

    void forward_real_fallback(const SampleType* input,
                               std::complex<SampleType>* output) const {
        if (size_ < 2) {
            output[0] = {input[0], SampleType{0}};
            return;
        }
        // The _fallback helper, not the public entry point: forward_real()
        // already counted this transform once.
        forward_real_half_fallback(input, output, SampleType{1});
        for (int i = 1; i < size_ / 2; ++i)
            output[size_ - i] = std::conj(output[i]);
    }

    // Real transform through an N/2-point complex one: pack z[k] =
    // x[2k] + i x[2k+1], transform, then separate the even/odd spectra
    // E, O pairwise and combine X[k] = E[k] + W^k O[k],
    // X[N/2 - k] = conj(E[k] - W^k O[k]).
    void forward_real_half_fallback(const SampleType* input, std::complex<SampleType>* output,
                                    SampleType scale) const {
        const int half = size_ / 2;
        for (int k = 0; k < half; ++k)
            output[k] = {input[2 * k], input[2 * k + 1]};
        transform_fallback(output, half);
        const SampleType h{0.5f};
        const auto z0 = output[0];
        for (int k = 1; k <= half / 2; ++k) {
            const auto a = output[k];
            const auto b = std::conj(output[half - k]);
            const std::complex<SampleType> e = h * (a + b);
            const std::complex<SampleType> d = h * (a - b);
            const std::complex<SampleType> o{d.imag(), -d.real()}; // d / i
            const std::complex<SampleType> w(twiddles_[static_cast<std::size_t>(k)]);
            const auto wo = w * o;
            output[k] = scale * (e + wo);
            if (k != half - k)
                output[half - k] = scale * std::conj(e - wo);
        }
        output[0] = {scale * (z0.real() + z0.imag()), SampleType{0}};
        output[half] = {scale * (z0.real() - z0.imag()), SampleType{0}};
    }

    // Inverse of the above: rebuild Z[k] = E[k] + i O[k] (scaled by 2 * scale,
    // which is what turns the N/2-point inverse into scale times the N-point
    // one), transform, and read the even/odd samples back out of the complex
    // result. Each pair (k, N/2 - k) reads and writes only its own two
    // slots, so the spectrum may share storage with the output.
    void inverse_real_half_fallback(const std::complex<SampleType>* input, SampleType* output,
                                    SampleType scale) const {
        const int half = size_ / 2;
        const SampleType nyquist = input[half].real();
        const SampleType dc = input[0].real();
        auto* z = reinterpret_cast<std::complex<SampleType>*>(output);
        // Build conj(Z) so the forward transform below acts as the inverse.
        for (int k = 1; k <= half / 2; ++k) {
            const auto xk = input[k];
            const auto xm = input[half - k];
            const std::complex<SampleType> w(twiddles_[static_cast<std::size_t>(k)]);
            // E = X[k] + conj(X[h-k]), O = (X[k] - conj(X[h-k])) conj(W^k);
            // the partner uses conj(W^(h-k)) = -W^k.
            const auto ek = xk + std::conj(xm);
            const auto ok = (xk - std::conj(xm)) * std::conj(w);
            const auto em = xm + std::conj(xk);
            const auto om = (xm - std::conj(xk)) * (-w);
            const std::complex<SampleType> i{SampleType{0}, SampleType{1}};
            z[k] = std::conj(scale * (ek + i * ok));
            if (k != half - k)
                z[half - k] = std::conj(scale * (em + i * om));
        }
        z[0] = std::conj(std::complex<SampleType>(scale * (dc + nyquist), scale * (dc - nyquist)));
        transform_fallback(z, half);
        for (int k = 0; k < half; ++k)
            z[k] = std::conj(z[k]);
    }

    void bit_reverse(std::complex<SampleType>* data) const {
        bit_reverse(data, size_);
    }

    void bit_reverse(std::complex<SampleType>* data, int count) const {
        int bits = 0;
        for (int n = count; n > 1; n >>= 1) ++bits;

        for (int i = 0; i < count; ++i) {
            int j = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) j |= 1 << (bits - 1 - b);
            if (i < j) std::swap(data[i], data[j]);
        }
    }
};

using Fft = FftT<float>;
using Fft64 = FftT<double>;

// ── Convolver ────────────────────────────────────────────────────────────────

// Simple frequency-domain convolver using overlap-add. Output is delayed by
// one block: a block is transformed once it has filled, and its result is
// read out while the next one fills.
//
// RT contract: load_ir() allocates FFT and overlap-add buffers and is not
// audio-thread safe. process(), buffer process(), and reset() are
// allocation-free after a valid impulse response has been loaded.
template <typename SampleType = float>
class ConvolverT {
public:
    // Load an impulse response
    void load_ir(const SampleType* ir, int ir_length, int block_size = 0) {
        if (ir == nullptr || ir_length <= 0) {
            fft_.reset();
            ir_freq_.clear();
            input_buf_.clear();
            output_buf_.clear();
            overlap_.clear();
            freq_buf_.clear();
            fft_size_ = 0;
            block_size_ = 0;
            pos_ = 0;
            return;
        }

        block_size_ = block_size > 0 ? block_size : 256;

        // FFT size: next power of 2 >= block_size + ir_length - 1
        // The real transform needs at least two points.
        fft_size_ = 2;
        while (fft_size_ < block_size_ + ir_length) fft_size_ <<= 1;

        fft_ = std::make_unique<FftT<SampleType>>(fft_size_);

        // Transform IR
        ir_freq_.resize(fft_size_);
        for (int i = 0; i < fft_size_; ++i)
            ir_freq_[i] = i < ir_length
                ? std::complex<SampleType>(ir[i], SampleType{0.0f})
                : std::complex<SampleType>(SampleType{0.0f}, SampleType{0.0f});
        fft_->forward(ir_freq_.data());

        // Buffers
        input_buf_.assign(fft_size_, SampleType{0.0f});
        output_buf_.assign(fft_size_, SampleType{0.0f});
        overlap_.assign(fft_size_, SampleType{0.0f});
        freq_buf_.assign(fft_size_, {});
        pos_ = 0;
    }

    // Process a single sample
    SampleType process(SampleType input) {
        if (!fft_ || block_size_ <= 0)
            return input;

        input_buf_[pos_] = input;
        SampleType output = output_buf_[pos_];
        ++pos_;

        if (pos_ >= block_size_) {
            process_block();
            pos_ = 0;
        }

        return output;
    }

    // Process a buffer
    void process(const SampleType* input, SampleType* output, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            output[i] = process(input[i]);
    }

    void reset() {
        std::fill(input_buf_.begin(), input_buf_.end(), SampleType{0.0f});
        std::fill(output_buf_.begin(), output_buf_.end(), SampleType{0.0f});
        std::fill(overlap_.begin(), overlap_.end(), SampleType{0.0f});
        pos_ = 0;
    }

private:
    std::unique_ptr<FftT<SampleType>> fft_;
    int fft_size_ = 0;
    int block_size_ = 0;
    int pos_ = 0;

    std::vector<std::complex<SampleType>> ir_freq_;
    std::vector<SampleType> input_buf_;
    std::vector<SampleType> output_buf_;
    std::vector<SampleType> overlap_;
    std::vector<std::complex<SampleType>> freq_buf_;

    void process_block() {
        // input_buf_ is already zero past block_size_ (cleared after every
        // block), so it is the zero-padded real frame as it stands.
        fft_->forward_real_half(input_buf_.data(), freq_buf_.data());

        // Multiply in frequency domain; both spectra are Hermitian, so only
        // DC..Nyquist carries information.
        const int bins = fft_->real_bins();
        for (int i = 0; i < bins; ++i)
            freq_buf_[i] *= ir_freq_[i];

        // Real inverse in place, normalised.
        auto* time = reinterpret_cast<SampleType*>(freq_buf_.data());
        fft_->inverse_real_half(freq_buf_.data(), time);

        // Overlap-add. The tail of an IR longer than one block reaches several
        // blocks ahead, so the pending overlap is accumulated and shifted down
        // by one block rather than replaced by this block's tail alone.
        for (int i = 0; i < block_size_; ++i)
            output_buf_[i] = time[i] + overlap_[i];
        for (int i = block_size_; i < fft_size_; ++i)
            overlap_[i - block_size_] = time[i] + overlap_[i];
        std::fill(overlap_.end() - block_size_, overlap_.end(), SampleType{0.0f});

        // Clear input buffer
        std::fill(input_buf_.begin(), input_buf_.begin() + block_size_, SampleType{0.0f});
    }
};

using Convolver = ConvolverT<float>;
using Convolver64 = ConvolverT<double>;

} // namespace pulp::signal

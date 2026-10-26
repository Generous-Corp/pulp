#pragma once

#include <pulp/signal/checked_allocation.hpp>
#include <pulp/signal/rt_work_counter.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
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

struct FftRealSplit;

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

namespace detail {

/// Whether a FftT of `fft_size` points keeps a real_bins()-long complex
/// scratch for the split-spectrum path (FftRealSplit): every size the portable real
/// transform serves, which is all of them except vDSP's float sizes.
template <typename SampleType>
inline constexpr bool fft_real_split_needs_scratch(std::uint64_t fft_size) noexcept {
#if PULP_FFT_HAS_VDSP
    if constexpr (std::is_same_v<SampleType, float>)
        return fft_size < 4u;
#endif
    static_cast<void>(fft_size);
    return true;
}

} // namespace detail

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
        if (fft_size > (UINT64_MAX - kVdspSplitPaddingFloats) / 2u ||
            !charge.add<float>(2u * fft_size + kVdspSplitPaddingFloats))
            return false;
    }
#endif
    if (detail::fft_real_split_needs_scratch<SampleType>(fft_size) &&
        !charge.add<std::complex<SampleType>>(fft_size / 2u + 1u))
        return false;
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
// magnitude_db(), and size() are allocation-free after construction when callers provide
// valid input/output buffers.
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
        : size_(other.size_), twiddles_(std::move(other.twiddles_)),
          real_scratch_(std::move(other.real_scratch_))
#if PULP_FFT_HAS_VDSP
          ,
          log2n_(other.log2n_), vdsp_setup_(other.vdsp_setup_), split_(std::move(other.split_)),
          split_base_(other.split_base_)
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
            real_scratch_ = std::move(other.real_scratch_);
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
            double angle = -2.0 * std::numbers::pi * i / size;
            twiddles_[i] = {std::cos(angle), std::sin(angle)};
        }
        if (size > 0 &&
            detail::fft_real_split_needs_scratch<SampleType>(static_cast<std::uint64_t>(size)))
            real_scratch_.resize(static_cast<std::size_t>(size / 2 + 1));
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
        if (detail::fft_real_split_needs_scratch<SampleType>(static_cast<std::uint64_t>(size_)) &&
            real_scratch_.size() != static_cast<std::size_t>(size_ / 2 + 1))
            return false;
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            return vdsp_setup_ != nullptr &&
                   split_.size() == 2 * static_cast<std::size_t>(size_) + kVdspSplitPaddingFloats;
        }
#endif
        return true;
    }

    /// Byte offset past a 64-byte boundary of the real and imaginary halves
    /// the vDSP transforms use, or -1 when this transform does not use vDSP.
    /// Always 16 for a ready float transform of 32 points or more (see the
    /// placement note on the split storage); exposed so that contract can be
    /// tested rather than assumed.
    [[nodiscard]] int vdsp_split_offset_bytes(bool real_transform) const noexcept {
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            if (!ready())
                return -1;
            const DSPSplitComplex z = real_transform ? real_split() : split();
            const auto re = static_cast<int>(reinterpret_cast<std::uintptr_t>(z.realp) % 64u);
            const auto im = static_cast<int>(reinterpret_cast<std::uintptr_t>(z.imagp) % 64u);
            return re == im ? re : -2;
        }
#endif
        static_cast<void>(real_transform);
        return -1;
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
            if (size_ >= kVdspMinRealSize) {
                forward_real_vdsp(input, output);
                return;
            }
            forward_real_fallback(input, output);
        } else {
            forward_real_fallback(input, output);
        }
#else
        forward_real_fallback(input, output);
#endif
    }

    /// Smallest size() forward_real_half() / inverse_real_half() accept.
    static constexpr int kMinRealSize = 2;

    /// Independent bins of a real transform of size(): DC through Nyquist.
    int real_bins() const {
        return size_ / 2 + 1;
    }

    /// Real-input forward transform. Reads size() real samples and writes
    /// real_bins() bins, X[k] = scale * sum_n x[n] e^(-2 pi i k n / N) for
    /// k = 0..N/2. DC and Nyquist are written with zero imaginary part and
    /// nothing past the Nyquist bin is touched. `output` must not overlap
    /// `input`. Requires size() >= kMinRealSize. The two-argument overload
    /// uses scale 1.
    void forward_real_half(const SampleType* input, std::complex<SampleType>* output,
                           SampleType scale) const {
        rt::count_fft();
        assert(size_ >= kMinRealSize && "real transforms need at least two points");
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            if (size_ >= kVdspMinRealSize) {
                forward_real_half_vdsp(input, output, scale);
                return;
            }
        }
#endif
        forward_real_half_fallback(input, output, scale);
    }

    void forward_real_half(const SampleType* input, std::complex<SampleType>* output) const {
        forward_real_half(input, output, SampleType{1});
    }

    /// Inverse of `forward_real_half()`. Reads real_bins() bins (the imaginary
    /// parts of DC and Nyquist are ignored, as a real signal cannot carry
    /// them) and writes size() real samples equal to `scale` times the
    /// unnormalised inverse DFT of the Hermitian spectrum they define; the
    /// default scale 1/N recovers the forward transform's input. `output` may
    /// occupy the same storage as `input` (a complex buffer reinterpreted as
    /// real samples), which lets a caller transform a spectrum in place.
    /// Requires size() >= kMinRealSize. The two-argument overload uses scale
    /// 1/N.
    void inverse_real_half(const std::complex<SampleType>* input, SampleType* output,
                           SampleType scale) const {
        rt::count_fft();
        assert(size_ >= kMinRealSize && "real transforms need at least two points");
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            if (size_ >= kVdspMinRealSize) {
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
    friend struct detail::FftRealSplit;

    /// Real-input forward transform into packed split form. `re` and `im`
    /// each receive size() / 2 values: re[0] is the DC bin and im[0] the
    /// Nyquist bin (both purely real), and re[k], im[k] for k = 1..N/2-1 are
    /// bin k's real and imaginary parts, scaled as by `forward_real_half()`.
    /// Neither array may overlap `input` or the other. Requires size() >=
    /// kMinRealSize.
    void forward_real_split(const SampleType* input, SampleType* re, SampleType* im,
                            SampleType scale) const {
        rt::count_fft();
        assert(size_ >= kMinRealSize && "real transforms need at least two points");
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            if (size_ >= kVdspMinRealSize) {
                forward_real_split_vdsp(input, re, im, scale);
                return;
            }
        }
#endif
        forward_real_split_fallback(input, re, im, scale);
    }

    /// Inverse of `forward_real_split()`: reads a packed split spectrum and
    /// writes size() real samples equal to `scale` times the unnormalised
    /// inverse DFT of the Hermitian spectrum it defines; scale 1/N recovers
    /// the forward transform's input. `output` must not overlap `re` or `im`.
    /// Requires size() >= kMinRealSize.
    void inverse_real_split(const SampleType* re, const SampleType* im, SampleType* output,
                            SampleType scale) const {
        rt::count_fft();
        assert(size_ >= kMinRealSize && "real transforms need at least two points");
#if PULP_FFT_HAS_VDSP
        if constexpr (std::is_same_v<SampleType, float>) {
            if (size_ >= kVdspMinRealSize) {
                inverse_real_split_vdsp(re, im, output, scale);
                return;
            }
        }
#endif
        inverse_real_split_fallback(re, im, output, scale);
    }

    int size_ = 0;
    std::vector<std::complex<double>> twiddles_;
    // real_bins() bins for the portable split-spectrum path, which builds
    // the interleaved half spectrum first; empty where vDSP serves it.
    mutable std::vector<std::complex<SampleType>> real_scratch_;
#if PULP_FFT_HAS_VDSP
    int log2n_ = 0;
    FFTSetup vdsp_setup_ = nullptr;
    // One buffer holds both split halves so their placement is fixed rather
    // than left to the allocator. vDSP takes a separate code path when both
    // halves of a split complex array sit on 64-byte boundaries, and that
    // path is the less accurate one: from 512 points up it rounds 0.4-5.3 dB
    // worse (worst bin error relative to the largest bin, against a
    // long-double DFT; about -131.6 to -137.6 dB on the boundary against
    // -135.8 to -141.4 dB off it) on every processor measured: real and
    // complex transforms on M1 Max, M5 Max and M5 Ultra, real transforms on
    // M3 Ultra. Both halves therefore always sit 16 bytes past a 64-byte
    // boundary.
    //
    // The cost of that choice depends on the processor. Forward transforms in
    // this buffer layout, at low priority on shared machines:
    //   - M5 Max and M5 Ultra (paired, interleaved; loaded): off the boundary
    //     is the faster path from 512 to 8192 points, by 1.08-2.16x for real
    //     transforms and 1.15-2.5x for complex ones (3.5-4.4x for complex at
    //     256); the two are within 0.1% at 256 real points, and at 16384 the
    //     boundary path takes 0.93-1.14x the time.
    //   - M1 Max (paired, interleaved; background QoS): the boundary path is
    //     faster from 2048 real and 1024 complex points, by 1.17-1.75x and
    //     1.19-2.1x.
    //   - M3 Ultra (best of five, one session, load not recorded): the
    //     boundary path is faster from 1024 real and 512 complex points, by
    //     1.06-1.42x and 1.18-1.59x.
    // On M1 Max and M3 Ultra, then, large transforms give up that much speed
    // for the accuracy above.
    //
    // Separate allocations made the same transform's speed and error depend
    // on where the buffers happened to land.
    mutable std::vector<float> split_;
    std::size_t split_base_ = 0;
    static constexpr std::size_t kVdspSplitOffsetFloats = 4; // 16 bytes
    static_assert(kVdspSplitPaddingFloats >= 64 / sizeof(float) - 1 + kVdspSplitOffsetFloats,
                  "split padding must cover the 64-byte alignment slack plus the 16-byte step");
#endif

#if PULP_FFT_HAS_VDSP
    // N-point halves for complex transforms, each 16 bytes past a 64-byte
    // boundary (N is a power of two, so the second half keeps the offset).
    DSPSplitComplex split() const {
        float* base = split_.data() + split_base_ + kVdspSplitOffsetFloats;
        return {base, base + size_};
    }

    // N/2-point halves for real transforms, placed like split()'s.
    DSPSplitComplex real_split() const {
        float* base = split_.data() + split_base_ + kVdspSplitOffsetFloats;
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
        out[1] = 0.0f; // DC has no imaginary part
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

    // vDSP's packed real transform already has the split layout; only its 2x
    // gain is folded into the copy out of the placed buffer.
    void forward_real_split_vdsp(const float* input, float* re, float* im, float scale) const {
        forward_real_packed_vdsp(input);
        const DSPSplitComplex z = real_split();
        const float s = 0.5f * scale;
        const auto half = static_cast<vDSP_Length>(size_ / 2);
        vDSP_vsmul(z.realp, 1, &s, re, 1, half);
        vDSP_vsmul(z.imagp, 1, &s, im, 1, half);
    }

    void inverse_real_split_vdsp(const float* re, const float* im, float* output,
                                 float scale) const {
        const int half = size_ / 2;
        DSPSplitComplex z = real_split();
        std::copy_n(re, half, z.realp);
        std::copy_n(im, half, z.imagp);
        vDSP_fft_zrip(vdsp_setup_, &z, 1, log2n_, kFFTDirection_Inverse);
        if (scale == 1.0f)
            vDSP_ztoc(&z, 1, reinterpret_cast<DSPComplex*>(output), 2,
                      static_cast<vDSP_Length>(half));
        else
            interleave_scaled(z.realp, z.imagp, output, half, scale);
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
    // vDSP's packed real transform needs two complex points; smaller real
    // transforms take the portable path.
    static constexpr int kVdspMinRealSize = 4;

    void forward_real_half_fallback(const SampleType* input, std::complex<SampleType>* output,
                                    SampleType scale) const {
        assert(size_ >= kMinRealSize);
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
        assert(size_ >= kMinRealSize);
        const int half = size_ / 2;
        const SampleType nyquist = input[half].real();
        const SampleType dc = input[0].real();
        // N real samples are N/2 complex values in the same storage
        // ([complex.numbers] guarantees std::complex<T> is an array of two T),
        // which is what lets the N/2-point transform run in the output.
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

    void forward_real_split_fallback(const SampleType* input, SampleType* re, SampleType* im,
                                     SampleType scale) const {
        std::complex<SampleType>* bins = real_scratch_.data();
        forward_real_half_fallback(input, bins, scale);
        const int half = size_ / 2;
        for (int k = 0; k < half; ++k) {
            re[k] = bins[k].real();
            im[k] = bins[k].imag();
        }
        im[0] = bins[half].real();
    }

    void inverse_real_split_fallback(const SampleType* re, const SampleType* im, SampleType* output,
                                     SampleType scale) const {
        std::complex<SampleType>* bins = real_scratch_.data();
        const int half = size_ / 2;
        for (int k = 0; k < half; ++k)
            bins[k] = {re[k], im[k]};
        bins[0] = {re[0], SampleType{0}};
        bins[half] = {im[0], SampleType{0}};
        inverse_real_half_fallback(bins, output, scale);
    }

    void bit_reverse(std::complex<SampleType>* data) const {
        bit_reverse(data, size_);
    }

    void bit_reverse(std::complex<SampleType>* data, int count) const {
        int bits = 0;
        for (int n = count; n > 1; n >>= 1)
            ++bits;

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

namespace detail {

/// The packed split-complex form of a real transform, for Pulp's own
/// frequency-domain stages (the overlap-add ConvolverT). `re` and `im` each
/// hold size() / 2 values: re[0] is the DC bin and im[0] the Nyquist bin
/// (both purely real), and re[k], im[k] for k = 1..N/2-1 are bin k, scaled
/// as by `forward_real_half()`. It is the layout vDSP's real transform
/// produces, so a stage that stays in it skips the interleave on the way in
/// and out. Not public API: the partitioned engines keep interleaved bins,
/// which their complex multiply-accumulate kernels run fastest on.
struct FftRealSplit {
    template <typename SampleType>
    static void forward(const FftT<SampleType>& fft, const SampleType* input, SampleType* re,
                        SampleType* im, SampleType scale) {
        fft.forward_real_split(input, re, im, scale);
    }

    template <typename SampleType>
    static void inverse(const FftT<SampleType>& fft, const SampleType* re, const SampleType* im,
                        SampleType* output, SampleType scale) {
        fft.inverse_real_split(re, im, output, scale);
    }
};

/// Product of two packed split spectra of `half` = size() / 2 slots each
/// (see FftRealSplit): bin k of `out` is bin k of `a` times
/// bin k of `b` for k = 1..half-1, and slot 0's DC and Nyquist reals multiply
/// separately. The result is the packed spectrum of the circular convolution.
/// `out` may be `a` or `b`; partial overlap is not allowed. Allocation-free.
template <typename SampleType>
inline void multiply_packed_split(const SampleType* a_re, const SampleType* a_im,
                                  const SampleType* b_re, const SampleType* b_im,
                                  SampleType* out_re, SampleType* out_im, int half) noexcept {
    if (half <= 0)
        return;
    const SampleType dc = a_re[0] * b_re[0];
    const SampleType nyquist = a_im[0] * b_im[0];
    for (int k = 0; k < half; ++k) {
        const SampleType re = a_re[k] * b_re[k] - a_im[k] * b_im[k];
        const SampleType im = a_re[k] * b_im[k] + a_im[k] * b_re[k];
        out_re[k] = re;
        out_im[k] = im;
    }
    out_re[0] = dc;
    out_im[0] = nyquist;
}

/// `acc += a * b` over packed split spectra, with the product as in
/// `multiply_packed_split()`. `acc` must not overlap `a` or `b`.
/// Allocation-free.
template <typename SampleType>
inline void multiply_accumulate_packed_split(const SampleType* a_re, const SampleType* a_im,
                                             const SampleType* b_re, const SampleType* b_im,
                                             SampleType* acc_re, SampleType* acc_im,
                                             int half) noexcept {
    if (half <= 0)
        return;
    const SampleType dc = acc_re[0] + a_re[0] * b_re[0];
    const SampleType nyquist = acc_im[0] + a_im[0] * b_im[0];
    for (int k = 0; k < half; ++k) {
        acc_re[k] += a_re[k] * b_re[k] - a_im[k] * b_im[k];
        acc_im[k] += a_re[k] * b_im[k] + a_im[k] * b_re[k];
    }
    acc_re[0] = dc;
    acc_im[0] = nyquist;
}

} // namespace detail

} // namespace pulp::signal

// ConvolverT is declared in its own header; it stays reachable through this
// one for code that has always included fft.hpp for it.
#include <pulp/signal/overlap_add_convolver.hpp>

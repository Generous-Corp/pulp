#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#define PULP_FFT_HAS_VDSP 1
#endif

namespace pulp::signal {

// Radix-2 FFT — in-place, decimation-in-time.
// Size must be a power of 2.
// On Apple platforms, uses vDSP for significantly faster large transforms.
//
// RT contract: construction/destruction allocate or release twiddle/vDSP
// storage and are prepare/control-thread work. forward(), inverse(),
// forward_real(), magnitude(), magnitude_db(), and size() are allocation-free
// after construction when callers provide valid input/output buffers.
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
        , split_real_(std::move(other.split_real_)), split_imag_(std::move(other.split_imag_))
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
            split_real_ = std::move(other.split_real_);
            split_imag_ = std::move(other.split_imag_);
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
            vdsp_setup_ = vDSP_create_fftsetup(log2n_, kFFTRadix2);
            split_real_.resize(size);
            split_imag_.resize(size);
        }
#endif
        // Pre-compute twiddle factors (used as fallback on non-Apple)
        twiddles_.resize(size / 2);
        for (int i = 0; i < size / 2; ++i) {
            double angle = -2.0 * pi * i / size;
            twiddles_[i] = {std::cos(angle), std::sin(angle)};
        }
    }

    ~FftT() {
#if PULP_FFT_HAS_VDSP
        if (vdsp_setup_) vDSP_destroy_fftsetup(vdsp_setup_);
#endif
    }

    int size() const { return size_; }

    // Forward FFT (time → frequency) — complex in-place
    void forward(std::complex<SampleType>* data) const {
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
    mutable std::vector<float> split_real_;
    mutable std::vector<float> split_imag_;
#endif

#if PULP_FFT_HAS_VDSP
    // Deinterleave std::complex<float> array into split-complex format
    void to_split(const std::complex<float>* data) const {
        for (int i = 0; i < size_; ++i) {
            split_real_[i] = data[i].real();
            split_imag_[i] = data[i].imag();
        }
    }

    // Interleave split-complex format back to std::complex<float>
    void from_split(std::complex<float>* data) const {
        for (int i = 0; i < size_; ++i) {
            data[i] = {split_real_[i], split_imag_[i]};
        }
    }

    void forward_vdsp(std::complex<float>* data) const {
        to_split(data);
        DSPSplitComplex split = {split_real_.data(), split_imag_.data()};
        vDSP_fft_zip(vdsp_setup_, &split, 1, log2n_, kFFTDirection_Forward);
        from_split(data);
    }

    void inverse_vdsp(std::complex<float>* data) const {
        to_split(data);
        DSPSplitComplex split = {split_real_.data(), split_imag_.data()};
        vDSP_fft_zip(vdsp_setup_, &split, 1, log2n_, kFFTDirection_Inverse);
        // vDSP inverse doesn't normalize — divide by N
        float scale = 1.0f / size_;
        vDSP_vsmul(split_real_.data(), 1, &scale, split_real_.data(), 1, static_cast<vDSP_Length>(size_));
        vDSP_vsmul(split_imag_.data(), 1, &scale, split_imag_.data(), 1, static_cast<vDSP_Length>(size_));
        from_split(data);
    }

    void forward_real_vdsp(const float* input, std::complex<float>* output) const {
        // Pack real data into split-complex: even samples → real, odd → imag
        int half = size_ / 2;
        for (int i = 0; i < half; ++i) {
            split_real_[i] = input[2 * i];
            split_imag_[i] = input[2 * i + 1];
        }
        DSPSplitComplex split = {split_real_.data(), split_imag_.data()};
        vDSP_fft_zrip(vdsp_setup_, &split, 1, log2n_, kFFTDirection_Forward);

        // vDSP_fft_zrip packs result: split.realp[0] = DC, split.imagp[0] = Nyquist
        // Unpack to standard complex format
        output[0] = {split_real_[0], 0.0f};        // DC (real only)
        output[half] = {split_imag_[0], 0.0f};     // Nyquist (real only)
        for (int i = 1; i < half; ++i) {
            output[i] = {split_real_[i], split_imag_[i]};
            // Conjugate symmetry: X[N-k] = conj(X[k])
            output[size_ - i] = {split_real_[i], -split_imag_[i]};
        }
        // vDSP real FFT has implicit 2x scale factor
        float scale = 0.5f;
        for (int i = 0; i < size_; ++i) {
            output[i] = {output[i].real() * scale, output[i].imag() * scale};
        }
    }
#endif

    void forward_fallback(std::complex<SampleType>* data) const {
        bit_reverse(data);
        for (int len = 2; len <= size_; len <<= 1) {
            int half = len / 2;
            int step = size_ / len;
            for (int i = 0; i < size_; i += len) {
                for (int j = 0; j < half; ++j) {
                    auto w = std::complex<SampleType>(twiddles_[j * step]);
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
        for (int i = 0; i < size_; ++i)
            output[i] = {input[i], SampleType{0.0f}};
        forward(output);
    }

    void bit_reverse(std::complex<SampleType>* data) const {
        int bits = 0;
        for (int n = size_; n > 1; n >>= 1) ++bits;

        for (int i = 0; i < size_; ++i) {
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

// Simple frequency-domain convolver using overlap-add.
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
        fft_size_ = 1;
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
        // Zero-pad input to FFT size
        for (int i = 0; i < fft_size_; ++i)
            freq_buf_[i] = i < block_size_
                ? std::complex<SampleType>(input_buf_[i], SampleType{0.0f})
                : std::complex<SampleType>(SampleType{0.0f}, SampleType{0.0f});

        // Forward FFT
        fft_->forward(freq_buf_.data());

        // Multiply in frequency domain
        for (int i = 0; i < fft_size_; ++i)
            freq_buf_[i] *= ir_freq_[i];

        // Inverse FFT
        fft_->inverse(freq_buf_.data());

        // Overlap-add
        for (int i = 0; i < fft_size_; ++i) {
            SampleType val = freq_buf_[i].real() + overlap_[i];
            if (i < block_size_)
                output_buf_[i] = val;
            overlap_[i] = SampleType{0.0f};
        }

        // Save overlap for next block
        for (int i = block_size_; i < fft_size_; ++i)
            overlap_[i - block_size_] = freq_buf_[i].real();

        // Clear input buffer
        std::fill(input_buf_.begin(), input_buf_.end(), SampleType{0.0f});
    }
};

using Convolver = ConvolverT<float>;
using Convolver64 = ConvolverT<double>;

} // namespace pulp::signal

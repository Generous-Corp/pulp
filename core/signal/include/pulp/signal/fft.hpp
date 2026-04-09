#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <cstdint>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#define PULP_FFT_HAS_VDSP 1
#endif

namespace pulp::signal {

// Radix-2 FFT — in-place, decimation-in-time
// Size must be a power of 2.
// On Apple platforms, uses vDSP for significantly faster large transforms.
class Fft {
public:
    Fft() = default;

    // Non-copyable (vDSP_setup handle), movable
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;

    Fft(Fft&& other) noexcept
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

    Fft& operator=(Fft&& other) noexcept {
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

    explicit Fft(int size) : size_(size) {
#if PULP_FFT_HAS_VDSP
        // Use vDSP for FFT — much faster for large sizes
        log2n_ = 0;
        for (int n = size; n > 1; n >>= 1) ++log2n_;
        vdsp_setup_ = vDSP_create_fftsetup(log2n_, kFFTRadix2);
        split_real_.resize(size);
        split_imag_.resize(size);
#endif
        // Pre-compute twiddle factors (used as fallback on non-Apple)
        twiddles_.resize(size / 2);
        for (int i = 0; i < size / 2; ++i) {
            double angle = -2.0 * pi * i / size;
            twiddles_[i] = {std::cos(angle), std::sin(angle)};
        }
    }

    ~Fft() {
#if PULP_FFT_HAS_VDSP
        if (vdsp_setup_) vDSP_destroy_fftsetup(vdsp_setup_);
#endif
    }

    int size() const { return size_; }

    // Forward FFT (time → frequency) — complex in-place
    void forward(std::complex<float>* data) const {
#if PULP_FFT_HAS_VDSP
        forward_vdsp(data);
#else
        forward_fallback(data);
#endif
    }

    // Inverse FFT (frequency → time) — complex in-place
    void inverse(std::complex<float>* data) const {
#if PULP_FFT_HAS_VDSP
        inverse_vdsp(data);
#else
        for (int i = 0; i < size_; ++i) data[i] = std::conj(data[i]);
        forward_fallback(data);
        float scale = 1.0f / size_;
        for (int i = 0; i < size_; ++i) data[i] = std::conj(data[i]) * scale;
#endif
    }

    // Real-valued forward FFT: float input → complex output
    void forward_real(const float* input, std::complex<float>* output) const {
#if PULP_FFT_HAS_VDSP
        forward_real_vdsp(input, output);
#else
        for (int i = 0; i < size_; ++i)
            output[i] = {input[i], 0.0f};
        forward(output);
#endif
    }

    // Compute magnitude spectrum in dB
    void magnitude_db(const std::complex<float>* freq, float* out, int num_bins) const {
        for (int i = 0; i < num_bins; ++i) {
            float mag = std::abs(freq[i]);
            out[i] = 20.0f * std::log10(std::max(mag, 1e-10f));
        }
    }

    // Compute magnitude spectrum (linear)
    void magnitude(const std::complex<float>* freq, float* out, int num_bins) const {
        for (int i = 0; i < num_bins; ++i) {
            out[i] = std::abs(freq[i]);
        }
    }

private:
    static constexpr double pi = 3.14159265358979323846;
    int size_;
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

    void forward_fallback(std::complex<float>* data) const {
        bit_reverse(data);
        for (int len = 2; len <= size_; len <<= 1) {
            int half = len / 2;
            int step = size_ / len;
            for (int i = 0; i < size_; i += len) {
                for (int j = 0; j < half; ++j) {
                    auto w = std::complex<float>(twiddles_[j * step]);
                    auto u = data[i + j];
                    auto v = data[i + j + half] * w;
                    data[i + j] = u + v;
                    data[i + j + half] = u - v;
                }
            }
        }
    }

    void bit_reverse(std::complex<float>* data) const {
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

// ── Convolver ────────────────────────────────────────────────────────────────

// Simple frequency-domain convolver using overlap-add
class Convolver {
public:
    // Load an impulse response
    void load_ir(const float* ir, int ir_length, int block_size = 0) {
        if (ir_length <= 0) return;

        block_size_ = block_size > 0 ? block_size : 256;

        // FFT size: next power of 2 >= block_size + ir_length - 1
        fft_size_ = 1;
        while (fft_size_ < block_size_ + ir_length) fft_size_ <<= 1;

        fft_ = std::make_unique<Fft>(fft_size_);

        // Transform IR
        ir_freq_.resize(fft_size_);
        for (int i = 0; i < fft_size_; ++i)
            ir_freq_[i] = i < ir_length ? std::complex<float>(ir[i], 0) : std::complex<float>(0, 0);
        fft_->forward(ir_freq_.data());

        // Buffers
        input_buf_.resize(fft_size_, 0);
        output_buf_.resize(fft_size_, 0);
        overlap_.resize(fft_size_, 0);
        freq_buf_.resize(fft_size_);
        pos_ = 0;
    }

    // Process a single sample
    float process(float input) {
        input_buf_[pos_] = input;
        float output = output_buf_[pos_];
        ++pos_;

        if (pos_ >= block_size_) {
            process_block();
            pos_ = 0;
        }

        return output;
    }

    // Process a buffer
    void process(const float* input, float* output, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            output[i] = process(input[i]);
    }

    void reset() {
        std::fill(input_buf_.begin(), input_buf_.end(), 0.0f);
        std::fill(output_buf_.begin(), output_buf_.end(), 0.0f);
        std::fill(overlap_.begin(), overlap_.end(), 0.0f);
        pos_ = 0;
    }

private:
    std::unique_ptr<Fft> fft_;
    int fft_size_ = 0;
    int block_size_ = 0;
    int pos_ = 0;

    std::vector<std::complex<float>> ir_freq_;
    std::vector<float> input_buf_;
    std::vector<float> output_buf_;
    std::vector<float> overlap_;
    std::vector<std::complex<float>> freq_buf_;

    void process_block() {
        // Zero-pad input to FFT size
        for (int i = 0; i < fft_size_; ++i)
            freq_buf_[i] = i < block_size_ ? std::complex<float>(input_buf_[i], 0) : std::complex<float>(0, 0);

        // Forward FFT
        fft_->forward(freq_buf_.data());

        // Multiply in frequency domain
        for (int i = 0; i < fft_size_; ++i)
            freq_buf_[i] *= ir_freq_[i];

        // Inverse FFT
        fft_->inverse(freq_buf_.data());

        // Overlap-add
        for (int i = 0; i < fft_size_; ++i) {
            float val = freq_buf_[i].real() + overlap_[i];
            if (i < block_size_)
                output_buf_[i] = val;
            overlap_[i] = 0;
        }

        // Save overlap for next block
        for (int i = block_size_; i < fft_size_; ++i)
            overlap_[i - block_size_] = freq_buf_[i].real();

        // Clear input buffer
        std::fill(input_buf_.begin(), input_buf_.end(), 0.0f);
    }
};

} // namespace pulp::signal

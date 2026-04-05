#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace pulp::signal {

// Radix-2 FFT — in-place, decimation-in-time
// Size must be a power of 2
class Fft {
public:
    explicit Fft(int size) : size_(size) {
        // Pre-compute twiddle factors
        twiddles_.resize(size / 2);
        for (int i = 0; i < size / 2; ++i) {
            double angle = -2.0 * pi * i / size;
            twiddles_[i] = {std::cos(angle), std::sin(angle)};
        }
    }

    int size() const { return size_; }

    // Forward FFT (time → frequency)
    void forward(std::complex<float>* data) const {
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

    // Inverse FFT (frequency → time)
    void inverse(std::complex<float>* data) const {
        // Conjugate, forward FFT, conjugate, scale
        for (int i = 0; i < size_; ++i) data[i] = std::conj(data[i]);
        forward(data);
        float scale = 1.0f / size_;
        for (int i = 0; i < size_; ++i) data[i] = std::conj(data[i]) * scale;
    }

    // Real-valued forward FFT: float input → complex output
    void forward_real(const float* input, std::complex<float>* output) const {
        for (int i = 0; i < size_; ++i)
            output[i] = {input[i], 0.0f};
        forward(output);
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

#pragma once

/// @file overlap_add_convolver.hpp
/// Single-partition overlap-add convolution on the real-input FFT.

#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace pulp::signal {

// Simple frequency-domain convolver using overlap-add. Output is delayed by
// one block: a block is transformed once it has filled, and its result is
// read out while the next one fills.
//
// RT contract: load_ir() allocates FFT and overlap-add buffers and is not
// audio-thread safe. process(), buffer process(), and reset() are
// allocation-free after a valid impulse response has been loaded.
template <typename SampleType = float> class ConvolverT {
  public:
    // Load an impulse response
    void load_ir(const SampleType* ir, int ir_length, int block_size = 0) {
        if (ir == nullptr || ir_length <= 0) {
            fft_.reset();
            ir_spectrum_.clear();
            input_buf_.clear();
            output_buf_.clear();
            overlap_.clear();
            spectrum_.clear();
            time_.clear();
            fft_size_ = 0;
            block_size_ = 0;
            pos_ = 0;
            return;
        }

        block_size_ = block_size > 0 ? block_size : 256;

        // FFT size: next power of 2 >= block_size + ir_length - 1
        // The real transform needs at least two points.
        fft_size_ = 2;
        while (fft_size_ < block_size_ + ir_length)
            fft_size_ <<= 1;

        fft_ = std::make_unique<FftT<SampleType>>(fft_size_);

        // Transform the zero-padded IR into packed split form. Every block
        // takes its input transform at scale 2 and inverts at scale 1 (both
        // free on vDSP, whose real transform has a gain of 2), so the IR
        // carries the remaining 1/(2N) that makes the round trip exact.
        time_.assign(fft_size_, SampleType{0.0f});
        std::copy_n(ir, ir_length, time_.begin());
        ir_spectrum_.assign(fft_size_, SampleType{0.0f});
        const int half = fft_size_ / 2;
        detail::FftRealSplit::forward(*fft_, time_.data(), ir_spectrum_.data(),
                                      ir_spectrum_.data() + half,
                                      SampleType{1} / static_cast<SampleType>(2 * fft_size_));

        // Buffers
        input_buf_.assign(fft_size_, SampleType{0.0f});
        output_buf_.assign(fft_size_, SampleType{0.0f});
        overlap_.assign(fft_size_, SampleType{0.0f});
        spectrum_.assign(fft_size_, SampleType{0.0f});
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

    // Packed split spectra (detail::FftRealSplit): N/2 real parts, then
    // N/2 imaginary parts.
    std::vector<SampleType> ir_spectrum_;
    std::vector<SampleType> spectrum_;
    std::vector<SampleType> input_buf_;
    std::vector<SampleType> output_buf_;
    std::vector<SampleType> overlap_;
    std::vector<SampleType> time_;

    void process_block() {
        // input_buf_ is already zero past block_size_ (cleared after every
        // block), so it is the zero-padded real frame as it stands.
        // Both spectra are Hermitian, so only DC..Nyquist carries
        // information, and they stay in split form from transform to inverse.
        const int half = fft_size_ / 2;
        SampleType* re = spectrum_.data();
        SampleType* im = re + half;
        detail::FftRealSplit::forward(*fft_, input_buf_.data(), re, im, SampleType{2});
        detail::multiply_packed_split(re, im, ir_spectrum_.data(), ir_spectrum_.data() + half, re,
                                      im, half);
        SampleType* time = time_.data();
        detail::FftRealSplit::inverse(*fft_, re, im, time, SampleType{1});

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

#pragma once

#include <pulp/signal/fft.hpp>
#include <vector>
#include <complex>
#include <cstddef>
#include <algorithm>
#include <memory>
#include <cassert>

namespace pulp::signal {

/// Uniform partitioned convolution engine.
///
/// Processes audio through an impulse response using overlap-save with
/// uniform block partitioning. Zero latency (current block output is immediate).
///
/// Usage:
///   PartitionedConvolver conv;
///   conv.load_ir(ir_data, ir_length, block_size);
///   // In process callback:
///   conv.process(input, output, num_samples);
class PartitionedConvolver {
public:
    PartitionedConvolver() = default;

    /// Load an impulse response. block_size should match your audio callback size.
    /// block_size must be a power of two (for the radix-2 FFT).
    /// If block_size is not a power of two, it is rounded up to the next one.
    void load_ir(const float* ir, size_t ir_length, size_t block_size) {
        // Validate power-of-two requirement for the radix-2 FFT
        if (block_size == 0 || (block_size & (block_size - 1)) != 0) {
            // Round up to next power of two
            size_t pot = 1;
            while (pot < block_size) pot <<= 1;
            block_size = pot;
        }
        block_size_ = static_cast<int>(block_size);
        fft_size_ = block_size_ * 2;
        fft_ = std::make_unique<Fft>(fft_size_);

        num_partitions_ = (ir_length + block_size - 1) / block_size;

        // Pre-compute FFT of each IR partition
        ir_spectra_.resize(num_partitions_);
        std::vector<std::complex<float>> padded(fft_size_, {0, 0});

        for (size_t p = 0; p < num_partitions_; ++p) {
            size_t offset = p * block_size;
            size_t count = std::min(block_size, ir_length - offset);

            std::fill(padded.begin(), padded.end(), std::complex<float>{0, 0});
            for (size_t i = 0; i < count; ++i)
                padded[i] = {ir[offset + i], 0};

            ir_spectra_[p].resize(fft_size_);
            std::copy(padded.begin(), padded.end(), ir_spectra_[p].begin());
            fft_->forward(ir_spectra_[p].data());
        }

        input_buffer_.assign(fft_size_, {0, 0});
        input_spectra_.resize(num_partitions_);
        for (auto& s : input_spectra_) s.assign(fft_size_, {0, 0});
        accum_.assign(fft_size_, {0, 0});
        partition_index_ = 0;
    }

    /// Process a block of audio. num_samples must equal block_size.
    void process(const float* input, float* output, size_t num_samples) {
        if (!fft_ || ir_spectra_.empty() || static_cast<int>(num_samples) != block_size_) {
            std::copy_n(input, num_samples, output);
            return;
        }

        for (int i = 0; i < block_size_; ++i)
            input_buffer_[block_size_ + i] = {input[i], 0};

        auto& current_spectrum = input_spectra_[partition_index_];
        std::copy(input_buffer_.begin(), input_buffer_.end(), current_spectrum.begin());
        fft_->forward(current_spectrum.data());

        std::fill(accum_.begin(), accum_.end(), std::complex<float>{0, 0});
        for (size_t p = 0; p < num_partitions_; ++p) {
            size_t idx = (partition_index_ + num_partitions_ - p) % num_partitions_;
            for (int i = 0; i < fft_size_; ++i)
                accum_[i] += input_spectra_[idx][i] * ir_spectra_[p][i];
        }

        fft_->inverse(accum_.data());

        for (int i = 0; i < block_size_; ++i)
            output[i] = accum_[block_size_ + i].real();

        std::copy_n(input_buffer_.begin() + block_size_, block_size_, input_buffer_.begin());
        std::fill(input_buffer_.begin() + block_size_, input_buffer_.end(), std::complex<float>{0, 0});

        partition_index_ = (partition_index_ + 1) % num_partitions_;
    }

    void reset() {
        for (auto& s : input_spectra_) std::fill(s.begin(), s.end(), std::complex<float>{0, 0});
        std::fill(input_buffer_.begin(), input_buffer_.end(), std::complex<float>{0, 0});
        partition_index_ = 0;
    }

    /// Returns the algorithmic latency in samples.
    /// Overlap-save produces valid output for the current block immediately
    /// (partition 0 is applied in the same callback), so latency is 0.
    size_t latency() const { return 0; }
    size_t num_partitions() const { return num_partitions_; }
    bool is_loaded() const { return !ir_spectra_.empty(); }

private:
    int block_size_ = 0;
    int fft_size_ = 0;
    size_t num_partitions_ = 0;
    size_t partition_index_ = 0;

    std::unique_ptr<Fft> fft_;
    std::vector<std::vector<std::complex<float>>> ir_spectra_;
    std::vector<std::vector<std::complex<float>>> input_spectra_;
    std::vector<std::complex<float>> input_buffer_;
    std::vector<std::complex<float>> accum_;
};

} // namespace pulp::signal

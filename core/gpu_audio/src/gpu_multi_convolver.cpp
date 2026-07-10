#include <pulp/gpu_audio/gpu_multi_convolver.hpp>

#include <pulp/gpu_audio/detail/gpu_ola.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pulp::gpu_audio {

GpuMultiConvolver::GpuMultiConvolver(uint32_t block_size, uint32_t sample_rate,
                                     std::vector<std::vector<float>> irs)
    : block_(block_size), sample_rate_(sample_rate), irs_(std::move(irs)),
      num_ir_(static_cast<uint32_t>(irs_.size())) {}

GpuAudioNodeDescriptor GpuMultiConvolver::descriptor() const {
    GpuAudioNodeDescriptor d;
    d.name = "gpu-multi-convolver";
    d.input_channels = 2;
    d.output_channels = 2;
    d.block_size = block_;
    d.sample_rate = sample_rate_;
    d.latency_blocks = 2;
    // The multi-room mode is the regime CPU cannot sustain at scale, so there is
    // no honest real-time CPU fallback: a (rare) miss outputs silence.
    d.miss_policy = MissPolicy::Silence;
    d.supports_cpu_fallback = false;
    return d;
}

bool GpuMultiConvolver::prepare() {
    prepared_ = false;
    if (block_ == 0 || num_ir_ == 0) return false;

    std::size_t max_ir = 0;
    for (const auto& ir : irs_) max_ir = std::max(max_ir, ir.size());
    if (max_ir == 0) return false;

    // fft_size = next power of two >= block + longest IR (linear convolution via
    // overlap-add; no time aliasing).
    fft_size_ = 1;
    while (fft_size_ < block_ + static_cast<uint32_t>(max_ir)) fft_size_ <<= 1;

    const uint32_t cplx = fft_size_ * 2u;
    in_pad_.assign(cplx, 0.0f);
    out_lr_.assign(cplx, 0.0f);              // L block [0,fft) + R block [fft,2fft)
    carry_l_.assign(fft_size_, 0.0f);
    carry_r_.assign(fft_size_, 0.0f);

    // Constant-power pan: spread the rooms evenly across the stereo field, scaled
    // by 1/sqrt(num_ir) so summing decorrelated rooms keeps a sane level.
    pan_l_.assign(num_ir_, 0.0f);
    pan_r_.assign(num_ir_, 0.0f);
    const float norm = 1.0f / std::sqrt(static_cast<float>(num_ir_));
    for (uint32_t k = 0; k < num_ir_; ++k) {
        const float t = (num_ir_ == 1) ? 0.5f
                                       : static_cast<float>(k) / static_cast<float>(num_ir_ - 1);
        const float theta = t * 1.57079632679f;  // 0..pi/2
        pan_l_[k] = std::cos(theta) * norm;
        pan_r_[k] = std::sin(theta) * norm;
    }

    gpu_ = render::GpuCompute::create();
    if (!gpu_ || !gpu_->initialize_standalone()) {
        gpu_.reset();
        return false;
    }

    // Build the num_ir IR spectra: zero-pad each IR to fft_size, forward-FFT on
    // the GPU, pack contiguously. Done once at prepare (non-RT).
    std::vector<float> ir_specs(static_cast<std::size_t>(cplx) * num_ir_, 0.0f);
    std::vector<float> pad(cplx, 0.0f);
    std::vector<float> spec(cplx, 0.0f);
    for (uint32_t k = 0; k < num_ir_; ++k) {
        std::fill(pad.begin(), pad.end(), 0.0f);
        const auto& ir = irs_[k];
        for (std::size_t i = 0; i < ir.size() && i < fft_size_; ++i)
            pad[2u * i] = ir[i];  // real; imag stays 0
        if (!gpu_->fft_forward(pad.data(), spec.data(), fft_size_)) {
            gpu_.reset();
            return false;
        }
        std::copy(spec.begin(), spec.end(),
                  ir_specs.begin() + static_cast<std::size_t>(k) * cplx);
    }

    if (!gpu_->prepare_multi_convolution(fft_size_, ir_specs.data(), num_ir_)) {
        gpu_.reset();
        return false;
    }

    prepared_ = true;
    return true;
}

bool GpuMultiConvolver::convolve_stereo(const float* mono_in, float* out_l,
                                        float* out_r, uint32_t n) {
    if (!prepared_ || !gpu_ || n != block_) return false;

    // Zero-padded complex input (one shared block for all rooms).
    std::fill(in_pad_.begin(), in_pad_.end(), 0.0f);
    for (uint32_t i = 0; i < n; ++i) in_pad_[2u * i] = mono_in[i];

    if (!gpu_->multi_convolve(in_pad_.data(), pan_l_.data(), pan_r_.data(),
                              out_lr_.data(), fft_size_, num_ir_)) {
        return false;
    }

    // Overlap-add per channel: out_lr_ holds the full-length (fft_size) panned
    // L and R results (packed real, stride 1); add into the carry, emit the first
    // n, shift the carry. The guarded add resets a channel's carry and emits
    // silence on a non-finite readback rather than poisoning it for the session.
    const float* res_l = out_lr_.data();              // [0, fft_size)
    const float* res_r = out_lr_.data() + fft_size_;  // [fft_size, 2*fft_size)
    detail::overlap_add_block(carry_l_.data(), res_l, /*src_stride=*/1, out_l, fft_size_, n);
    detail::overlap_add_block(carry_r_.data(), res_r, /*src_stride=*/1, out_r, fft_size_, n);
    return true;
}

void GpuMultiConvolver::process_block(const audio::BufferView<const float>& input,
                                      audio::BufferView<float>& output, uint32_t n) {
    if (!prepared_ || !gpu_ || n != block_ || output.num_channels() < 2) {
        output.clear();
        return;
    }
    // Downmix the stereo input to one mono drive signal for the room bank.
    static thread_local std::vector<float> mono;
    mono.assign(n, 0.0f);
    const uint32_t in_ch = static_cast<uint32_t>(input.num_channels());
    if (in_ch >= 2) {
        const float* l = input.channel_ptr(0);
        const float* r = input.channel_ptr(1);
        for (uint32_t i = 0; i < n; ++i) mono[i] = 0.5f * (l[i] + r[i]);
    } else if (in_ch == 1) {
        const float* l = input.channel_ptr(0);
        for (uint32_t i = 0; i < n; ++i) mono[i] = l[i];
    }
    if (!convolve_stereo(mono.data(), output.channel_ptr(0), output.channel_ptr(1), n))
        output.clear();
}

void GpuMultiConvolver::process_cpu_fallback(const audio::BufferView<const float>&,
                                             audio::BufferView<float>& output,
                                             uint32_t) noexcept {
    // No real-time CPU fallback for the multi-room mode by design — output
    // silence on a miss rather than a misleading partial result.
    output.clear();
}

} // namespace pulp::gpu_audio

#pragma once

// Glue between the CPU-side NAM loader/oracle (nam_model.hpp) and the generic
// GPU NAM primitive (pulp::render::GpuCompute::prepare_nam / nam_forward). It
// translates a parsed NamModel's per-array config into NamLayerArraySpec values
// and hands the GPU the SAME flat weight blob the oracle runs — so the GPU
// forward reproduces the CPU forward exactly (validated by cross-correlation in
// the tests). The core primitive stays model-agnostic; this header owns the
// NAM-specific translation.

#include "nam_model.hpp"

#include <pulp/render/gpu_compute.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::examples::nam {

class GpuNam {
public:
    // Create a standalone GPU device and prepare from a loaded model. Returns
    // false if no GPU is available or the model shape is unsupported on the GPU.
    bool prepare(const NamModel& model, std::uint32_t block_size) {
        owned_gpu_ = render::GpuCompute::create();
        if (!owned_gpu_ || !owned_gpu_->initialize_standalone()) return false;
        return prepare_with(*owned_gpu_, model, block_size);
    }

    // Prepare against a caller-owned GpuCompute (e.g. one sharing a device with
    // a render surface). The caller keeps ownership of `gpu`.
    bool prepare_with(render::GpuCompute& gpu, const NamModel& model,
                      std::uint32_t block_size) {
        const std::vector<LayerArrayConfig>& cfg = model.arrays();
        if (cfg.empty()) return false;

        dilations_.assign(cfg.size(), {});
        specs_.assign(cfg.size(), {});
        for (std::size_t i = 0; i < cfg.size(); ++i) {
            // The GPU primitive implements NAM's default Tanh path (and the
            // sigmoid gate); other activations would need extra shader variants.
            if (cfg[i].activation != "Tanh") return false;
            dilations_[i].assign(cfg[i].dilations.begin(), cfg[i].dilations.end());

            render::GpuCompute::NamLayerArraySpec s;
            s.input_size = static_cast<std::uint32_t>(cfg[i].input_size);
            s.condition_size = static_cast<std::uint32_t>(cfg[i].condition_size);
            s.channels = static_cast<std::uint32_t>(cfg[i].channels);
            s.kernel = static_cast<std::uint32_t>(cfg[i].kernel_size);
            s.head_size = static_cast<std::uint32_t>(cfg[i].head_size);
            s.gated = cfg[i].gated ? 1u : 0u;
            s.head_bias = cfg[i].head_bias ? 1u : 0u;
            s.dilations = dilations_[i].data();
            s.num_layers = static_cast<std::uint32_t>(dilations_[i].size());
            specs_[i] = s;
        }

        gpu_ = &gpu;
        block_size_ = block_size;
        return gpu.prepare_nam(specs_.data(), static_cast<std::uint32_t>(specs_.size()),
                               model.weights_data(),
                               static_cast<std::uint32_t>(model.weights_size()),
                               block_size, model.head_scale());
    }

    // Run one mono block through the GPU forward (streaming-continuous across
    // calls). `n` must equal the prepared block size.
    bool forward(const float* in, float* out, std::uint32_t n) {
        return gpu_ != nullptr && gpu_->nam_forward(in, out, n);
    }

    std::uint32_t block_size() const { return block_size_; }
    render::GpuCompute* gpu() const { return gpu_; }

private:
    std::unique_ptr<render::GpuCompute> owned_gpu_;
    render::GpuCompute* gpu_ = nullptr;     // owned_gpu_ or a caller-provided device
    std::vector<render::GpuCompute::NamLayerArraySpec> specs_;
    std::vector<std::vector<std::uint32_t>> dilations_;  // stable storage for spec ptrs
    std::uint32_t block_size_ = 0;
};

}  // namespace pulp::examples::nam

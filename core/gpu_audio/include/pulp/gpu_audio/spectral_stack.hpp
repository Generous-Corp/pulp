#pragma once

#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <pulp/render/gpu_compute.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/windowing.hpp>

namespace pulp::gpu_audio {

/// A stack of N captured spectral "moments" (layers) that can be sustained and
/// morphed. capture() freezes the windowed spectrum of one frame into a layer;
/// render() advances every layer's phase, optionally smears (magnitude blur)
/// and jitters each layer, weights the layers, and synthesizes ONE combined
/// real frame. This is the authoring surface shared by the CPU reference
/// (CpuSpectralStack) and the GPU-resident batched engine (GpuSpectralStack) so
/// the framing/host code is identical regardless of where the per-bin work runs.
class SpectralStack {
public:
    virtual ~SpectralStack() = default;

    virtual uint32_t fft_size() const = 0;
    virtual uint32_t num_layers() const = 0;

    /// True if this stack's compute backend is live. The CPU stack is always
    /// available; the GPU stack is available only with a GPU device.
    virtual bool available() const = 0;

    /// Analyze `frame_in` (fft_size real samples) with the analysis window and
    /// freeze its magnitude + phase into `layer`, activating it.
    virtual bool capture(uint32_t layer, const float* frame_in) = 0;

    /// Deactivate a layer (its magnitude goes silent; phase keeps advancing).
    virtual void clear(uint32_t layer) = 0;
    virtual bool layer_active(uint32_t layer) const = 0;

    /// Advance + smear + weighted-sum the stack into `frame_out` (fft_size real
    /// samples). `weights` (length num_layers) scales each layer; `smear` (0..1)
    /// blurs magnitude across frequency; `jitter` (0..1) adds phase wander.
    /// Returns false if no layer is active or the backend is unavailable.
    virtual bool render(float* frame_out, const float* weights, float smear,
                        float jitter) = 0;
};

/// CPU reference + always-available fallback. Holds each layer's magnitude and
/// a persistent per-bin phase; render() advances the phase, smears, weighted-sums
/// every active layer and does ONE inverse FFT. This is the bit-for-bit semantic
/// the GPU stack reproduces — and the engine used when there's no GPU device.
class CpuSpectralStack : public SpectralStack {
public:
    bool prepare(uint32_t fft_size, uint32_t hop, uint32_t num_layers);

    uint32_t fft_size() const override { return n_; }
    uint32_t num_layers() const override {
        return static_cast<uint32_t>(layers_.size());
    }
    bool available() const override { return n_ > 0; }
    bool capture(uint32_t layer, const float* frame_in) override;
    void clear(uint32_t layer) override;
    bool layer_active(uint32_t layer) const override;
    bool render(float* frame_out, const float* weights, float smear,
                float jitter) override;

private:
    struct Layer {
        std::vector<float> mag;    // n
        std::vector<float> phase;  // n, persists + advances across renders
        bool active = false;
    };
    uint32_t n_ = 0;
    uint32_t hop_ = 0;
    float hop_ratio_ = 0.0f;          // hop / n: per-bin phase advance factor
    std::vector<Layer> layers_;
    std::unique_ptr<signal::Fft> fft_;
    std::vector<std::complex<float>> scratch_;  // n: capture + combine workspace
    std::vector<float> weights_;     // local copy (null weights => all 1)
    uint32_t seed_ = 0x9E3779B9u;    // jitter RNG seed, advanced each render
};

/// GPU-resident engine: the captured layer spectra live on the GPU and every hop
/// runs advance + smear + weighted-sum + one inverse FFT on the device, reading
/// back only the single synthesized frame. Wins over the CPU at high layer counts
/// (the per-bin smear + the N-layer reduce parallelize). available() is true only
/// with a GPU device; capture's forward FFT is done on the CPU (cheap, once per
/// freeze). Not real-time-safe (blocks on the readback) — drive it off the audio
/// thread. May share a device with a sibling primitive (e.g. GpuStft).
class GpuSpectralStack : public SpectralStack {
public:
    /// `shared_device` (optional) reuses an existing GpuCompute so the stack and
    /// a sibling don't each spin up a device; null = create + own one.
    bool prepare(uint32_t fft_size, uint32_t hop, uint32_t num_layers,
                 render::GpuCompute* shared_device = nullptr);

    uint32_t fft_size() const override { return n_; }
    uint32_t num_layers() const override { return num_layers_; }
    bool available() const override { return gpu_ != nullptr; }
    bool capture(uint32_t layer, const float* frame_in) override;
    void clear(uint32_t layer) override;
    bool layer_active(uint32_t layer) const override;
    bool render(float* frame_out, const float* weights, float smear,
                float jitter) override;

    /// GPU backend name ("Metal"/"D3D12"/"Vulkan"), or "" if no device.
    std::string backend() const;

private:
    uint32_t n_ = 0;
    uint32_t hop_ = 0;
    uint32_t num_layers_ = 0;
    render::GpuCompute* gpu_ = nullptr;                 // borrowed or == owned_
    std::unique_ptr<render::GpuCompute> owned_;         // when not shared
    std::unique_ptr<signal::Fft> fft_;                  // CPU forward for capture
    std::vector<std::complex<float>> cap_scratch_;      // n
    std::vector<float> mag_up_, phase_up_;              // n upload scratch
    std::vector<std::uint8_t> active_;                  // per layer
    std::vector<float> weights_;                        // num_layers (null => 1)
    uint32_t seed_ = 0x9E3779B9u;                       // jitter seed, advanced
};

/// Per-hop control values handed to the framer (read on whatever thread drives
/// the framer — the audio thread for the CPU engine, the GPU worker for the GPU
/// engine).
struct SpectralFreezeControls {
    const float* weights = nullptr;  // length num_layers (null = all 1)
    float smear = 0.0f;
    float jitter = 0.0f;
    bool freeze = false;             // capture-trigger LEVEL (rising edge captures)
    bool active = true;              // false = emit silence (stack not primed)
};

/// STFT freeze framer: turns an arbitrary-length input stream into hop-spaced
/// analysis frames, drives a SpectralStack (capture on a Freeze rising edge,
/// render every hop), and overlap-adds the synthesized frames back into an
/// arbitrary-length output stream. Owns no GPU state itself — the SpectralStack
/// does — so it runs unchanged inline (CPU engine) or on the transport worker
/// (GPU engine).
class SpectralFreezeFramer {
public:
    /// `stack` must outlive the framer and already be prepared with a matching
    /// fft_size. hop must divide evenly into the overlap (hop <= fft_size).
    bool prepare(SpectralStack* stack, uint32_t fft_size, uint32_t hop,
                 signal::WindowFunction::Type window = signal::WindowFunction::Type::hann);

    /// Process `n` input samples → `n` output samples (mono). RT-safety depends
    /// on the stack: the CPU stack is heavy-but-bounded; the GPU stack blocks on
    /// readback, so the GPU path must run off the audio thread.
    void process(const float* in, float* out, uint32_t n,
                 const SpectralFreezeControls& ctl);

    uint32_t fft_size() const { return fft_size_; }
    uint32_t hop() const { return hop_; }
    uint32_t captured_layers() const { return captured_; }

    void reset();

private:
    void run_hop(const SpectralFreezeControls& ctl);

    SpectralStack* stack_ = nullptr;
    uint32_t fft_size_ = 0;
    uint32_t hop_ = 0;
    std::vector<float> window_;     // analysis/synthesis window (hann)
    float ola_norm_ = 1.0f;         // steady-state window-overlap normalization

    std::vector<float> in_hist_;    // ring of the latest fft_size input samples
    uint32_t in_wr_ = 0;            // ring write index (oldest sample sits here)
    uint32_t in_fill_ = 0;          // samples accumulated toward the next hop
    std::vector<float> frame_;      // analysis frame scratch (fft_size)
    std::vector<float> rendered_;   // stack render output (fft_size)
    std::vector<float> ola_;        // overlap-add accumulator (fft_size)
    std::vector<float> out_fifo_;   // produced output waiting to be emitted
    uint32_t out_head_ = 0, out_tail_ = 0;

    bool prev_freeze_ = false;
    uint32_t next_layer_ = 0;       // round-robin capture target
    uint32_t captured_ = 0;
};

} // namespace pulp::gpu_audio

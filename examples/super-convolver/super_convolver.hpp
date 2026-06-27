#pragma once

// SuperConvolver — a convolution reverb / impulse processor.
//
// The live audio path is the RT-safe CPU convolution engine
// (signal::PartitionedConvolver, zero-latency, deterministic): it works and is
// harness-validatable today. Runtime IR changes (the Size knob) are rebuilt on
// a background worker and handed to the audio thread through the lock-free
// signal::ConvolverIrSwapper — process() never allocates or runs an FFT plan.
//
// The GPU convolution engine (render::GpuCompute fused/batched convolution via
// gpu_audio::GpuConvolver + GpuAudioTransport) is built and golden-validated
// separately as the accelerator for very long IRs / many instances, and feeds
// the GPU front-end (live IR waveform + spectrogram).

#include <pulp/format/processor.hpp>
#include <pulp/signal/convolver.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace pulp::examples {

enum SuperConvolverParams : state::ParamID {
    kMix     = 1,  // dry/wet, %
    kSize    = 2,  // IR length, seconds
    kGain    = 3,  // output gain, dB
    kBypass  = 4,
};

/// Build a deterministic, plausible reverb IR: exponentially-decaying white
/// noise (seeded LCG → reproducible, so a golden test can rebuild it). length
/// samples. The first sample is 1 so a delta IR-ish onset is present.
inline std::vector<float> make_reverb_ir(std::size_t length, std::uint32_t seed = 0x51C04711u) {
    std::vector<float> ir(length, 0.0f);
    if (length == 0) return ir;
    std::uint32_t s = seed;
    const float decay = 6.0f / static_cast<float>(length);  // ~ -52 dB over the tail
    for (std::size_t i = 0; i < length; ++i) {
        s = s * 1664525u + 1013904223u;
        const float white = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;  // [-1,1)
        ir[i] = white * std::exp(-decay * static_cast<float>(i));
    }
    ir[0] = 1.0f;  // direct onset
    return ir;
}

class SuperConvolverProcessor : public format::Processor {
public:
    ~SuperConvolverProcessor() override { stop_worker(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "SuperConvolver",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.superconvolver",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kMix,  .name = "Mix",  .unit = "%",
                             .range = {0.0f, 100.0f, 35.0f, 0.1f}});
        store.add_parameter({.id = kSize, .name = "Size", .unit = "s",
                             .range = {0.05f, 4.0f, 1.5f, 0.0f}});
        store.add_parameter({.id = kGain, .name = "Gain", .unit = "dB",
                             .range = {-24.0f, 24.0f, 0.0f, 0.0f}});
        store.add_parameter({.id = kBypass, .name = "Bypass", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        stop_worker();
        sample_rate_ = ctx.sample_rate;
        block_size_ = ctx.max_buffer_size > 0 ? static_cast<std::size_t>(ctx.max_buffer_size) : 512;
        wet_.assign(block_size_, 0.0f);            // pre-allocate the RT scratch
        rebuild_ir_inline(current_size());          // first IR loaded synchronously
        start_worker();
    }

    void release() override {
        stop_worker();
        for (auto& c : conv_) c.reset();
    }

    /// A snapshot of the current impulse response for the GPU UI waveform /
    /// spectrogram. UI-thread only — never call from the audio thread.
    std::vector<float> impulse_response_snapshot() const {
        std::lock_guard<std::mutex> lock(ir_display_mutex_);
        return ir_display_;
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const std::size_t n = output.num_samples();
        const std::size_t ch_count = output.num_channels();

        // Block-boundary IR handoff: pick up anything the worker staged. RT-safe
        // (two atomic pointer ops, no alloc, no free).
        for (std::size_t ch = 0; ch < conv_.size(); ++ch)
            conv_[ch].try_swap_ir(swapper_[ch]);

        // Tell the worker the currently-requested IR length (atomic store).
        requested_size_.store(current_size(), std::memory_order_relaxed);

        if (state().get_value(kBypass) >= 0.5f) {
            for (std::size_t ch = 0; ch < ch_count && ch < input.num_channels(); ++ch) {
                const auto in = input.channel(ch);
                auto out = output.channel(ch);
                for (std::size_t i = 0; i < n; ++i) out[i] = in[i];
            }
            return;
        }

        // Fully RT-safe: read params (atomics), convolve into pre-allocated
        // scratch, mix. No allocation / FFT re-plan on the audio thread.
        const float mix = state().get_value(kMix) / 100.0f;
        const float gain = std::pow(10.0f, state().get_value(kGain) / 20.0f);
        const std::size_t cap = wet_.size();

        for (std::size_t ch = 0; ch < ch_count && ch < input.num_channels() && ch < conv_.size(); ++ch) {
            const float* in = input.channel(ch).data();
            float* out = output.channel(ch).data();
            const std::size_t m = n < cap ? n : cap;
            conv_[ch].process(in, wet_.data(), m);  // RT-safe, zero-latency
            for (std::size_t i = 0; i < m; ++i)
                out[i] = (1.0f - mix) * in[i] + mix * gain * wet_[i];
            for (std::size_t i = m; i < n; ++i) out[i] = in[i];  // (n>cap: pass dry)
        }
    }

private:
    float current_size() const {
        const float s = state().get_value(kSize);
        return s > 0.05f ? s : 0.05f;
    }

    std::size_t ir_length_for(float seconds) const {
        std::size_t len = static_cast<std::size_t>(seconds * sample_rate_);
        return len < 1 ? 1 : len;
    }

    // prepare-time: build the IR and load it into both channels synchronously.
    void rebuild_ir_inline(float seconds) {
        auto ir = make_reverb_ir(ir_length_for(seconds));
        for (auto& c : conv_) c.load_ir(ir.data(), ir.size(), block_size_);
        publish_display_ir(ir);
        worker_built_size_ = seconds;
        requested_size_.store(seconds, std::memory_order_relaxed);
    }

    void publish_display_ir(const std::vector<float>& ir) {
        std::lock_guard<std::mutex> lock(ir_display_mutex_);
        ir_display_ = ir;
    }

    void start_worker() {
        worker_run_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { worker_loop(); });
    }

    void stop_worker() {
        worker_run_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    // Background thread: rebuild + stage the IR whenever Size changes, and
    // reclaim displaced IRs. Never touches the audio thread's buffers.
    void worker_loop() {
        using namespace std::chrono_literals;
        while (worker_run_.load(std::memory_order_acquire)) {
            const float want = requested_size_.load(std::memory_order_relaxed);
            if (want != worker_built_size_ && want > 0.0f) {
                auto ir = make_reverb_ir(ir_length_for(want));
                for (auto& sw : swapper_)
                    sw.stage_ir(ir.data(), ir.size(), block_size_);
                publish_display_ir(ir);
                worker_built_size_ = want;
            }
            for (auto& sw : swapper_) sw.drain_old();
            std::this_thread::sleep_for(5ms);
        }
        // Final drain so nothing leaks once the audio thread has stopped.
        for (auto& sw : swapper_) sw.drain_old();
    }

    double sample_rate_ = 48000.0;
    std::size_t block_size_ = 512;
    std::vector<float> wet_;
    std::array<signal::PartitionedConvolver, 2> conv_{};
    std::array<signal::ConvolverIrSwapper, 2> swapper_{};

    // Worker / live-IR-swap state.
    std::thread worker_;
    std::atomic<bool> worker_run_{false};
    std::atomic<float> requested_size_{-1.0f};
    float worker_built_size_ = -1.0f;  // worker-thread-local

    // UI display snapshot (UI + worker thread only; never audio thread).
    mutable std::mutex ir_display_mutex_;
    std::vector<float> ir_display_;
};

inline std::unique_ptr<format::Processor> create_super_convolver() {
    return std::make_unique<SuperConvolverProcessor>();
}

} // namespace pulp::examples

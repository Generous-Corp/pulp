#pragma once

// GPU NAM — a Neural Amp Modeler player that runs open-source `.nam` WaveNet
// captures through Pulp's GPU audio runtime.
//
// The live audio path defaults to the RT-safe inline CPU engine: the exact NAM
// WaveNet oracle (nam_model.hpp), applied per channel sample-by-sample. The input
// is re-blocked into fixed kInternalBlock chunks so the inline CPU engine and the
// GPU engine share one re-blocking FIFO and one fixed reported latency.
//
// An optional, default-OFF GPU engine (the Engine knob) routes the same fixed
// blocks through the real GPU audio runtime: a GpuNamCloudNode (one fused GPU
// `nam_forward` per channel) driven by gpu_audio::GpuAudioTransport on a non-RT
// worker. The GPU forward blocks on the device readback, so it runs only on the
// transport worker; the audio thread calls the lock-free transport.process() and,
// on a worker miss, the node's CpuFallback runs the exact CPU oracle. If no GPU
// device exists the processor stays on the inline CPU engine and always works.
//
// Engine (CPU<->GPU) and the loaded model are switchable LIVE without a reload:
// a background worker builds the requested engine stack off-thread and publishes
// it through atomic pointers (gpu_active_ for the GPU transport, cpu_active_ for
// the inline CPU engine) that the audio thread loads each block. The previously
// active stack is retired and freed one rebuild later, so the audio thread never
// holds a stack as it is freed — it never allocates or frees. Reported latency is
// FIXED for the prepared lifetime (kInternalBlock plus the GPU transport's delay
// when a device exists), applied to both engines so a live switch keeps the
// host's PDC correct and dry/wet phase-aligned. See gpu_engine_active().
//
// The native GPU front-end (input→output transfer curve + output spectrum +
// gain/mix/engine controls, rendered through canvas/Skia/Dawn) is in gpu_nam_ui.hpp.

#include "gpu_nam.hpp"
#include "gpu_nam_cloud_node.hpp"
#include "nam_model.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/signal/biquad.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/noise_gate.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

#ifndef GPU_NAM_DEFAULT_MODEL_PATH
#define GPU_NAM_DEFAULT_MODEL_PATH ""
#endif

namespace pulp::view { class View; }

namespace pulp::examples {

enum GpuNamParams : state::ParamID {
    kInputGain  = 1,  // drive into the model, dB
    kOutputGain = 2,  // output gain, dB
    kMix        = 3,  // dry/wet, % — retained for state compat; face UI is 100% wet
    kEngine     = 4,  // 0 = CPU (default), 1 = GPU
    kBypass     = 5,
    // NAM-faithful control set. The noise gate runs on the drive before the
    // model; the Bass/Middle/Treble tone stack runs on the model output.
    kNoiseGateThreshold = 6,  // gate threshold, dB
    kToneBass           = 7,  // low shelf, 0..10 (5 = flat)
    kToneMiddle         = 8,  // mid peak,  0..10 (5 = flat)
    kToneTreble         = 9,  // high shelf,0..10 (5 = flat)
    kNoiseGateActive    = 10, // 0 = off, 1 = on
    kEQActive           = 11, // 0 = off (tone stack bypassed), 1 = on
};

// Live output magnitude spectrum (dB), published lock-free from the audio thread
// to the GPU UI's frequency display. 256 log-ready bins.
inline constexpr int kSpectrumBins = 256;
using SpectrumFrame = std::array<float, kSpectrumBins>;
using SpectrumBus = pulp::runtime::TripleBuffer<SpectrumFrame>;

// Static input→output transfer ("amp character") curve. kCurvePoints output
// levels for inputs swept linearly across [-kCurveRange, +kCurveRange].
inline constexpr int kCurvePoints = 97;
inline constexpr float kCurveRange = 1.0f;
using TransferCurve = std::array<float, kCurvePoints>;

// Directory of the running module (executable or plugin dylib), for locating the
// bundled default model relative to the binary. Empty when unavailable.
inline std::string gpu_nam_module_dir() {
#if defined(__APPLE__)
    Dl_info info;
    if (dladdr(reinterpret_cast<const void*>(&gpu_nam_module_dir), &info) && info.dli_fname) {
        const std::string p = info.dli_fname;
        const auto slash = p.find_last_of('/');
        if (slash != std::string::npos) return p.substr(0, slash);
    }
#endif
    return {};
}

// Resolve the default `.nam` model: an explicit GPU_NAM_MODEL override, then the
// copy bundled into the plugin's Resources (relative to the binary), then the
// source-tree copy baked in at build time. Returns the first that exists.
inline std::string gpu_nam_default_model_path() {
    auto exists = [](const std::string& p) {
        if (p.empty()) return false;
        std::ifstream f(p, std::ios::binary);
        return static_cast<bool>(f);
    };
    if (const char* env = std::getenv("GPU_NAM_MODEL"); env && exists(env)) return env;
    const std::string dir = gpu_nam_module_dir();
    if (!dir.empty()) {
        // macOS bundle: Contents/MacOS/<bin> → Contents/Resources/example.nam.
        for (const std::string& rel : {"/../Resources/example.nam", "/example.nam"}) {
            const std::string p = dir + rel;
            if (exists(p)) return p;
        }
    }
    if (exists(GPU_NAM_DEFAULT_MODEL_PATH)) return GPU_NAM_DEFAULT_MODEL_PATH;
    return GPU_NAM_DEFAULT_MODEL_PATH;
}

// Filename (no directory) of a path, for display.
inline std::string gpu_nam_basename(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

#ifndef GPU_NAM_ASSET_DIR
#define GPU_NAM_ASSET_DIR ""
#endif

// Resolve the vendored NAM UI-asset directory (images + fonts): an explicit
// GPU_NAM_ASSETS override, then the copy bundled into the plugin's Resources
// (relative to the binary), then the source-tree copy baked in at build time.
// Returns the first directory that exists; empty when none is found.
inline std::string gpu_nam_asset_dir() {
    auto dir_ok = [](const std::string& p) {
        if (p.empty()) return false;
        std::ifstream f(p + "/Background.jpg", std::ios::binary);
        return static_cast<bool>(f);
    };
    if (const char* env = std::getenv("GPU_NAM_ASSETS"); env && dir_ok(env)) return env;
    const std::string dir = gpu_nam_module_dir();
    if (!dir.empty()) {
        for (const std::string& rel : {"/../Resources/assets/nam", "/assets/nam"}) {
            const std::string p = dir + rel;
            if (dir_ok(p)) return p;
        }
    }
    if (dir_ok(GPU_NAM_ASSET_DIR)) return GPU_NAM_ASSET_DIR;
    return {};
}

class GpuNamProcessor : public format::Processor {
public:
    // Fixed engine block. The GPU NAM forward is prepared at this size, so an
    // internal re-blocking FIFO chunks the host stream (variable, often smaller)
    // into kInternalBlock blocks for both engines. 512 samples ≈ 10.7 ms at
    // 48 kHz; the re-block FIFO adds this much latency, reported for host PDC.
    static constexpr std::size_t kInternalBlock = 512;

    ~GpuNamProcessor() override { stop_worker(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "GPU NAM",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.gpunam",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        // Ranges mirror NeuralAmpModelerPlugin's face controls.
        store.add_parameter({.id = kInputGain,  .name = "Input",  .unit = "dB",
                             .range = {-20.0f, 20.0f, 0.0f, 0.0f}});
        store.add_parameter({.id = kOutputGain, .name = "Output", .unit = "dB",
                             .range = {-40.0f, 40.0f, 0.0f, 0.0f}});
        store.add_parameter({.id = kMix, .name = "Mix", .unit = "%",
                             .range = {0.0f, 100.0f, 100.0f, 0.1f}});
        // Engine: 0 = inline CPU oracle (default), 1 = GPU runtime. CPU is the
        // default — the live GPU path is opt-in.
        store.add_parameter({.id = kEngine, .name = "Engine", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kBypass, .name = "Bypass", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kNoiseGateThreshold, .name = "Gate", .unit = "dB",
                             .range = {-100.0f, 0.0f, -80.0f, 0.0f}});
        store.add_parameter({.id = kToneBass,   .name = "Bass",   .unit = "",
                             .range = {0.0f, 10.0f, 5.0f, 0.0f}});
        store.add_parameter({.id = kToneMiddle, .name = "Middle", .unit = "",
                             .range = {0.0f, 10.0f, 5.0f, 0.0f}});
        store.add_parameter({.id = kToneTreble, .name = "Treble", .unit = "",
                             .range = {0.0f, 10.0f, 5.0f, 0.0f}});
        store.add_parameter({.id = kNoiseGateActive, .name = "Noise Gate", .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 1.0f}});
        store.add_parameter({.id = kEQActive, .name = "EQ", .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 1.0f}});
    }

    // Total latency, FIXED at prepare() for the prepared lifetime: the re-block
    // FIFO (kInternalBlock) plus, whenever a GPU device exists, the GPU
    // transport's fixed delay — reported for BOTH engines so a live Engine switch
    // never moves the reported latency (the host's PDC stays correct and dry/wet
    // stay phase-aligned). When no GPU device exists the latency is kInternalBlock.
    int latency_samples() const override { return latency_samples_; }

    /// Set the .nam model loaded for a fresh path at runtime. Thread-safe; the
    /// background worker rebuilds both engines off the audio thread (no audio-
    /// thread alloc/free). Returns immediately; the swap lands a few worker ticks
    /// later. Pass an empty path to (re)load the default model.
    void load_model(const std::string& path) {
        std::lock_guard<std::mutex> lock(model_req_mutex_);
        requested_model_path_ = path.empty() ? gpu_nam_default_model_path() : path;
        // A non-empty path is a real user selection; empty resets to the bundled
        // default. The file-slot UI shows the prompt until a user picks a model.
        user_model_loaded_.store(!path.empty(), std::memory_order_release);
        model_req_generation_.fetch_add(1, std::memory_order_release);
    }

    /// True once a user has selected a model via load_model(); false for the
    /// bundled default. UI-thread only. Drives the file-slot prompt vs name.
    bool user_model_loaded() const {
        return user_model_loaded_.load(std::memory_order_acquire);
    }

    /// True when the live audio path is actually the GPU engine (Engine=GPU is
    /// requested AND a GPU device is available AND the transport is published).
    bool gpu_engine_active() const {
        return gpu_engine_active_.load(std::memory_order_acquire);
    }

    /// The live GPU backend ("Metal"/"D3D12"/"Vulkan") when the GPU engine is
    /// active, else "". UI/main-thread only (takes the worker mutex).
    std::string gpu_backend() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        if (!gpu_engine_active() || !current_stack_ || !current_stack_->node)
            return std::string();
        return current_stack_->node->backend();
    }

    /// Live {GPU blocks produced, blocks missed (CPU-filled)}. UI/main-thread only.
    std::pair<std::uint64_t, std::uint64_t> gpu_block_stats() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        if (!gpu_engine_active() || !current_stack_ || !current_stack_->transport)
            return {0, 0};
        const auto s = current_stack_->transport->stats();
        return {s.produced_blocks, s.miss_blocks};
    }

    /// Live GPU cost: {last, average} wall-clock microseconds per block.
    std::pair<double, double> gpu_block_us() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        if (!gpu_engine_active() || !current_stack_ || !current_stack_->transport)
            return {0.0, 0.0};
        const auto s = current_stack_->transport->stats();
        return {s.last_block_us, s.avg_block_us};
    }

    /// One coherent snapshot of the live GPU engine for the UI status line, taken
    /// under a SINGLE lock so the fields can't disagree across a repaint.
    /// `budget_us` is one GPU block's real-time budget on THIS device + sample
    /// rate; `rt_percent` is the measured average cost as a percentage of it.
    struct GpuStatus {
        bool active = false;
        std::string backend;
        std::uint64_t blocks = 0;
        std::uint64_t misses = 0;
        double avg_us = 0.0;
        double budget_us = 0.0;
        double rt_percent = 0.0;
    };
    GpuStatus gpu_status() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        GpuStatus g;
        g.active = gpu_engine_active();
        if (!g.active || !current_stack_) return g;
        if (current_stack_->node) g.backend = current_stack_->node->backend();
        if (current_stack_->transport) {
            const auto s = current_stack_->transport->stats();
            g.blocks = s.produced_blocks;
            g.misses = s.miss_blocks;
            g.avg_us = s.avg_block_us;
        }
        if (sample_rate_ > 0.0) {
            g.budget_us = static_cast<double>(kInternalBlock) / sample_rate_ * 1e6;
            if (g.budget_us > 0.0) g.rt_percent = g.avg_us / g.budget_us * 100.0;
        }
        return g;
    }

    /// Display name of the loaded model (its filename). UI/main-thread only.
    std::string model_name() const {
        std::lock_guard<std::mutex> lock(model_mutex_);
        return model_name_;
    }

    /// Snapshot of the input→output transfer ("amp character") curve. UI-thread
    /// only — never the audio thread.
    TransferCurve transfer_curve_snapshot() const {
        std::lock_guard<std::mutex> lock(model_mutex_);
        return transfer_curve_;
    }
    /// Monotonic counter bumped each time the model (and its curve) changes.
    std::uint32_t model_generation() const {
        return model_generation_.load(std::memory_order_relaxed);
    }

    /// Lock-free latest output spectrum for the UI (UI is sole reader).
    SpectrumBus& spectrum_bus() { return spectrum_bus_; }

    /// Live {input, output} meter levels in dBFS (peak with fast-attack/
    /// slow-release ballistics). Published from the audio thread; UI reads.
    /// −120 dB is the noise floor / silence sentinel.
    std::pair<float, float> meter_levels_db() const {
        return {in_level_db_.load(std::memory_order_relaxed),
                out_level_db_.load(std::memory_order_relaxed)};
    }

    /// Native GPU front-end.
    std::unique_ptr<view::View> create_view() override;

    format::ViewSize view_size() const override {
        // NeuralAmpModelerPlugin's window aspect (its Background art is 600×400).
        return format::view_size_from_design(600, 400);
    }

    void prepare(const format::PrepareContext& ctx) override {
        stop_worker();
        gpu_active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        cpu_active_.store(nullptr, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            current_stack_.reset();
        }
        retired_stack_.reset();
        current_cpu_.reset();
        retired_cpu_.reset();

        sample_rate_ = ctx.sample_rate;
        const std::size_t max_block =
            ctx.max_buffer_size > 0 ? static_cast<std::size_t>(ctx.max_buffer_size) : 512;

        const std::size_t cap = max_block + 4 * kInternalBlock;
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            in_buf_[ch].assign(cap, 0.0f);   in_len_[ch] = 0;
            out_buf_[ch].assign(cap, 0.0f);  out_len_[ch] = kInternalBlock;  // primed: B zeros
            gpu_wet_[ch].assign(kInternalBlock, 0.0f);
        }
        wet_.assign(kInternalBlock, 0.0f);

        // Noise gate + tone-stack DSP (RT-safe; no alloc after this). Reset filter
        // state and invalidate the cached coefficients so process() retunes on its
        // first block from the live parameter values.
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            gate_[ch].set_sample_rate(static_cast<float>(sample_rate_));
            gate_[ch].reset();
            for (auto& b : tone_[ch]) b.reset();
        }
        cached_gate_threshold_ = std::nanf("");
        cached_bass_ = cached_middle_ = cached_treble_ = std::nanf("");

        // Resolve + load the model synchronously so the first block is correct.
        const std::string path = current_requested_path();
        nam::NamModel model;
        std::string err;
        const bool ok = nam::load_nam(path, model, &err);
        if (!ok) {
            runtime::log_error("GPU NAM: failed to load model '{}' ({}); passing dry.",
                               path, err);
        }
        loaded_ok_ = ok;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            model_ = model;
            model_name_ = ok ? gpu_nam_basename(path) : std::string("(no model)");
        }
        recompute_transfer_curve();

        // Inline CPU engine: per-channel copies of the model. Published for the
        // audio thread via cpu_active_ (built/freed only off the audio thread).
        {
            auto cpu = std::make_unique<CpuEngine>();
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                cpu->model[ch] = model;
                cpu->model[ch].reset();
            }
            current_cpu_ = std::move(cpu);
            cpu_active_.store(current_cpu_.get(), std::memory_order_release);
        }

        // Learn the GPU transport latency by pre-building the stack once when a
        // device exists. The stack stays built even when Engine=CPU; gpu_active_
        // is only published when Engine=GPU.
        gpu_extra_ = 0;
        device_available_ = false;
        if (ok) {
            auto stack = build_gpu_stack(model);
            if (stack) {
                device_available_ = true;
                gpu_extra_ = static_cast<std::size_t>(stack->transport->latency_samples());
                std::lock_guard<std::mutex> lock(stack_mutex_);
                current_stack_ = std::move(stack);
            }
        }

        latency_samples_ = static_cast<int>(kInternalBlock + gpu_extra_);
        const std::size_t dry_delay = static_cast<std::size_t>(latency_samples_);
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            dry_ring_[ch].assign(dry_delay, 0.0f);
            dry_pos_[ch] = 0;
            cpu_extra_ring_[ch].assign(gpu_extra_, 0.0f);
            cpu_extra_pos_[ch] = 0;
        }

        requested_engine_.store(state().get_value(kEngine) >= 0.5f ? 1 : 0,
                                std::memory_order_relaxed);
        if (requested_engine_.load(std::memory_order_relaxed) == 1) {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            if (current_stack_) {
                gpu_active_.store(current_stack_->transport.get(), std::memory_order_release);
                gpu_engine_active_.store(true, std::memory_order_release);
            }
        }

        start_worker();
    }

    void release() override {
        stop_worker();
        gpu_active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        cpu_active_.store(nullptr, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            current_stack_.reset();
        }
        retired_stack_.reset();
        current_cpu_.reset();
        retired_cpu_.reset();
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const std::size_t n = output.num_samples();
        const std::size_t ch_count = output.num_channels();

        const bool bypass = state().get_value(kBypass) >= 0.5f;
        const float mix = bypass ? 0.0f : state().get_value(kMix) / 100.0f;
        const float in_gain = std::pow(10.0f, state().get_value(kInputGain) / 20.0f);
        const float out_gain = std::pow(10.0f, state().get_value(kOutputGain) / 20.0f);

        requested_engine_.store(state().get_value(kEngine) >= 0.5f ? 1 : 0,
                                std::memory_order_relaxed);

        // Retune the gate + tone stack from the live parameters (only when they
        // change, at this block boundary — the RT-safe way to move IIR/gate coeffs).
        gate_active_ = state().get_value(kNoiseGateActive) >= 0.5f;
        eq_active_ = state().get_value(kEQActive) >= 0.5f;
        update_gate(state().get_value(kNoiseGateThreshold));
        update_tone(state().get_value(kToneBass), state().get_value(kToneMiddle),
                    state().get_value(kToneTreble));

        // Append the drive signal (input × input-gain, then noise gate) to the
        // re-block FIFO; the active engine transforms drive → wet (model output).
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            float* dst = in_buf_[ch].data() + in_len_[ch];
            const float* in = ch < input.num_channels() ? input.channel(ch).data() : nullptr;
            for (std::size_t i = 0; i < n; ++i) {
                float drive = in ? in[i] * in_gain : 0.0f;
                if (gate_active_) drive = gate_[ch].process(drive);
                dst[i] = drive;
            }
            in_len_[ch] += n;
        }

        gpu_audio::GpuAudioTransport* tp = gpu_active_.load(std::memory_order_acquire);
        if (tp) fill_wet_gpu(tp);
        else    fill_wet_cpu();

        // Emit n samples per channel: wet from the primed output FIFO, dry delayed
        // by the fixed total latency so they stay aligned under either engine.
        for (std::size_t ch = 0; ch < ch_count && ch < kChannels; ++ch) {
            const float* in = ch < input.num_channels() ? input.channel(ch).data() : nullptr;
            float* out = output.channel(ch).data();
            const std::size_t avail = out_len_[ch];
            const std::size_t delay = dry_ring_[ch].size();
            for (std::size_t i = 0; i < n; ++i) {
                const float wet_i = i < avail ? out_buf_[ch][i] : 0.0f;
                const float dry_i = delay > 0 ? dry_ring_[ch][dry_pos_[ch]] : (in ? in[i] : 0.0f);
                if (delay > 0) {
                    dry_ring_[ch][dry_pos_[ch]] = in ? in[i] : 0.0f;
                    dry_pos_[ch] = (dry_pos_[ch] + 1) % delay;
                }
                out[i] = (1.0f - mix) * dry_i + mix * out_gain * wet_i;
            }
            const std::size_t consumed = n < avail ? n : avail;
            std::memmove(out_buf_[ch].data(), out_buf_[ch].data() + consumed,
                         (out_len_[ch] - consumed) * sizeof(float));
            out_len_[ch] -= consumed;
        }

        publish_meters(input, output, n);
        if (ch_count > 0) publish_spectrum(output.channel(0).data(), static_cast<int>(n));
    }

private:
    static constexpr std::size_t kChannels = 2;

    // The inline CPU engine: one NAM oracle per channel. Heap-owned, built/freed
    // only off the audio thread; the audio thread loads a pointer to it.
    struct CpuEngine {
        std::array<nam::NamModel, kChannels> model{};
    };

    // One GPU engine: the node (per-channel GPU NAM forward + CPU fallback) plus
    // the transport. The stack owns its OWN model copy so the node's model pointer
    // stays valid across a live model reload (the processor's authoritative model_
    // is a separate copy). Member destruction is reverse-of-declaration: transport
    // (last) is destroyed before node — required, as the transport points into the
    // node.
    struct GpuStack {
        std::unique_ptr<nam::NamModel> model;
        std::unique_ptr<GpuNamCloudNode> node;
        std::unique_ptr<gpu_audio::GpuAudioTransport> transport;
    };

    // CPU engine: re-block the drive FIFO into fixed blocks, run the inline NAM
    // oracle per channel, delay by the GPU transport's fixed extra latency so the
    // two engines align, and push to the output FIFO. RT-safe (no alloc/free).
    void fill_wet_cpu() {
        CpuEngine* cpu = cpu_active_.load(std::memory_order_acquire);
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            while (in_len_[ch] >= kInternalBlock) {
                if (cpu) cpu->model[ch].process(in_buf_[ch].data(), wet_.data(),
                                                static_cast<std::uint32_t>(kInternalBlock));
                else std::memset(wet_.data(), 0, kInternalBlock * sizeof(float));
                std::memmove(in_buf_[ch].data(), in_buf_[ch].data() + kInternalBlock,
                             (in_len_[ch] - kInternalBlock) * sizeof(float));
                in_len_[ch] -= kInternalBlock;
                apply_tone(ch, wet_.data());
                delay_cpu_wet(ch);
                std::memcpy(out_buf_[ch].data() + out_len_[ch], wet_.data(),
                            kInternalBlock * sizeof(float));
                out_len_[ch] += kInternalBlock;
            }
        }
    }

    // Push one internal block of CPU wet through the per-channel extra-delay ring
    // (length gpu_extra_), in place. RT-safe: the ring is preallocated.
    void delay_cpu_wet(std::size_t ch) {
        const std::size_t d = gpu_extra_;
        if (d == 0) return;
        auto& ring = cpu_extra_ring_[ch];
        std::size_t& pos = cpu_extra_pos_[ch];
        for (std::size_t i = 0; i < kInternalBlock; ++i) {
            const float in = wet_[i];
            wet_[i] = ring[pos];
            ring[pos] = in;
            pos = (pos + 1) % d;
        }
    }

    // Retune the per-channel noise gate on a threshold change (block boundary).
    // Fast, alloc-free — safe to call every block; the cache skips the no-op case.
    void update_gate(float threshold_db) {
        if (threshold_db == cached_gate_threshold_) return;
        cached_gate_threshold_ = threshold_db;
        signal::NoiseGate::Params p;
        p.threshold_db = threshold_db;
        p.ratio = 10.0f;       // hard downward expander ≈ gate
        p.attack_ms = 1.0f;
        p.release_ms = 50.0f;
        p.range_db = -80.0f;
        for (std::size_t ch = 0; ch < kChannels; ++ch) gate_[ch].set_params(p);
    }

    // Retune the per-channel Bass/Middle/Treble tone stack. 0..10 maps linearly to
    // ±kToneRangeDb with 5 = flat: low shelf (Bass), mid peak (Middle), high shelf
    // (Treble). Only recomputes coefficients when a value actually changes.
    void update_tone(float bass, float middle, float treble) {
        if (bass == cached_bass_ && middle == cached_middle_ && treble == cached_treble_)
            return;
        cached_bass_ = bass;
        cached_middle_ = middle;
        cached_treble_ = treble;
        const float sr = static_cast<float>(sample_rate_);
        const float bass_db   = (bass   - 5.0f) / 5.0f * kToneRangeDb;
        const float mid_db    = (middle - 5.0f) / 5.0f * kToneRangeDb;
        const float treble_db = (treble - 5.0f) / 5.0f * kToneRangeDb;
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            tone_[ch][0].set_coefficients(signal::Biquad::Type::low_shelf, 120.0f, 0.707f, sr, bass_db);
            tone_[ch][1].set_coefficients(signal::Biquad::Type::peaking, 750.0f, 0.7f, sr, mid_db);
            tone_[ch][2].set_coefficients(signal::Biquad::Type::high_shelf, 3000.0f, 0.707f, sr, treble_db);
        }
    }

    // Apply one channel's tone stack in place over an internal block (RT-safe).
    // A no-op when EQ is off, so the model output passes through unfiltered.
    void apply_tone(std::size_t ch, float* buf) {
        if (!eq_active_) return;
        auto& t = tone_[ch];
        for (std::size_t i = 0; i < kInternalBlock; ++i)
            buf[i] = t[2].process(t[1].process(t[0].process(buf[i])));
    }

    // GPU engine: same fixed re-blocking, each B-block processed as ONE stereo
    // block through the GPU transport (RT-safe by contract — the GPU forward runs
    // on the transport's non-RT worker; a worker miss runs the node's CpuFallback).
    // Both channels advance in lockstep, so in_len_[0] gates the drain.
    void fill_wet_gpu(gpu_audio::GpuAudioTransport* tp) {
        while (in_len_[0] >= kInternalBlock && in_len_[1] >= kInternalBlock) {
            const float* in_ptrs[kChannels] = {in_buf_[0].data(), in_buf_[1].data()};
            float* out_ptrs[kChannels] = {gpu_wet_[0].data(), gpu_wet_[1].data()};
            audio::BufferView<const float> iv(in_ptrs, kChannels, kInternalBlock);
            audio::BufferView<float> ov(out_ptrs, kChannels, kInternalBlock);
            tp->process(iv, ov, static_cast<std::uint32_t>(kInternalBlock));
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                std::memmove(in_buf_[ch].data(), in_buf_[ch].data() + kInternalBlock,
                             (in_len_[ch] - kInternalBlock) * sizeof(float));
                in_len_[ch] -= kInternalBlock;
                apply_tone(ch, gpu_wet_[ch].data());
                std::memcpy(out_buf_[ch].data() + out_len_[ch], gpu_wet_[ch].data(),
                            kInternalBlock * sizeof(float));
                out_len_[ch] += kInternalBlock;
            }
        }
    }

    // Build a self-contained GPU stack (model copy + node + transport) off the
    // audio thread. Returns nullptr on any failure (no GPU device, unsupported
    // shape) — the caller then routes the CPU engine. Non-RT.
    std::unique_ptr<GpuStack> build_gpu_stack(const nam::NamModel& model) {
        auto stack = std::make_unique<GpuStack>();
        stack->model = std::make_unique<nam::NamModel>(model);
        stack->node = std::make_unique<GpuNamCloudNode>(
            static_cast<std::uint32_t>(kChannels),
            static_cast<std::uint32_t>(kInternalBlock),
            static_cast<std::uint32_t>(sample_rate_), stack->model.get());
        if (!stack->node->prepare() || !stack->node->gpu_available()) return nullptr;

        stack->transport = std::make_unique<gpu_audio::GpuAudioTransport>();
        gpu_audio::GpuAudioTransport::Config cfg;
        cfg.ring_blocks = 8;
        cfg.run_worker_thread = true;
        if (!stack->transport->prepare(stack->node.get(), cfg)) return nullptr;
        return stack;
    }

    // Worker-thread only. Build + publish a fresh GPU stack for `model`. The
    // current stack is RETIRED and freed on the NEXT rebuild — never freed while
    // the audio thread might still be loading its pointer.
    void rebuild_gpu_stack(const nam::NamModel& model) {
        retired_stack_.reset();
        gpu_active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            retired_stack_ = std::move(current_stack_);
        }
        auto fresh = build_gpu_stack(model);
        if (!fresh) {
            runtime::log_info("GPU NAM: GPU stack rebuild failed; routing the CPU engine.");
            return;
        }
        gpu_audio::GpuAudioTransport* tp = fresh->transport.get();
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            current_stack_ = std::move(fresh);
        }
        if (requested_engine_.load(std::memory_order_relaxed) == 1) {
            gpu_active_.store(tp, std::memory_order_release);
            gpu_engine_active_.store(true, std::memory_order_release);
        }
    }

    // Worker-thread only. Publish a fresh inline CPU engine for `model`, retiring
    // the old one (freed one rebuild later).
    void rebuild_cpu_engine(const nam::NamModel& model) {
        retired_cpu_.reset();
        retired_cpu_ = std::move(current_cpu_);
        cpu_active_.store(nullptr, std::memory_order_release);
        auto cpu = std::make_unique<CpuEngine>();
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            cpu->model[ch] = model;
            cpu->model[ch].reset();
        }
        current_cpu_ = std::move(cpu);
        cpu_active_.store(current_cpu_.get(), std::memory_order_release);
    }

    // Compute the static input→output transfer curve on a fresh model copy: for
    // each swept input level, settle a reset model on that constant and take the
    // last output. Publishes under model_mutex_ + bumps model_generation_. UI/
    // worker-thread only (never the audio thread).
    void recompute_transfer_curve() {
        nam::NamModel m;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            m = model_;
        }
        TransferCurve curve{};
        if (loaded_ok_ && m.weights_size() > 0) {
            constexpr int settle = 1024;
            for (int p = 0; p < kCurvePoints; ++p) {
                const float x = -kCurveRange + 2.0f * kCurveRange * p / (kCurvePoints - 1);
                m.reset();
                float y = 0.0f;
                for (int i = 0; i < settle; ++i) y = m.process_sample(x);
                curve[static_cast<std::size_t>(p)] = std::isfinite(y) ? y : 0.0f;
            }
        }
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            transfer_curve_ = curve;
        }
        model_generation_.fetch_add(1, std::memory_order_relaxed);
    }

    // Peak meter with fast-attack / slow-release ballistics, per block. Input is
    // the raw (pre-gain) signal; output is the post-mix result. Audio-thread only.
    void publish_meters(const audio::BufferView<const float>& input,
                        const audio::BufferView<float>& output, std::size_t n) {
        float in_peak = 0.0f, out_peak = 0.0f;
        for (std::size_t ch = 0; ch < input.num_channels(); ++ch) {
            const float* p = input.channel(ch).data();
            for (std::size_t i = 0; i < n; ++i) in_peak = std::max(in_peak, std::abs(p[i]));
        }
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            const float* p = output.channel(ch).data();
            for (std::size_t i = 0; i < n; ++i) out_peak = std::max(out_peak, std::abs(p[i]));
        }
        const float rel = 0.10f;  // slow release toward the new peak
        in_env_  = in_peak  > in_env_  ? in_peak  : in_env_  + rel * (in_peak  - in_env_);
        out_env_ = out_peak > out_env_ ? out_peak : out_env_ + rel * (out_peak - out_env_);
        auto to_db = [](float lin) {
            return lin > 1e-6f ? 20.0f * std::log10(lin) : -120.0f;
        };
        in_level_db_.store(to_db(in_env_), std::memory_order_relaxed);
        out_level_db_.store(to_db(out_env_), std::memory_order_relaxed);
    }

    void publish_spectrum(const float* mono, int n) {
        for (int i = 0; i < n; ++i) {
            spec_ring_[static_cast<std::size_t>(spec_pos_)] = mono[i];
            spec_pos_ = (spec_pos_ + 1) % kSpectrumFft;
        }
        for (int i = 0; i < kSpectrumFft; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / kSpectrumFft);
            spec_time_[static_cast<std::size_t>(i)] =
                spec_ring_[static_cast<std::size_t>((spec_pos_ + i) % kSpectrumFft)] * w;
        }
        spec_fft_.forward_real(spec_time_.data(), spec_freq_.data());
        SpectrumFrame frame;
        for (int k = 0; k < kSpectrumBins; ++k) {
            const float mag = std::abs(spec_freq_[static_cast<std::size_t>(k)]) / (kSpectrumFft * 0.25f);
            frame[static_cast<std::size_t>(k)] = 20.0f * std::log10(mag + 1e-7f);
        }
        spectrum_bus_.write(frame);
    }

    std::string current_requested_path() {
        std::lock_guard<std::mutex> lock(model_req_mutex_);
        if (requested_model_path_.empty())
            requested_model_path_ = gpu_nam_default_model_path();
        return requested_model_path_;
    }

    void start_worker() {
        worker_run_.store(true, std::memory_order_release);
        last_seen_model_gen_ = model_req_generation_.load(std::memory_order_acquire);
        worker_ = std::thread([this] { worker_loop(); });
    }

    void stop_worker() {
        worker_run_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    // Background thread: reconcile the published GPU path with the requested
    // Engine, and rebuild both engines on a model reload. Never touches the audio
    // thread's buffers or frees a stack the audio thread might still hold.
    void worker_loop() {
        using namespace std::chrono_literals;
        while (worker_run_.load(std::memory_order_acquire)) {
            // Model reload?
            const std::uint32_t gen = model_req_generation_.load(std::memory_order_acquire);
            if (gen != last_seen_model_gen_) {
                last_seen_model_gen_ = gen;
                const std::string path = current_requested_path();
                nam::NamModel m;
                std::string err;
                if (nam::load_nam(path, m, &err)) {
                    loaded_ok_ = true;
                    {
                        std::lock_guard<std::mutex> lock(model_mutex_);
                        model_ = m;
                        model_name_ = gpu_nam_basename(path);
                    }
                    rebuild_cpu_engine(m);
                    if (device_available_) rebuild_gpu_stack(m);
                    recompute_transfer_curve();
                } else {
                    runtime::log_error("GPU NAM: load_model('{}') failed ({}).", path, err);
                }
            }

            // Engine toggle (GPU publish/unpublish; the stack stays built).
            if (device_available_) {
                const int want = requested_engine_.load(std::memory_order_relaxed);
                if (want == 1 && gpu_active_.load(std::memory_order_relaxed) == nullptr) {
                    gpu_audio::GpuAudioTransport* tp = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(stack_mutex_);
                        if (current_stack_) tp = current_stack_->transport.get();
                    }
                    if (tp) {
                        gpu_active_.store(tp, std::memory_order_release);
                        gpu_engine_active_.store(true, std::memory_order_release);
                    }
                } else if (want == 0 && gpu_active_.load(std::memory_order_relaxed) != nullptr) {
                    gpu_active_.store(nullptr, std::memory_order_release);
                    gpu_engine_active_.store(false, std::memory_order_release);
                }
            }
            std::this_thread::sleep_for(5ms);
        }
    }

    double sample_rate_ = 48000.0;
    bool loaded_ok_ = false;

    // Re-blocking FIFO state (audio thread only). in_buf_ carries the drive.
    std::array<std::vector<float>, kChannels> in_buf_{};
    std::array<std::vector<float>, kChannels> out_buf_{};
    std::array<std::size_t, kChannels> in_len_{};
    std::array<std::size_t, kChannels> out_len_{};
    std::array<std::vector<float>, kChannels> dry_ring_{};   // dry delay (total latency)
    std::array<std::size_t, kChannels> dry_pos_{};
    std::array<std::vector<float>, kChannels> cpu_extra_ring_{};  // CPU wet → GPU-latency align
    std::array<std::size_t, kChannels> cpu_extra_pos_{};
    std::array<std::vector<float>, kChannels> gpu_wet_{};
    std::vector<float> wet_;                                  // internal-block scratch

    // Noise gate (on the drive) + Bass/Middle/Treble tone stack (on the model
    // output). Audio-thread only; retuned at block boundaries via the caches.
    static constexpr float kToneRangeDb = 12.0f;
    std::array<signal::NoiseGate, kChannels> gate_{};
    std::array<std::array<signal::Biquad, 3>, kChannels> tone_{};
    bool gate_active_ = true;
    bool eq_active_ = true;
    float cached_gate_threshold_ = 0.0f;
    float cached_bass_ = 0.0f;
    float cached_middle_ = 0.0f;
    float cached_treble_ = 0.0f;

    // Inline CPU engine, published lock-free for the audio thread.
    std::unique_ptr<CpuEngine> current_cpu_;
    std::unique_ptr<CpuEngine> retired_cpu_;
    std::atomic<CpuEngine*> cpu_active_{nullptr};

    // Optional GPU engine (default OFF), switchable live.
    std::unique_ptr<GpuStack> current_stack_;
    std::unique_ptr<GpuStack> retired_stack_;
    std::atomic<gpu_audio::GpuAudioTransport*> gpu_active_{nullptr};
    mutable std::mutex stack_mutex_;
    std::atomic<bool> gpu_engine_active_{false};
    bool device_available_ = false;
    std::size_t gpu_extra_ = 0;
    int latency_samples_ = static_cast<int>(kInternalBlock);

    // Worker / live-reload state.
    std::thread worker_;
    std::atomic<bool> worker_run_{false};
    std::atomic<int> requested_engine_{0};
    std::mutex model_req_mutex_;
    std::string requested_model_path_;
    std::atomic<std::uint32_t> model_req_generation_{0};
    std::uint32_t last_seen_model_gen_ = 0;

    // Authoritative model + display (UI/worker only; never the audio thread).
    mutable std::mutex model_mutex_;
    nam::NamModel model_;
    std::string model_name_ = "(no model)";
    std::atomic<bool> user_model_loaded_{false};
    TransferCurve transfer_curve_{};
    std::atomic<std::uint32_t> model_generation_{0};

    // Live peak meters (audio thread writes, UI reads). in_env_/out_env_ are
    // audio-thread-only envelope state; the dB atomics are the published values.
    float in_env_ = 0.0f, out_env_ = 0.0f;
    std::atomic<float> in_level_db_{-120.0f};
    std::atomic<float> out_level_db_{-120.0f};

    // Live output spectrum (audio thread writes, UI reads).
    static constexpr int kSpectrumFft = 2 * kSpectrumBins;  // 512
    SpectrumBus spectrum_bus_;
    signal::Fft spec_fft_{kSpectrumFft};
    std::array<float, kSpectrumFft> spec_ring_{};
    std::array<float, kSpectrumFft> spec_time_{};
    std::array<std::complex<float>, kSpectrumFft> spec_freq_{};
    int spec_pos_ = 0;
};

inline std::unique_ptr<format::Processor> create_gpu_nam() {
    return std::make_unique<GpuNamProcessor>();
}

} // namespace pulp::examples

#pragma once

// Spectral Lab — a GPU spectral freeze / morph "cloud".
//
// Capture frozen spectral "moments" (layers) from the live input on a Freeze
// trigger, then sustain and morph them into an evolving pad. Each layer is one
// windowed STFT frame whose magnitude is held while its phase keeps advancing;
// every hop the engine advances + smears + weighted-sums all captured layers and
// inverse-FFTs one frame. The Morph control scrubs a gaussian weighting across
// the captured chord so you can sweep through the frozen moments.
//
// The live audio path defaults to the RT-bounded CPU engine
// (gpu_audio::CpuSpectralStack + SpectralFreezeFramer per channel, run inline).
// An opt-in GPU engine (the Engine knob) routes the same framing through the GPU
// audio runtime: a GpuSpectralCloudNode driven by a gpu_audio::GpuAudioTransport
// on a non-RT worker. The GPU's structural win is high layer counts — the
// per-bin smear and the N-layer reduce parallelize — so the GPU engine carries
// big "clouds" (many layers) for far less per-block cost than the serial CPU.
//
// Engine (CPU<->GPU) and Layers are switchable LIVE without a reload. The audio
// thread never builds or frees an engine: a background worker builds the
// requested engine off-thread and publishes it through an atomic pointer
// (active_) that the audio thread loads each block. The previously-active engine
// is retired and freed one rebuild later, so the audio thread is never holding
// an engine as it is freed. Reported latency is FIXED for the prepared lifetime
// (the re-block FIFO + the framer's frame latency, plus the GPU transport's
// delay whenever a device exists) so the host's PDC stays correct and the engine
// can switch live without the reported latency moving. Both engines re-block the
// host stream into fixed kInternalBlock chunks because the GPU transport requires
// a fixed block size.
//
// The native GPU front-end (layer "cloud" + output spectrum + controls) is in
// spectral_lab_ui.hpp.

#include "gpu_spectral_cloud_node.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/spectral_stack.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/signal/fft.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::view { class View; }

namespace pulp::examples {

enum SpectralLabParams : state::ParamID {
    kMix    = 1,  // dry/wet, %
    kFreeze = 2,  // 0/1 capture trigger (rising edge captures the next layer)
    kLayers = 3,  // target number of frozen layers in the cloud
    kMorph  = 4,  // 0..1 scrubs the loud layer across the captured chord
    kSmear  = 5,  // 0..1 magnitude blur across frequency
    kJitter = 6,  // 0..1 per-layer phase wander
    kEngine = 7,  // 0 = CPU (default), 1 = GPU
};

// Live wet-output magnitude spectrum (dB), published lock-free from the audio
// thread to the UI's frequency display. 256 log-ready bins.
inline constexpr int kSpectrumBins = 256;
using SpectrumFrame = std::array<float, kSpectrumBins>;
using SpectrumBus = pulp::runtime::TripleBuffer<SpectrumFrame>;

class SpectralLabProcessor : public format::Processor {
public:
    // Fixed engine block. Independent of the host's block size; the re-blocking
    // FIFO chunks the host stream into this. Equal to the STFT hop so each
    // internal block is exactly one framer hop. Power of two for the radix-2 FFT.
    static constexpr std::size_t kInternalBlock = kSpectralHop;  // 512

    ~SpectralLabProcessor() override { stop_worker(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "SpectralLab",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.spectrallab",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kMix, .name = "Mix", .unit = "%",
                             .range = {0.0f, 100.0f, 60.0f, 0.1f}});
        store.add_parameter({.id = kFreeze, .name = "Freeze", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kLayers, .name = "Layers", .unit = "",
                             .range = {1.0f, 128.0f, 16.0f, 1.0f}});
        store.add_parameter({.id = kMorph, .name = "Morph", .unit = "",
                             .range = {0.0f, 1.0f, 0.5f, 0.0f}});
        store.add_parameter({.id = kSmear, .name = "Smear", .unit = "",
                             .range = {0.0f, 1.0f, 0.2f, 0.0f}});
        store.add_parameter({.id = kJitter, .name = "Jitter", .unit = "",
                             .range = {0.0f, 1.0f, 0.1f, 0.0f}});
        // Engine: 0 = CPU spectral stack (default), 1 = GPU runtime. CPU is the
        // default — the live GPU path is opt-in by governance.
        store.add_parameter({.id = kEngine, .name = "Engine", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
    }

    // Total latency, FIXED at prepare() for the prepared lifetime: the re-block
    // FIFO (kInternalBlock) + the framer's frame latency (kSpectralFft) +,
    // whenever a GPU device exists, the GPU transport's fixed delay (reported for
    // BOTH engines so a live Engine switch never moves the reported latency).
    int latency_samples() const override { return latency_samples_; }

    /// True when the live audio path is actually the GPU engine (Engine=GPU is
    /// requested AND a GPU device is available AND the engine is published).
    bool gpu_engine_active() const {
        return gpu_engine_active_.load(std::memory_order_acquire);
    }

    /// One coherent snapshot of the live engine for the UI status line, taken
    /// under a SINGLE lock so the fields can't disagree across a repaint.
    /// budget_us is how long one engine block has to finish on this device +
    /// sample rate; rt_percent is the measured average cost as a percentage of
    /// that budget, so 100 − rt_percent is the GPU headroom left. UI thread only.
    struct GpuStatus {
        bool active = false;
        std::string backend;
        int layers = 0;
        std::uint64_t blocks = 0;
        std::uint64_t misses = 0;
        double avg_us = 0.0;
        double budget_us = 0.0;
        double rt_percent = 0.0;
    };
    GpuStatus gpu_status() const {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        GpuStatus g;
        g.active = gpu_engine_active();
        if (!g.active || !current_engine_ || !current_engine_->gpu) return g;
        g.backend = current_engine_->backend();
        g.layers = current_engine_->layers;
        if (current_engine_->transport) {
            const auto s = current_engine_->transport->stats();
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

    void prepare(const format::PrepareContext& ctx) override {
        stop_worker();
        // The worker is stopped (so the audio thread is no longer running) — safe
        // to free the engines directly here.
        active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(engine_mutex_);
            current_engine_.reset();
        }
        retired_engine_.reset();

        sample_rate_ = ctx.sample_rate;
        const std::size_t max_block =
            ctx.max_buffer_size > 0 ? static_cast<std::size_t>(ctx.max_buffer_size) : 512;

        // FIFO scratch sized for the worst-case host block plus headroom for the
        // primed output latency and an in-flight internal block.
        const std::size_t cap = max_block + 4 * kInternalBlock;
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            in_buf_[ch].assign(cap, 0.0f);   in_len_[ch] = 0;
            out_buf_[ch].assign(cap, 0.0f);  out_len_[ch] = kInternalBlock;  // primed
            engine_wet_[ch].assign(kInternalBlock, 0.0f);
        }

        // Mirror the parameter state into the long-lived control block (read by
        // both engines) and the worker request atomics.
        const int init_layers = requested_layers_value();
        const int init_engine = state().get_value(kEngine) >= 0.5f ? 1 : 0;
        publish_controls();
        requested_engine_.store(init_engine, std::memory_order_relaxed);
        requested_layers_.store(init_layers, std::memory_order_relaxed);

        // A GPU device adds the transport's fixed delay (latency_blocks * block =
        // 1 * kInternalBlock) to the wet path. We learn whether a device exists by
        // probing once; the delay is rooms-/layers-independent, so it does not
        // need a full stack build.
        device_available_ = probe_gpu_device();
        gpu_extra_ = device_available_ ? kInternalBlock : 0;

        // Total latency is FIXED for the prepared lifetime — see latency_samples().
        latency_samples_ =
            static_cast<int>(kInternalBlock + kSpectralFft + gpu_extra_);
        const std::size_t dry_delay = static_cast<std::size_t>(latency_samples_);
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            dry_ring_[ch].assign(dry_delay, 0.0f);
            dry_pos_[ch] = 0;
            cpu_extra_ring_[ch].assign(gpu_extra_, 0.0f);
            cpu_extra_pos_[ch] = 0;
        }

        // Build the initial engine synchronously so audio flows from block one.
        // A GPU request that can't build (no device) cleanly falls back to CPU.
        auto fresh = build_engine(init_engine == 1, init_layers);
        if (!fresh) fresh = build_engine(false, init_layers);
        built_engine_ = fresh && fresh->gpu ? 1 : 0;
        built_layers_ = init_layers;
        if (fresh) {
            const bool is_gpu = fresh->gpu;
            LiveEngine* p = fresh.get();
            {
                std::lock_guard<std::mutex> lock(engine_mutex_);
                current_engine_ = std::move(fresh);
            }
            active_.store(p, std::memory_order_release);
            gpu_engine_active_.store(is_gpu, std::memory_order_release);
        }

        start_worker();
    }

    void release() override {
        stop_worker();  // worker stopped → audio thread stopped → safe to free
        active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(engine_mutex_);
            current_engine_.reset();
        }
        retired_engine_.reset();
    }

    /// Lock-free latest wet-output spectrum for the UI (UI is sole reader).
    SpectrumBus& spectrum_bus() { return spectrum_bus_; }

    /// Number of frozen layers currently captured into the live cloud (for the
    /// UI's layer visualization). UI/main thread only (takes the engine mutex).
    int captured_layers() const {
        std::lock_guard<std::mutex> lock(engine_mutex_);
        if (!current_engine_) return 0;
        return static_cast<int>(current_engine_->captured_layers());
    }

    /// Native GPU front-end (layer cloud + output spectrum + controls).
    std::unique_ptr<view::View> create_view() override;

    /// Resizable editor with a real design size so AU/CLAP hosts allow
    /// aspect-locked proportional resize (see super_convolver for the rationale).
    format::ViewSize view_size() const override {
        return format::view_size_from_design(820, 560);
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const std::size_t n = output.num_samples();
        const std::size_t ch_count = output.num_channels();

        const float mix = state().get_value(kMix) / 100.0f;

        // Publish the live controls (cheap atomic stores) for whichever engine is
        // currently carrying the audio, and the desired engine/layers for the
        // worker. The worker builds/frees engines off-thread and republishes
        // active_; the audio thread only ever LOADS that pointer.
        publish_controls();
        requested_engine_.store(state().get_value(kEngine) >= 0.5f ? 1 : 0,
                                std::memory_order_relaxed);
        requested_layers_.store(requested_layers_value(), std::memory_order_relaxed);

        // Fill the per-channel wet output FIFO via the active engine. A null
        // engine (only momentarily, before the first publish) produces silence,
        // which the dry/wet mix then renders as dry-only.
        LiveEngine* e = active_.load(std::memory_order_acquire);
        if (e && e->gpu)
            fill_wet_gpu(e, input, n);
        else if (e)
            fill_wet_cpu(e, input, n);

        for (std::size_t ch = 0; ch < ch_count && ch < kChannels; ++ch) {
            const float* in =
                ch < input.num_channels() ? input.channel(ch).data() : nullptr;
            float* out = output.channel(ch).data();
            const std::size_t avail = out_len_[ch];
            const std::size_t delay = dry_ring_[ch].size();
            for (std::size_t i = 0; i < n; ++i) {
                const float wet_i = i < avail ? out_buf_[ch][i] : 0.0f;
                const float dry_i = dry_ring_[ch][dry_pos_[ch]];
                dry_ring_[ch][dry_pos_[ch]] = in ? in[i] : 0.0f;
                dry_pos_[ch] = (dry_pos_[ch] + 1) % delay;
                out[i] = (1.0f - mix) * dry_i + mix * wet_i;
            }
            const std::size_t consumed = n < avail ? n : avail;
            std::memmove(out_buf_[ch].data(), out_buf_[ch].data() + consumed,
                         (out_len_[ch] - consumed) * sizeof(float));
            out_len_[ch] -= consumed;
        }

        if (ch_count > 0) publish_spectrum(output.channel(0).data(), static_cast<int>(n));
    }

private:
    static constexpr std::size_t kChannels = kSpectralChannels;  // 2

    // One self-contained engine: either the CPU spectral stacks + framers (run
    // inline on the audio thread) OR the GPU node + transport (run on a non-RT
    // worker). Heap-owned and built/freed ENTIRELY off the audio thread; the
    // audio thread only ever loads a pointer to it. Member destruction order is
    // reverse-of-declaration, so `transport` (declared last) is destroyed before
    // the node it points into.
    struct LiveEngine {
        bool gpu = false;
        int layers = 1;
        // CPU path (used when !gpu).
        std::array<gpu_audio::CpuSpectralStack, kChannels> cpu_stack{};
        std::array<gpu_audio::SpectralFreezeFramer, kChannels> cpu_framer{};
        // GPU path (used when gpu).
        std::unique_ptr<GpuSpectralCloudNode> node;
        std::unique_ptr<gpu_audio::GpuAudioTransport> transport;

        std::string backend() const {
            return node ? node->backend() : std::string("CPU");
        }
        std::uint32_t captured_layers() const {
            if (node) return node->captured_layers();
            return cpu_framer[0].captured_layers();
        }
    };

    // CPU engine: append the host block and drain full internal blocks through
    // the per-channel framer into the output FIFO. Allocation-free after prepare
    // (the stack's FFT + scratch are preallocated), so RT-bounded inline.
    void fill_wet_cpu(LiveEngine* e, const audio::BufferView<const float>& input,
                      std::size_t n) {
        const float morph = controls_.morph.load(std::memory_order_relaxed);
        const float smear = controls_.smear.load(std::memory_order_relaxed);
        const float jitter = controls_.jitter.load(std::memory_order_relaxed);
        const bool freeze = controls_.freeze.load(std::memory_order_relaxed) != 0;
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            const float* in =
                ch < input.num_channels() ? input.channel(ch).data() : nullptr;
            if (in)
                std::memcpy(in_buf_[ch].data() + in_len_[ch], in, n * sizeof(float));
            else
                std::memset(in_buf_[ch].data() + in_len_[ch], 0, n * sizeof(float));
            in_len_[ch] += n;
            while (in_len_[ch] >= kInternalBlock) {
                spectral_morph_weights(cpu_weights_.data(), e->layers,
                                       e->cpu_framer[ch].captured_layers(), morph);
                gpu_audio::SpectralFreezeControls ctl;
                ctl.weights = cpu_weights_.data();
                ctl.smear = smear;
                ctl.jitter = jitter;
                ctl.freeze = freeze;
                ctl.active = true;
                e->cpu_framer[ch].process(in_buf_[ch].data(), wet_.data(),
                                          static_cast<std::uint32_t>(kInternalBlock), ctl);
                std::memmove(in_buf_[ch].data(), in_buf_[ch].data() + kInternalBlock,
                             (in_len_[ch] - kInternalBlock) * sizeof(float));
                in_len_[ch] -= kInternalBlock;
                // Delay the CPU wet by the GPU transport's fixed extra latency so
                // CPU and GPU outputs align at the same reported latency, letting
                // the engine switch live without the wet timing moving. A no-op
                // when no GPU device exists (gpu_extra_ == 0).
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

    // GPU engine: same fixed re-blocking, but each B-block is processed as ONE
    // stereo block through the GPU transport (RT-safe by contract — the framer +
    // GPU readback run on the transport's non-RT worker; on a worker miss the
    // node's PassthroughDry policy fills the block).
    void fill_wet_gpu(LiveEngine* e, const audio::BufferView<const float>& input,
                      std::size_t n) {
        gpu_audio::GpuAudioTransport* tp = e->transport.get();
        const std::size_t in_ch = input.num_channels();
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            if (ch < in_ch)
                std::memcpy(in_buf_[ch].data() + in_len_[ch],
                            input.channel(ch).data(), n * sizeof(float));
            else
                std::memset(in_buf_[ch].data() + in_len_[ch], 0, n * sizeof(float));
            in_len_[ch] += n;
        }
        while (in_len_[0] >= kInternalBlock) {
            const float* in_ptrs[kChannels] = {in_buf_[0].data(), in_buf_[1].data()};
            float* out_ptrs[kChannels] = {engine_wet_[0].data(), engine_wet_[1].data()};
            audio::BufferView<const float> iv(in_ptrs, kChannels, kInternalBlock);
            audio::BufferView<float> ov(out_ptrs, kChannels, kInternalBlock);
            tp->process(iv, ov, static_cast<std::uint32_t>(kInternalBlock));
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                std::memmove(in_buf_[ch].data(), in_buf_[ch].data() + kInternalBlock,
                             (in_len_[ch] - kInternalBlock) * sizeof(float));
                in_len_[ch] -= kInternalBlock;
                std::memcpy(out_buf_[ch].data() + out_len_[ch], engine_wet_[ch].data(),
                            kInternalBlock * sizeof(float));
                out_len_[ch] += kInternalBlock;
            }
        }
    }

    // Build a self-contained engine off the audio thread. For gpu==true the node
    // is the GPU spectral cloud driven by a transport; returns nullptr on any
    // failure (no GPU device, transport prepare rejected) so the caller can fall
    // back to CPU. For gpu==false it prepares the per-channel CPU stacks +
    // framers. Non-RT: allocates, builds FFT plans, may spawn the transport
    // worker. MUST run on the worker / prepare, never the audio thread.
    std::unique_ptr<LiveEngine> build_engine(bool gpu, int layers) {
        const int n_layers = layers > 0 ? layers : 1;
        auto e = std::make_unique<LiveEngine>();
        e->gpu = gpu;
        e->layers = n_layers;
        if (gpu) {
            e->node = std::make_unique<GpuSpectralCloudNode>(
                static_cast<std::uint32_t>(kChannels),
                static_cast<std::uint32_t>(kInternalBlock),
                static_cast<std::uint32_t>(sample_rate_), n_layers, &controls_);
            if (!e->node->prepare() || !e->node->gpu_available()) return nullptr;
            e->transport = std::make_unique<gpu_audio::GpuAudioTransport>();
            gpu_audio::GpuAudioTransport::Config cfg;
            cfg.ring_blocks = 8;
            cfg.run_worker_thread = true;
            if (!e->transport->prepare(e->node.get(), cfg)) return nullptr;
        } else {
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                if (!e->cpu_stack[ch].prepare(kSpectralFft, kSpectralHop,
                                              static_cast<std::uint32_t>(n_layers)))
                    return nullptr;
                if (!e->cpu_framer[ch].prepare(&e->cpu_stack[ch], kSpectralFft,
                                               kSpectralHop))
                    return nullptr;
            }
        }
        return e;
    }

    // Worker-thread only. Build the requested engine, then atomically swap it in
    // (old → new, never null) and RETIRE the previous engine, freed on the NEXT
    // rebuild — so the audio thread never frees an engine it might be loading.
    void rebuild_engine(bool gpu, int layers) {
        // The audio thread has had many blocks to drop the previously-retired
        // pointer; freeing it now is safe.
        retired_engine_.reset();
        auto fresh = build_engine(gpu, layers);
        if (!fresh && gpu) {
            runtime::log_info(
                "SpectralLab: GPU engine build failed (layers={}); "
                "routing the CPU spectral engine.", layers);
            fresh = build_engine(false, layers);
        }
        if (!fresh) return;  // keep the current engine
        const bool is_gpu = fresh->gpu;
        LiveEngine* p = fresh.get();
        {
            std::lock_guard<std::mutex> lock(engine_mutex_);
            retired_engine_ = std::move(current_engine_);
            current_engine_ = std::move(fresh);
        }
        active_.store(p, std::memory_order_release);
        gpu_engine_active_.store(is_gpu, std::memory_order_release);
        built_engine_ = is_gpu ? 1 : 0;
        built_layers_ = layers;
    }

    void publish_controls() {
        controls_.morph.store(state().get_value(kMorph), std::memory_order_relaxed);
        controls_.smear.store(state().get_value(kSmear), std::memory_order_relaxed);
        controls_.jitter.store(state().get_value(kJitter), std::memory_order_relaxed);
        controls_.freeze.store(state().get_value(kFreeze) >= 0.5f ? 1 : 0,
                               std::memory_order_relaxed);
    }

    int requested_layers_value() const {
        const int l = static_cast<int>(std::lround(state().get_value(kLayers)));
        return l > 1 ? l : 1;
    }

    static bool probe_gpu_device() {
        auto g = render::GpuCompute::create();
        return g && g->initialize_standalone();
    }

    // Accumulate the output into a ring; once per block run a windowed FFT and
    // publish a 256-bin dB magnitude spectrum. RT-safe: the Fft is preallocated.
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
            const float mag = std::abs(spec_freq_[static_cast<std::size_t>(k)]) /
                              (kSpectrumFft * 0.25f);
            frame[static_cast<std::size_t>(k)] = 20.0f * std::log10(mag + 1e-7f);
        }
        spectrum_bus_.write(frame);
    }

    void start_worker() {
        worker_run_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { worker_loop(); });
    }

    void stop_worker() {
        worker_run_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    // Background thread: rebuild the live engine whenever Engine or Layers
    // changes. Never touches the audio thread's buffers or the published engine
    // while the audio thread might hold it (the atomic swap + deferred free do
    // that safely).
    void worker_loop() {
        using namespace std::chrono_literals;
        while (worker_run_.load(std::memory_order_acquire)) {
            const int want_engine = requested_engine_.load(std::memory_order_relaxed);
            const int want_layers = requested_layers_.load(std::memory_order_relaxed);
            if (want_engine != built_engine_ || want_layers != built_layers_)
                rebuild_engine(want_engine == 1, want_layers);
            std::this_thread::sleep_for(5ms);
        }
    }

    double sample_rate_ = 48000.0;

    // Long-lived live controls, read by whichever engine is active (the CPU
    // framer inline, or the GPU node on the worker). Outlives every engine.
    SpectralControlBlock controls_;

    // Re-blocking FIFO state (audio thread only).
    std::array<std::vector<float>, kChannels> in_buf_{};
    std::array<std::vector<float>, kChannels> out_buf_{};
    std::array<std::size_t, kChannels> in_len_{};
    std::array<std::size_t, kChannels> out_len_{};
    std::array<std::vector<float>, kChannels> dry_ring_{};   // dry delay (total latency)
    std::array<std::size_t, kChannels> dry_pos_{};
    std::array<std::vector<float>, kChannels> cpu_extra_ring_{};
    std::array<std::size_t, kChannels> cpu_extra_pos_{};
    std::vector<float> wet_ =                                 // internal-block scratch
        std::vector<float>(kInternalBlock, 0.0f);
    std::vector<float> cpu_weights_ =                         // morph weights scratch
        std::vector<float>(128, 0.0f);
    std::array<std::vector<float>, kChannels> engine_wet_{};  // GPU transport scratch

    // Live engine (CPU default, GPU opt-in), switchable live. The worker owns
    // current_engine_ (the built engine) and retired_engine_ (a previous engine
    // pending free, reclaimed one rebuild later). The audio thread routes solely
    // through active_ (an atomic pointer into current_engine_, or null before the
    // first publish). engine_mutex_ guards current_engine_ for the UI accessors
    // vs the worker — the audio thread NEVER takes it.
    std::unique_ptr<LiveEngine> current_engine_;
    std::unique_ptr<LiveEngine> retired_engine_;
    std::atomic<LiveEngine*> active_{nullptr};
    mutable std::mutex engine_mutex_;
    std::atomic<bool> gpu_engine_active_{false};   // mirrors (active_ && active_->gpu)
    bool device_available_ = false;
    std::size_t gpu_extra_ = 0;                     // GPU transport latency, samples (fixed)
    int latency_samples_ = static_cast<int>(kInternalBlock + kSpectralFft);

    // Worker / live-engine request state.
    std::thread worker_;
    std::atomic<bool> worker_run_{false};
    std::atomic<int> requested_engine_{0};   // 0 = CPU, 1 = GPU
    std::atomic<int> requested_layers_{16};
    int built_engine_ = -1;                  // worker-thread-local (current config)
    int built_layers_ = -1;                  // worker-thread-local (current config)

    // Live wet-output spectrum (audio thread writes, UI reads).
    static constexpr int kSpectrumFft = 2 * kSpectrumBins;  // 512
    SpectrumBus spectrum_bus_;
    signal::Fft spec_fft_{kSpectrumFft};
    std::array<float, kSpectrumFft> spec_ring_{};
    std::array<float, kSpectrumFft> spec_time_{};
    std::array<std::complex<float>, kSpectrumFft> spec_freq_{};
    int spec_pos_ = 0;
};

inline std::unique_ptr<format::Processor> create_spectral_lab() {
    return std::make_unique<SpectralLabProcessor>();
}

} // namespace pulp::examples

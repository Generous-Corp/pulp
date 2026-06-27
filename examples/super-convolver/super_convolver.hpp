#pragma once

// SuperConvolver — a convolution reverb / impulse processor.
//
// The live audio path is the RT-safe CPU convolution engine
// (signal::PartitionedConvolver, uniform overlap-save). The convolver requires
// a fixed block size, so an internal re-blocking FIFO feeds it fixed
// kInternalBlock chunks regardless of the host's (variable, often smaller)
// block — this is what makes the reverb correct in every host and the
// standalone (whose max block is floored well above the real device pull). The
// re-block adds kInternalBlock samples of latency, reported via
// latency_samples() so the host's PDC aligns it; the dry path is delayed to
// match so the dry/wet mix stays phase-coherent.
//
// Runtime IR changes (the Size knob) are rebuilt off-thread and handed to the
// audio thread through the lock-free signal::ConvolverIrSwapper — process()
// never allocates or runs an FFT plan.
//
// An optional, default-OFF GPU engine (the Engine knob) routes the same
// fixed-block convolution through the real GPU audio runtime
// (gpu_audio::GpuConvolver driven by gpu_audio::GpuAudioTransport) instead of
// the CPU PartitionedConvolver. The transport runs the GPU FFT on its own
// non-RT worker and hands the audio thread a fixed-latency, lock-free result;
// if no GPU device is present (or the transport fails to prepare) the processor
// transparently falls back to the CPU engine so the plugin always works.
//
// Engine (CPU<->GPU) and Rooms are switchable LIVE, without a reload. The audio
// thread never builds or frees a GPU stack: a background worker builds the
// requested stack off-thread and publishes it through an atomic pointer
// (gpu_active_) that the audio thread loads each block — non-null routes the GPU
// path, null routes the CPU path. The previously-active stack is retired and
// freed one rebuild later, so the audio thread is never holding a stack as it is
// freed. Reported latency is FIXED for the prepared lifetime (kInternalBlock
// plus the GPU transport's delay when a device exists) so the host's PDC stays
// correct and dry/wet stay phase-aligned under either engine. See
// gpu_engine_active().
//
// The native GPU front-end (live IR waveform + frequency display + controls,
// rendered through canvas/Skia/Dawn) is in super_convolver_ui.hpp.

#include <pulp/format/processor.hpp>
#include <pulp/signal/convolver.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_multi_convolver.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include <memory>

#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::view { class View; }

namespace pulp::examples {

enum SuperConvolverParams : state::ParamID {
    kMix     = 1,  // dry/wet, %
    kSize    = 2,  // IR length, seconds
    kGain    = 3,  // output gain, dB
    kBypass  = 4,
    kEngine  = 5,  // 0 = CPU (default), 1 = GPU
    kRooms   = 6,  // GPU multi-room reverb: # of distinct IRs in one GPU batch
};

// Live wet-output magnitude spectrum (dB), published lock-free from the audio
// thread to the GPU UI's frequency display. 256 log-ready bins.
inline constexpr int kSpectrumBins = 256;
using SpectrumFrame = std::array<float, kSpectrumBins>;
using SpectrumBus = pulp::runtime::TripleBuffer<SpectrumFrame>;

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
    // Fixed convolver block. Independent of the host's block size; the
    // re-blocking FIFO chunks the host stream into this. Power of two for the
    // radix-2 FFT. 256 samples ≈ 5.3 ms latency at 48 kHz — fine for a reverb.
    static constexpr std::size_t kInternalBlock = 256;

    ~SuperConvolverProcessor() override { stop_worker(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "SuperConvolver",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.superconvolver",
            .version = "1.0.5",
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
        // Engine: 0 = CPU PartitionedConvolver (default), 1 = GPU runtime.
        // CPU is the default — the live GPU path is opt-in by governance.
        store.add_parameter({.id = kEngine, .name = "Engine", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        // Rooms: with Engine=GPU and Rooms>1, the input is convolved against
        // this many DISTINCT impulse responses ("rooms"), each panned to its own
        // stereo position, in ONE batched GPU submit per block — a dense
        // multi-room convolution reverb that is the GPU's structural win over the
        // CPU (which would need Rooms× independent convolvers). Rooms=1 keeps the
        // single-IR GPU path. No effect when Engine=CPU.
        store.add_parameter({.id = kRooms, .name = "Rooms", .unit = "",
                             .range = {1.0f, 256.0f, 16.0f, 1.0f}});
    }

    // Total latency, FIXED at prepare() for the prepared lifetime: the re-block
    // FIFO adds kInternalBlock, plus — whenever a GPU device exists — the GPU
    // transport's fixed delay, reported for BOTH engines so a live Engine switch
    // never moves the reported latency (the host's PDC stays correct and dry/wet
    // stay phase-aligned regardless of which engine is currently active).
    int latency_samples() const override { return latency_samples_; }

    /// True when the live audio path is actually the GPU engine (Engine=GPU is
    /// requested AND a GPU device is available AND the transport is published).
    /// If the GPU was requested but unavailable, this is false — the processor
    /// runs the CPU engine. Switchable live; safe to poll at any time.
    bool gpu_engine_active() const {
        return gpu_engine_active_.load(std::memory_order_acquire);
    }

    /// The live GPU compute backend ("Metal"/"D3D12"/"Vulkan") when the GPU
    /// engine is active, else "". UI/main-thread only (takes the worker mutex).
    std::string gpu_backend() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        if (!gpu_engine_active() || !current_stack_) return std::string();
        return current_stack_->backend();
    }

    /// Number of GPU convolution rooms in the live audio path (1 for the single
    /// IR path, >1 for the batched multi-room mode), or 0 when the GPU engine is
    /// not active. UI/main-thread only (takes the worker mutex).
    int gpu_rooms() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        if (!gpu_engine_active() || !current_stack_) return 0;
        return current_stack_->rooms;
    }

    /// True when the live path is the batched multi-room GPU reverb.
    bool gpu_multi_active() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        return gpu_engine_active() && current_stack_ && current_stack_->multi != nullptr;
    }

    /// Live {GPU blocks produced, blocks missed (CPU-filled)} so the UI can show
    /// whether the GPU is actually carrying the work or mostly falling back.
    /// UI/main-thread only (takes the worker mutex; never the audio thread).
    std::pair<std::uint64_t, std::uint64_t> gpu_block_stats() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        if (!gpu_engine_active() || !current_stack_ || !current_stack_->transport)
            return {0, 0};
        const auto s = current_stack_->transport->stats();
        return {s.produced_blocks, s.miss_blocks};
    }

    void prepare(const format::PrepareContext& ctx) override {
        stop_worker();
        // Tear down any previous GPU stacks (the worker is stopped, so the audio
        // thread is no longer running — safe to free directly here).
        gpu_active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            current_stack_.reset();
        }
        retired_stack_.reset();

        sample_rate_ = ctx.sample_rate;
        const std::size_t max_block =
            ctx.max_buffer_size > 0 ? static_cast<std::size_t>(ctx.max_buffer_size) : 512;

        // FIFO scratch sized for the worst-case host block plus headroom for the
        // primed output latency and an in-flight internal block.
        const std::size_t cap = max_block + 4 * kInternalBlock;
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            in_buf_[ch].assign(cap, 0.0f);   in_len_[ch] = 0;
            out_buf_[ch].assign(cap, 0.0f);  out_len_[ch] = kInternalBlock;  // primed: B zeros
            gpu_wet_[ch].assign(kInternalBlock, 0.0f);
            // Drop any IR a previous lifecycle's worker staged but the audio
            // thread never consumed (it was built at the old block size).
            while (swapper_[ch].try_consume()) { /* freed here, off the audio thread */ }
        }
        wet_.assign(kInternalBlock, 0.0f);

        rebuild_ir_inline(current_size());   // first IR loaded synchronously (CPU)

        // Learn the GPU transport latency by pre-building the stack once when a
        // device exists. Latency is rooms-independent, but pre-building at the
        // currently-requested Rooms/Size also makes the first live switch to GPU
        // instant. The stack stays built even when Engine=CPU; gpu_active_ is
        // only published when Engine=GPU.
        const int init_rooms = requested_rooms_value();
        const float init_size = current_size();
        gpu_extra_ = 0;
        device_available_ = false;
        {
            auto stack = build_gpu_stack(init_rooms, init_size);
            if (stack) {
                device_available_ = true;
                gpu_extra_ = static_cast<std::size_t>(stack->transport->latency_samples());
                std::lock_guard<std::mutex> lock(stack_mutex_);
                current_stack_ = std::move(stack);
            }
        }
        gpu_built_rooms_ = init_rooms > 1 ? init_rooms : 1;
        gpu_built_size_ = init_size;

        // Total latency is FIXED for the prepared lifetime: the re-block FIFO
        // (kInternalBlock) plus, whenever a GPU device exists, the transport's
        // fixed delay — applied to BOTH engines (the GPU transport supplies it on
        // the GPU path; the cpu_extra_ring_ supplies it on the CPU path) so the
        // engine can switch live without the reported latency moving. When no GPU
        // device exists gpu_extra_ == 0 and the latency is just kInternalBlock.
        latency_samples_ = static_cast<int>(kInternalBlock + gpu_extra_);
        const std::size_t dry_delay = static_cast<std::size_t>(latency_samples_);
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            dry_ring_[ch].assign(dry_delay, 0.0f);
            dry_pos_[ch] = 0;
            cpu_extra_ring_[ch].assign(gpu_extra_, 0.0f);
            cpu_extra_pos_[ch] = 0;
        }

        // Publish the pre-built stack immediately when Engine=GPU so the GPU path
        // carries audio from the first block.
        requested_engine_.store(state().get_value(kEngine) >= 0.5f ? 1 : 0,
                                std::memory_order_relaxed);
        requested_rooms_.store(init_rooms, std::memory_order_relaxed);
        requested_size_.store(init_size, std::memory_order_relaxed);
        if (requested_engine_.load(std::memory_order_relaxed) == 1) {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            if (current_stack_) {
                gpu_active_.store(current_stack_->transport.get(),
                                  std::memory_order_release);
                gpu_engine_active_.store(true, std::memory_order_release);
            }
        }

        start_worker();
    }

    void release() override {
        stop_worker();   // worker stopped → audio thread stopped → safe to free
        gpu_active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            current_stack_.reset();
        }
        retired_stack_.reset();
        for (auto& c : conv_) c.reset();
    }

    /// A snapshot of the current impulse response for the GPU UI waveform /
    /// spectrogram. UI-thread only — never call from the audio thread.
    std::vector<float> impulse_response_snapshot() const {
        std::lock_guard<std::mutex> lock(ir_display_mutex_);
        return ir_display_;
    }

    /// Monotonic counter bumped each time the displayed IR changes, so the UI
    /// can skip re-pulling the snapshot when nothing changed.
    std::uint32_t ir_generation() const { return ir_generation_.load(std::memory_order_relaxed); }

    /// Lock-free latest wet-output spectrum for the UI (UI is sole reader).
    SpectrumBus& spectrum_bus() { return spectrum_bus_; }

    /// Native GPU front-end (live IR waveform + frequency display + controls).
    std::unique_ptr<view::View> create_view() override;

    /// Declare a resizable editor with a real design size. Without this the base
    /// default is a tiny 400x300 with min=0, which CLAP's gui_can_resize (and the
    /// AU preferred-size path) read as "fixed, non-resizable" — so AU/CLAP in
    /// Logic open small and won't resize. view_size_from_design() derives the
    /// min/max/aspect so hosts allow aspect-locked proportional resize; the UI is
    /// fully proportional (scale() = height/560), so any size looks right.
    format::ViewSize view_size() const override {
        return format::view_size_from_design(820, 560);
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const std::size_t n = output.num_samples();
        const std::size_t ch_count = output.num_channels();

        const bool bypass = state().get_value(kBypass) >= 0.5f;
        const float mix = bypass ? 0.0f : state().get_value(kMix) / 100.0f;
        const float gain = std::pow(10.0f, state().get_value(kGain) / 20.0f);

        // Publish the desired live config to the worker (cheap atomic stores, no
        // alloc). The worker builds/frees stacks off-thread and republishes
        // gpu_active_; the audio thread only ever LOADS that pointer.
        requested_engine_.store(state().get_value(kEngine) >= 0.5f ? 1 : 0,
                                std::memory_order_relaxed);
        requested_rooms_.store(requested_rooms_value(), std::memory_order_relaxed);
        requested_size_.store(current_size(), std::memory_order_relaxed);

        // Fill the per-channel wet output FIFO via the active engine. Both engines
        // re-block the host stream into fixed kInternalBlock chunks; only the
        // convolution itself (CPU PartitionedConvolver vs GPU transport) differs.
        // A non-null gpu_active_ (acquire) routes the GPU path; null routes CPU.
        gpu_audio::GpuAudioTransport* tp =
            gpu_active_.load(std::memory_order_acquire);
        if (tp)
            fill_wet_gpu(tp, input, n);
        else
            fill_wet_cpu(input, n);

        // Emit n samples per channel: wet from the primed output FIFO, dry
        // delayed by the active engine's total latency so they stay aligned.
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
                out[i] = (1.0f - mix) * dry_i + mix * gain * wet_i;
            }
            const std::size_t consumed = n < avail ? n : avail;
            std::memmove(out_buf_[ch].data(), out_buf_[ch].data() + consumed,
                         (out_len_[ch] - consumed) * sizeof(float));
            out_len_[ch] -= consumed;
        }

        if (ch_count > 0) publish_spectrum(output.channel(0).data(), static_cast<int>(n));
    }

private:
    static constexpr std::size_t kChannels = 2;

    // One self-contained GPU engine: the node (single-IR OR multi-room) plus the
    // transport that drives it on a non-RT worker. Heap-owned and built/freed
    // ENTIRELY by the worker thread; the audio thread only ever loads a pointer to
    // `transport`. Member destruction order is reverse-of-declaration, so
    // `transport` (declared last of the owning ptrs) is destroyed before the node
    // — which is required because the transport holds a pointer into the node.
    struct GpuStack {
        std::unique_ptr<gpu_audio::GpuConvolver> single;
        std::unique_ptr<gpu_audio::GpuMultiConvolver> multi;
        std::unique_ptr<gpu_audio::GpuAudioTransport> transport;
        int rooms = 1;
        std::string backend() const {
            if (multi) return multi->backend();
            return single ? single->backend() : std::string();
        }
    };

    // CPU engine: append the host block and drain full internal blocks through
    // the per-channel PartitionedConvolver into the output FIFO. RT-safe.
    void fill_wet_cpu(const audio::BufferView<const float>& input, std::size_t n) {
        // Block-boundary IR handoff: pick up anything the worker staged. RT-safe
        // (two atomic pointer ops, no alloc, no free).
        for (std::size_t ch = 0; ch < conv_.size(); ++ch)
            conv_[ch].try_swap_ir(swapper_[ch]);
        // Tell the worker the currently-requested IR length (atomic store).
        requested_size_.store(current_size(), std::memory_order_relaxed);

        for (std::size_t ch = 0; ch < input.num_channels() && ch < kChannels; ++ch) {
            const float* in = input.channel(ch).data();
            std::memcpy(in_buf_[ch].data() + in_len_[ch], in, n * sizeof(float));
            in_len_[ch] += n;
            while (in_len_[ch] >= kInternalBlock) {
                conv_[ch].process(in_buf_[ch].data(), wet_.data(), kInternalBlock);
                std::memmove(in_buf_[ch].data(), in_buf_[ch].data() + kInternalBlock,
                             (in_len_[ch] - kInternalBlock) * sizeof(float));
                in_len_[ch] -= kInternalBlock;
                // Delay the CPU wet by the GPU transport's fixed extra latency so
                // CPU and GPU outputs align at the same reported latency — letting
                // the engine switch live without dry/wet drifting. When no GPU
                // device exists gpu_extra_ == 0 and the ring is a no-op.
                delay_cpu_wet(ch);
                std::memcpy(out_buf_[ch].data() + out_len_[ch], wet_.data(),
                            kInternalBlock * sizeof(float));
                out_len_[ch] += kInternalBlock;
            }
        }
    }

    // Push one internal block of CPU wet through the per-channel extra-delay ring
    // (length gpu_extra_), in place. RT-safe: the ring is preallocated, no alloc.
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
    // stereo block through the GPU transport (RT-safe by contract — the GPU FFT
    // runs on the transport's non-RT worker; on a worker miss the node's
    // PassthroughDry policy fills the block). Both channels advance in lockstep
    // (the same n is appended every call), so in_len_[0] gates the drain.
    void fill_wet_gpu(gpu_audio::GpuAudioTransport* tp,
                      const audio::BufferView<const float>& input, std::size_t n) {
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
            float* out_ptrs[kChannels] = {gpu_wet_[0].data(), gpu_wet_[1].data()};
            audio::BufferView<const float> iv(in_ptrs, kChannels, kInternalBlock);
            audio::BufferView<float> ov(out_ptrs, kChannels, kInternalBlock);
            tp->process(iv, ov, static_cast<uint32_t>(kInternalBlock));
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                std::memmove(in_buf_[ch].data(), in_buf_[ch].data() + kInternalBlock,
                             (in_len_[ch] - kInternalBlock) * sizeof(float));
                in_len_[ch] -= kInternalBlock;
                std::memcpy(out_buf_[ch].data() + out_len_[ch], gpu_wet_[ch].data(),
                            kInternalBlock * sizeof(float));
                out_len_[ch] += kInternalBlock;
            }
        }
    }

    // Build a self-contained GPU stack (node + transport) off the audio thread.
    // With rooms>1 the node is the batched multi-room GPU reverb
    // (gpu_audio::GpuMultiConvolver); with rooms==1 it is the single-IR
    // gpu_audio::GpuConvolver. Returns nullptr on any failure (no GPU device,
    // transport prepare rejected) — the caller then routes the CPU path. Both
    // node kinds are GpuAudioNodes driven by the same transport, so the RT path
    // (fill_wet_gpu) is identical. Non-RT: allocates, builds FFT plans, spawns the
    // transport worker thread. MUST be called from the worker / prepare, never the
    // audio thread.
    std::unique_ptr<GpuStack> build_gpu_stack(int rooms, float size) {
        auto stack = std::make_unique<GpuStack>();
        stack->rooms = rooms > 1 ? rooms : 1;
        const std::size_t len = ir_length_for(size);
        gpu_audio::GpuAudioNode* node = nullptr;

        if (rooms > 1) {
            // N decorrelated rooms from `size`, distinct per-room seeds.
            std::vector<std::vector<float>> irs;
            irs.reserve(static_cast<std::size_t>(rooms));
            for (int k = 0; k < rooms; ++k)
                irs.push_back(make_reverb_ir(
                    len, 0x51C04711u + static_cast<std::uint32_t>(k) * 2654435761u));
            stack->multi = std::make_unique<gpu_audio::GpuMultiConvolver>(
                static_cast<uint32_t>(kInternalBlock),
                static_cast<uint32_t>(sample_rate_), std::move(irs));
            if (stack->multi->prepare() && stack->multi->gpu_available())
                node = stack->multi.get();
        } else {
            auto ir = make_reverb_ir(len);
            stack->single = std::make_unique<gpu_audio::GpuConvolver>(
                static_cast<uint32_t>(kChannels), static_cast<uint32_t>(kInternalBlock),
                static_cast<uint32_t>(sample_rate_), std::move(ir));
            if (stack->single->prepare() && stack->single->gpu_available())
                node = stack->single.get();
        }
        if (!node) return nullptr;

        stack->transport = std::make_unique<gpu_audio::GpuAudioTransport>();
        gpu_audio::GpuAudioTransport::Config cfg;
        cfg.ring_blocks = 8;
        cfg.run_worker_thread = true;
        if (!stack->transport->prepare(node, cfg)) return nullptr;
        return stack;
    }

    // Worker-thread only. Build a fresh stack for (rooms, size) and atomically
    // hand it to the audio thread. The currently-active stack is RETIRED (moved
    // to retired_stack_) and freed on the NEXT rebuild — never freed while the
    // audio thread might still be loading its pointer. On build failure the GPU
    // path is published as null so the audio thread routes the CPU engine.
    void rebuild_gpu_stack(int rooms, float size) {
        // The audio thread has had many blocks since the last rebuild to drop the
        // previously-retired pointer; freeing it now is safe.
        retired_stack_.reset();
        // Stop the audio thread from using the current stack before retiring it.
        gpu_active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            retired_stack_ = std::move(current_stack_);
        }

        auto fresh = build_gpu_stack(rooms, size);
        gpu_built_rooms_ = rooms > 1 ? rooms : 1;
        gpu_built_size_ = size;
        if (!fresh) {
            runtime::log_info(
                "SuperConvolver: GPU stack rebuild failed (rooms={}); "
                "routing the CPU convolution engine.", rooms);
            return;  // gpu_active_ already null → CPU path
        }
        gpu_audio::GpuAudioTransport* tp = fresh->transport.get();
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            current_stack_ = std::move(fresh);
        }
        gpu_active_.store(tp, std::memory_order_release);
        gpu_engine_active_.store(true, std::memory_order_release);
    }

    int requested_rooms_value() const {
        const int r = static_cast<int>(std::lround(state().get_value(kRooms)));
        return r > 1 ? r : 1;
    }

    // Accumulate the output into a ring; once per block run a windowed FFT and
    // publish a 256-bin dB magnitude spectrum. RT-safe: the Fft is preallocated
    // and the TripleBuffer write never blocks.
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

    float current_size() const {
        return quantize_size(state().get_value(kSize));
    }

    // Quantize Size so a continuous host automation ramp doesn't make the worker
    // rebuild a (potentially 192k-tap) IR every poll tick. 0.05 s steps are
    // inaudible for a reverb tail.
    static float quantize_size(float s) {
        const float q = std::round(s * 20.0f) / 20.0f;
        return q > 0.05f ? q : 0.05f;
    }

    std::size_t ir_length_for(float seconds) const {
        std::size_t len = static_cast<std::size_t>(seconds * sample_rate_);
        return len < 1 ? 1 : len;
    }

    // prepare-time: build the IR and load it into both channels synchronously.
    void rebuild_ir_inline(float seconds) {
        auto ir = make_reverb_ir(ir_length_for(seconds));
        for (auto& c : conv_) c.load_ir(ir.data(), ir.size(), kInternalBlock);
        publish_display_ir(ir);
        worker_built_size_ = seconds;
        requested_size_.store(seconds, std::memory_order_relaxed);
    }

    void publish_display_ir(const std::vector<float>& ir) {
        {
            std::lock_guard<std::mutex> lock(ir_display_mutex_);
            ir_display_ = ir;
        }
        ir_generation_.fetch_add(1, std::memory_order_relaxed);
    }

    void start_worker() {
        worker_run_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { worker_loop(); });
    }

    void stop_worker() {
        worker_run_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    // Background thread: rebuild + stage the CPU IR whenever Size changes, manage
    // the live GPU stack as Engine/Rooms/Size change, and reclaim displaced IRs.
    // Never touches the audio thread's buffers or the atomic-published stack while
    // the audio thread might hold it. Builds both channels' CPU states from one IR
    // and stages them back-to-back to keep the L/R swap window minimal.
    void worker_loop() {
        using namespace std::chrono_literals;
        while (worker_run_.load(std::memory_order_acquire)) {
            const float want_size = requested_size_.load(std::memory_order_relaxed);
            const int want_engine = requested_engine_.load(std::memory_order_relaxed);
            const int want_rooms = requested_rooms_.load(std::memory_order_relaxed);

            // CPU IR rebuild on Size change (always — CPU is the fallback engine).
            if (want_size != worker_built_size_ && want_size > 0.0f) {
                auto ir = make_reverb_ir(ir_length_for(want_size));
                for (auto& sw : swapper_)
                    sw.stage_ir(ir.data(), ir.size(), kInternalBlock);
                publish_display_ir(ir);
                worker_built_size_ = want_size;
            }

            // GPU stack management (only meaningful when a device exists).
            if (device_available_)
                service_gpu_stack(want_engine, want_rooms, want_size);

            for (auto& sw : swapper_) sw.drain_old();
            std::this_thread::sleep_for(5ms);
        }
        // Final drain so nothing leaks once the audio thread has stopped.
        for (auto& sw : swapper_) sw.drain_old();
    }

    // Worker-thread only. Reconcile the published GPU path with the requested
    // Engine/Rooms/Size: rebuild on a Rooms/Size change, republish the pre-built
    // stack on a CPU->GPU toggle (instant), and unpublish on a GPU->CPU toggle
    // while KEEPING the stack built so the next switch back is instant too.
    void service_gpu_stack(int want_engine, int want_rooms, float want_size) {
        const int rooms = want_rooms > 1 ? want_rooms : 1;
        if (want_engine == 1) {
            const bool config_changed =
                !current_stack_ || gpu_built_rooms_ != rooms || gpu_built_size_ != want_size;
            if (config_changed) {
                rebuild_gpu_stack(rooms, want_size);
            } else if (gpu_active_.load(std::memory_order_relaxed) == nullptr) {
                // Stack already built for this config (after a GPU->CPU toggle) —
                // just republish for an instant switch.
                gpu_audio::GpuAudioTransport* tp = nullptr;
                {
                    std::lock_guard<std::mutex> lock(stack_mutex_);
                    if (current_stack_) tp = current_stack_->transport.get();
                }
                if (tp) {
                    gpu_active_.store(tp, std::memory_order_release);
                    gpu_engine_active_.store(true, std::memory_order_release);
                }
            }
        } else {  // Engine == CPU: stop the GPU path, keep the stack built.
            if (gpu_active_.load(std::memory_order_relaxed) != nullptr) {
                gpu_active_.store(nullptr, std::memory_order_release);
                gpu_engine_active_.store(false, std::memory_order_release);
            }
        }
    }

    double sample_rate_ = 48000.0;

    // Re-blocking FIFO state (audio thread only).
    std::array<std::vector<float>, kChannels> in_buf_{};
    std::array<std::vector<float>, kChannels> out_buf_{};
    std::array<std::size_t, kChannels> in_len_{};
    std::array<std::size_t, kChannels> out_len_{};
    std::array<std::vector<float>, kChannels> dry_ring_{};   // dry delay (total latency)
    std::array<std::size_t, kChannels> dry_pos_{};
    // Per-channel extra delay applied to the CPU wet so it lines up with the GPU
    // wet at the same fixed reported latency (length gpu_extra_; empty when no GPU
    // device exists). Audio thread only.
    std::array<std::vector<float>, kChannels> cpu_extra_ring_{};
    std::array<std::size_t, kChannels> cpu_extra_pos_{};
    std::vector<float> wet_;                                  // internal-block scratch

    std::array<signal::PartitionedConvolver, kChannels> conv_{};
    std::array<signal::ConvolverIrSwapper, kChannels> swapper_{};

    // Optional GPU engine (default OFF), switchable live. The worker owns
    // current_stack_ (the built stack) and retired_stack_ (a previous stack
    // pending free, reclaimed one rebuild later). The audio thread routes the GPU
    // path solely through gpu_active_ (an atomic pointer into current_stack_'s
    // transport, or null for the CPU path). stack_mutex_ guards current_stack_ for
    // the UI accessors vs the worker — the audio thread NEVER takes it. gpu_wet_
    // is the per-channel B-sized scratch the transport writes each stereo block.
    std::unique_ptr<GpuStack> current_stack_;
    std::unique_ptr<GpuStack> retired_stack_;
    std::atomic<gpu_audio::GpuAudioTransport*> gpu_active_{nullptr};
    mutable std::mutex stack_mutex_;
    std::array<std::vector<float>, kChannels> gpu_wet_{};
    std::atomic<bool> gpu_engine_active_{false};   // mirrors (gpu_active_ != null)
    bool device_available_ = false;                // a GPU device exists (set at prepare)
    std::size_t gpu_extra_ = 0;                     // GPU transport latency, samples (fixed)
    int latency_samples_ = static_cast<int>(kInternalBlock);

    // Worker / live-IR-swap + live-engine state.
    std::thread worker_;
    std::atomic<bool> worker_run_{false};
    std::atomic<float> requested_size_{-1.0f};
    std::atomic<int> requested_engine_{0};          // 0 = CPU, 1 = GPU
    std::atomic<int> requested_rooms_{1};
    float worker_built_size_ = -1.0f;  // worker-thread-local (CPU IR)
    int gpu_built_rooms_ = 0;          // worker-thread-local (current_stack_ config)
    float gpu_built_size_ = -1.0f;     // worker-thread-local (current_stack_ config)

    // UI display snapshot (UI + worker thread only; never audio thread).
    mutable std::mutex ir_display_mutex_;
    std::vector<float> ir_display_;
    std::atomic<std::uint32_t> ir_generation_{0};

    // Live wet-output spectrum (audio thread writes, UI reads).
    static constexpr int kSpectrumFft = 2 * kSpectrumBins;  // 512
    SpectrumBus spectrum_bus_;
    signal::Fft spec_fft_{kSpectrumFft};
    std::array<float, kSpectrumFft> spec_ring_{};
    std::array<float, kSpectrumFft> spec_time_{};
    std::array<std::complex<float>, kSpectrumFft> spec_freq_{};
    int spec_pos_ = 0;
};

inline std::unique_ptr<format::Processor> create_super_convolver() {
    return std::make_unique<SuperConvolverProcessor>();
}

} // namespace pulp::examples

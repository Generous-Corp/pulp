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
// transparently falls back to the CPU engine so the plugin always works. The
// engine is chosen once at prepare() (not per block) so the reported latency is
// stable. See gpu_engine_active().
//
// The native GPU front-end (live IR waveform + frequency display + controls,
// rendered through canvas/Skia/Dawn) is in super_convolver_ui.hpp.

#include <pulp/format/processor.hpp>
#include <pulp/signal/convolver.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>
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
            .version = "1.0.3",
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
    }

    // The active engine's total latency, chosen once at prepare(): the re-block
    // FIFO adds kInternalBlock; the GPU transport adds its own (latency_blocks *
    // kInternalBlock) on top. Stable for the prepared lifetime so the host's PDC
    // stays correct and dry/wet stay phase-aligned.
    int latency_samples() const override { return latency_samples_; }

    /// True when the live audio path is actually the GPU engine (Engine=GPU was
    /// requested AND a GPU device was available AND the transport prepared). If
    /// the GPU was requested but unavailable, this is false — the processor fell
    /// back to the CPU engine. Read after prepare().
    bool gpu_engine_active() const { return gpu_engine_active_; }

    /// The live GPU compute backend ("Metal"/"D3D12"/"Vulkan") when the GPU
    /// engine is active, else "". UI/main-thread only.
    std::string gpu_backend() const {
        return gpu_engine_active_ && gpu_node_ ? gpu_node_->backend() : std::string();
    }

    /// Live {GPU blocks produced, blocks missed (CPU-filled)} so the UI can show
    /// whether the GPU is actually carrying the work or mostly falling back.
    std::pair<std::uint64_t, std::uint64_t> gpu_block_stats() const {
        if (!gpu_engine_active_) return {0, 0};
        const auto s = gpu_transport_.stats();
        return {s.produced_blocks, s.miss_blocks};
    }

    void prepare(const format::PrepareContext& ctx) override {
        stop_worker();
        // Tear down any previous GPU engine before re-selecting (transport first,
        // it holds a pointer into the node).
        gpu_transport_.release();
        gpu_node_.reset();
        gpu_engine_active_ = false;

        sample_rate_ = ctx.sample_rate;
        const std::size_t max_block =
            ctx.max_buffer_size > 0 ? static_cast<std::size_t>(ctx.max_buffer_size) : 512;

        // FIFO scratch sized for the worst-case host block plus headroom for the
        // primed output latency and an in-flight internal block.
        const std::size_t cap = max_block + 4 * kInternalBlock;
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            in_buf_[ch].assign(cap, 0.0f);   in_len_[ch] = 0;
            out_buf_[ch].assign(cap, 0.0f);  out_len_[ch] = kInternalBlock;  // primed: B zeros
            // Drop any IR a previous lifecycle's worker staged but the audio
            // thread never consumed (it was built at the old block size).
            while (swapper_[ch].try_consume()) { /* freed here, off the audio thread */ }
        }
        wet_.assign(kInternalBlock, 0.0f);

        rebuild_ir_inline(current_size());   // first IR loaded synchronously (CPU)

        // Choose the engine ONCE (not per-block) so the reported latency is
        // stable. The live GPU path is opt-in; if it can't initialize we fall
        // back to the CPU engine so the plugin always produces audio.
        if (state().get_value(kEngine) >= 0.5f)
            try_enable_gpu_engine();

        // Total latency = re-block FIFO (kInternalBlock) + (GPU only) the
        // transport's fixed delay. The dry path is delayed to match so dry/wet
        // stay phase-coherent under either engine.
        latency_samples_ = static_cast<int>(kInternalBlock);
        if (gpu_engine_active_)
            latency_samples_ += static_cast<int>(gpu_transport_.latency_samples());
        const std::size_t dry_delay = static_cast<std::size_t>(latency_samples_);
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            dry_ring_[ch].assign(dry_delay, 0.0f);
            dry_pos_[ch] = 0;
        }

        start_worker();
    }

    void release() override {
        stop_worker();
        gpu_transport_.release();   // transport before node (it points at the node)
        gpu_node_.reset();
        gpu_engine_active_ = false;
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

        // Fill the per-channel wet output FIFO via the active engine. Both engines
        // re-block the host stream into fixed kInternalBlock chunks; only the
        // convolution itself (CPU PartitionedConvolver vs GPU transport) differs.
        if (gpu_engine_active_)
            fill_wet_gpu(input, n);
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
                std::memcpy(out_buf_[ch].data() + out_len_[ch], wet_.data(),
                            kInternalBlock * sizeof(float));
                out_len_[ch] += kInternalBlock;
            }
        }
    }

    // GPU engine: same fixed re-blocking, but each B-block is processed as ONE
    // stereo block through the GPU transport (RT-safe by contract — the GPU FFT
    // runs on the transport's non-RT worker; on a worker miss the node's
    // PassthroughDry policy fills the block). Both channels advance in lockstep
    // (the same n is appended every call), so in_len_[0] gates the drain.
    void fill_wet_gpu(const audio::BufferView<const float>& input, std::size_t n) {
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
            gpu_transport_.process(iv, ov, static_cast<uint32_t>(kInternalBlock));
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

    // Build + start the GPU engine. On any failure (no GPU device, transport
    // prepare rejected) leaves gpu_engine_active_ == false so process() uses CPU.
    void try_enable_gpu_engine() {
        auto ir = make_reverb_ir(ir_length_for(current_size()));
        gpu_node_ = std::make_unique<gpu_audio::GpuConvolver>(
            static_cast<uint32_t>(kChannels), static_cast<uint32_t>(kInternalBlock),
            static_cast<uint32_t>(sample_rate_), std::move(ir));
        bool ok = gpu_node_->prepare() && gpu_node_->gpu_available();
        if (ok) {
            gpu_audio::GpuAudioTransport::Config cfg;
            cfg.ring_blocks = 8;
            cfg.run_worker_thread = true;
            ok = gpu_transport_.prepare(gpu_node_.get(), cfg);
        }
        if (ok) {
            for (std::size_t ch = 0; ch < kChannels; ++ch)
                gpu_wet_[ch].assign(kInternalBlock, 0.0f);
            gpu_engine_active_ = true;
        } else {
            runtime::log_info(
                "SuperConvolver: GPU engine requested but unavailable "
                "(gpu_available={}); falling back to CPU convolution.",
                gpu_node_ ? gpu_node_->gpu_available() : false);
            gpu_transport_.release();
            gpu_node_.reset();
            gpu_engine_active_ = false;
        }
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

    // Background thread: rebuild + stage the IR whenever Size changes, and
    // reclaim displaced IRs. Never touches the audio thread's buffers. Builds
    // both channels' states from one IR and stages them back-to-back to keep the
    // L/R swap window minimal.
    void worker_loop() {
        using namespace std::chrono_literals;
        while (worker_run_.load(std::memory_order_acquire)) {
            const float want = requested_size_.load(std::memory_order_relaxed);
            if (want != worker_built_size_ && want > 0.0f) {
                auto ir = make_reverb_ir(ir_length_for(want));
                for (auto& sw : swapper_)
                    sw.stage_ir(ir.data(), ir.size(), kInternalBlock);
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

    // Re-blocking FIFO state (audio thread only).
    std::array<std::vector<float>, kChannels> in_buf_{};
    std::array<std::vector<float>, kChannels> out_buf_{};
    std::array<std::size_t, kChannels> in_len_{};
    std::array<std::size_t, kChannels> out_len_{};
    std::array<std::vector<float>, kChannels> dry_ring_{};   // dry delay, kInternalBlock
    std::array<std::size_t, kChannels> dry_pos_{};
    std::vector<float> wet_;                                  // internal-block scratch

    std::array<signal::PartitionedConvolver, kChannels> conv_{};
    std::array<signal::ConvolverIrSwapper, kChannels> swapper_{};

    // Optional GPU engine (default OFF). Chosen once at prepare(); the transport
    // points into the node, so node must outlive (and prepare before) it and the
    // transport must be released before the node. gpu_wet_ is the per-channel
    // B-sized scratch the transport writes each stereo block into.
    std::unique_ptr<gpu_audio::GpuConvolver> gpu_node_;
    gpu_audio::GpuAudioTransport gpu_transport_;
    std::array<std::vector<float>, kChannels> gpu_wet_{};
    bool gpu_engine_active_ = false;
    int latency_samples_ = static_cast<int>(kInternalBlock);

    // Worker / live-IR-swap state.
    std::thread worker_;
    std::atomic<bool> worker_run_{false};
    std::atomic<float> requested_size_{-1.0f};
    float worker_built_size_ = -1.0f;  // worker-thread-local

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

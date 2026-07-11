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
// path, null routes the CPU path. A retired stack is freed only once the audio
// thread is provably no longer using it: the audio thread publishes the transport
// it is about to use in a hazard pointer (gpu_in_use_) for the span of each block,
// and the worker defers freeing any retired stack whose transport matches. This
// is what makes a live Engine/Rooms switch safe even when an audio block runs
// long (e.g. an over-budget GPU block at a high sample rate). Reported latency is
// FIXED for the prepared lifetime (kInternalBlock
// plus the GPU transport's delay when a device exists) so the host's PDC stays
// correct and dry/wet stay phase-aligned under either engine. See
// gpu_engine_active().
//
// The convolution IR has two sources. By default it is the built-in synthetic
// reverb (make_reverb_ir). When the user loads an audio file (set_ir_path —
// WAV/AIFF/FLAC) that file becomes the BASE IR: it is summed to mono, resampled
// to the session sample rate, and unit-energy normalized off the audio thread.
// The Rooms control then generates N DECORRELATED VARIANTS of that one base IR
// (room 0 is the base verbatim; rooms 1..N-1 are per-room phase-scrambled,
// pre-delayed copies), so a single real space becomes a lush N-room cloud;
// Rooms=1 is the pure loaded IR. An unreadable/missing path falls back to the
// synthetic IR so the plugin always produces audio. The IR path persists through
// serialize_plugin_state(), so a host restores it with the project and presets.
//
// The native GPU front-end (live IR waveform + frequency display + controls,
// rendered through canvas/Skia/Dawn) is in super_convolver_ui.hpp.

#include <pulp/format/processor.hpp>
#include <pulp/signal/convolver.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/resampler.hpp>
#include <pulp/audio/impulse_response.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/platform/file_dialog.hpp>
#include "source_display.hpp"
#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_multi_convolver.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include <memory>

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
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
    // Normalize to UNIT ENERGY so a fully-wet convolution sits at roughly the dry
    // signal's loudness — otherwise the wet is the sum of `length` (tens of
    // thousands of) scaled taps and swamps the dry, making the Mix knob useless
    // (anything above ~1% reads as fully wet). With sum(ir^2)=1 the output RMS for
    // broadband input ≈ the input RMS, so Mix becomes a perceptually sensible
    // dry/wet balance.
    double energy = 0.0;
    for (float v : ir) energy += static_cast<double>(v) * v;
    if (energy > 0.0) {
        const float g = static_cast<float>(1.0 / std::sqrt(energy));
        for (float& v : ir) v *= g;
    }
    return ir;
}

class SuperConvolverProcessor : public format::Processor {
public:
    // Fixed convolver block. Independent of the host's block size; the
    // re-blocking FIFO chunks the host stream into this. Power of two for the
    // radix-2 FFT. 256 samples ≈ 5.3 ms latency at 48 kHz — fine for a reverb.
    // 512-sample internal block: the GPU multi-room path amortizes its CPU<->GPU
    // round-trip over the whole block, so a larger block raises how many rooms
    // run within the real-time budget (the budget grows with the block too). The
    // re-block FIFO adds this much latency, reported for host PDC.
    static constexpr std::size_t kInternalBlock = 512;

    // Hard cap on a loaded IR's length (at the session rate). A multi-minute
    // file would otherwise drive an unbounded decode + resample + GPU FFT; we
    // truncate to this so the worst case stays bounded. 10 s is far longer than
    // any plausible convolution reverb tail.
    static constexpr double kMaxIrSeconds = 10.0;

    // Versioned plugin-state blob header: magic "SCv1" + 1-byte version.
    static constexpr char kStateMagic[4] = {'S', 'C', 'v', '1'};
    static constexpr std::uint8_t kStateVersion = 1;
    static constexpr std::size_t kStateHeaderSize = 5;

    ~SuperConvolverProcessor() override { stop_worker(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "SuperConvolver",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.superconvolver",
            .version = "1.1.1",
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

    /// Live GPU cost: {last, average} wall-clock microseconds the GPU worker
    /// spent per block (the real per-block cost of the GPU path, including the
    /// CPU↔GPU round-trip). {0,0} when the GPU engine isn't carrying the audio.
    /// UI/main-thread only (takes the worker mutex).
    std::pair<double, double> gpu_block_us() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        if (!gpu_engine_active() || !current_stack_ || !current_stack_->transport)
            return {0.0, 0.0};
        const auto s = current_stack_->transport->stats();
        return {s.last_block_us, s.avg_block_us};
    }

    /// One coherent snapshot of the live GPU engine for the UI status line,
    /// taken under a SINGLE lock so the fields can't disagree across a repaint
    /// (the granular accessors above are kept as per-field probes for tests).
    /// Also derives the real-time headroom: `budget_us` is how long one GPU
    /// block has to finish on THIS device + sample rate, and `rt_percent` is the
    /// measured average cost as a percentage of that budget — so 100 − rt_percent
    /// is the headroom left (roughly how much more work, e.g. more rooms, the GPU
    /// could still take in real time). UI/main-thread only.
    struct GpuStatus {
        bool active = false;
        std::string backend;
        int rooms = 0;
        bool multi = false;
        std::uint64_t blocks = 0;
        std::uint64_t misses = 0;
        double avg_us = 0.0;      // EWMA wall-clock per block (round-trip included)
        double budget_us = 0.0;   // real-time budget for one GPU block here
        double rt_percent = 0.0;  // avg_us / budget_us * 100 (lower = more headroom)
    };
    GpuStatus gpu_status() const {
        std::lock_guard<std::mutex> lock(stack_mutex_);
        GpuStatus g;
        g.active = gpu_engine_active();
        if (!g.active || !current_stack_) return g;
        g.backend = current_stack_->backend();
        g.rooms = current_stack_->rooms;
        g.multi = current_stack_->multi != nullptr;
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
        retired_stacks_.clear();
        gpu_in_use_.store(nullptr, std::memory_order_release);

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
            auto stack = build_gpu_stack(init_rooms, worker_base_ir_);
            if (stack) {
                device_available_ = true;
                gpu_extra_ = static_cast<std::size_t>(stack->transport->latency_samples());
                std::lock_guard<std::mutex> lock(stack_mutex_);
                current_stack_ = std::move(stack);
            }
        }
        gpu_built_rooms_ = init_rooms > 1 ? init_rooms : 1;
        gpu_base_dirty_ = false;

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
        retired_stacks_.clear();
        gpu_in_use_.store(nullptr, std::memory_order_release);
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

    /// Set the convolution IR source file (WAV/AIFF/FLAC). An empty path clears
    /// back to the built-in synthetic reverb IR. Main/UI thread only.
    ///
    /// The file is read, summed to mono, resampled to the session sample rate,
    /// and unit-energy normalized ENTIRELY off the audio thread by the background
    /// worker; the audio thread only ever receives the finished IR through the
    /// existing lock-free swap (CPU) / atomic GPU-stack publish. Setting the path
    /// is the trigger — the worker picks up the change on its next poll and
    /// rebuilds both engines. Persisted via serialize_plugin_state().
    void set_ir_path(std::string path) {
        {
            std::lock_guard<std::mutex> lock(ir_path_mutex_);
            if (path == active_ir_path_) return;
            active_ir_path_ = std::move(path);
        }
        ir_path_gen_.fetch_add(1, std::memory_order_release);
    }

    /// Force a (re)load of `path` even when it equals the current path — the
    /// user re-picking the same file from the button (e.g. it changed on disk,
    /// or was missing and has since appeared). Always bumps the generation so
    /// the worker rebuilds. Main/UI thread only. Use this for explicit user
    /// "Load IR" actions; set_ir_path() (dedup) is for state restore.
    void load_ir_path(std::string path) {
        {
            std::lock_guard<std::mutex> lock(ir_path_mutex_);
            active_ir_path_ = std::move(path);
        }
        ir_path_gen_.fetch_add(1, std::memory_order_release);
    }

    /// The current IR source path ("" when using the built-in synthetic IR).
    /// UI/main-thread only (takes a mutex).
    std::string ir_path() const {
        std::lock_guard<std::mutex> lock(ir_path_mutex_);
        return active_ir_path_;
    }

    /// Monotonic counter advanced each time the IR source is (re)selected. The
    /// background worker rebuilds when it moves; exposed for tests / tooling.
    std::uint32_t ir_path_generation() const {
        return ir_path_gen_.load(std::memory_order_acquire);
    }

    /// UI / main thread only. Open a native file picker, load the chosen file as
    /// the Source impulse response, and return its display (clean name + facts).
    /// Returns nullopt if the user cancels or no dialog backend is registered.
    std::optional<superconvolver::SourceDisplay> browse_and_load_source() {
        static const std::vector<platform::FileFilter> kFilters = {
            {"Impulse Response", "wav;aiff;aif;flac"},
            {"All Files", "*"},
        };
        auto path = platform::FileDialog::open_file(
            "Load Impulse Response", kFilters, ir_path());
        if (!path || path->empty()) return std::nullopt;
        load_ir_path(*path);
        return superconvolver::derive_source_display(
            *path, audio::read_audio_file_info(*path));
    }

    /// The display for the currently-loaded Source (derived from the persisted
    /// path), or nullopt when the built-in synthetic IR is in use. Lets the
    /// editor restore the Source name/facts after a preset load. Main-thread only.
    std::optional<superconvolver::SourceDisplay> current_source_display() const {
        const std::string p = ir_path();
        if (p.empty()) return std::nullopt;
        return superconvolver::derive_source_display(
            p, audio::read_audio_file_info(p));
    }

    /// Persist the IR file path alongside StateStore so a host restores the
    /// loaded impulse response with the project / preset. The path is opaque
    /// non-parameter state, exactly what serialize_plugin_state() is for.
    std::vector<uint8_t> serialize_plugin_state() const override {
        // Versioned blob: 4-byte magic + 1-byte version + UTF-8 path bytes. The
        // header lets future SuperConvolver state evolve while still reading the
        // legacy raw-path format written before versioning existed.
        const std::string path = ir_path();
        std::vector<uint8_t> out;
        out.reserve(kStateHeaderSize + path.size());
        out.insert(out.end(), kStateMagic, kStateMagic + 4);
        out.push_back(kStateVersion);
        out.insert(out.end(), path.begin(), path.end());
        return out;
    }

    /// Restore the IR file path. Reads the versioned blob (magic + version +
    /// path); a blob without the magic is treated as a legacy raw-path blob; an
    /// empty/short blob means "no loaded IR". Setting the path re-triggers the
    /// off-thread load. Always returns true (no fatal parse — unknown bytes just
    /// become the path, same fail-safe as a missing file).
    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        std::string path;
        if (data.size() >= kStateHeaderSize &&
            std::equal(kStateMagic, kStateMagic + 4,
                       reinterpret_cast<const char*>(data.data()))) {
            // Versioned: payload after the header is the path (any version ≥ 1,
            // which currently only ever appends a path — forward-tolerant).
            path.assign(reinterpret_cast<const char*>(data.data()) + kStateHeaderSize,
                        data.size() - kStateHeaderSize);
        } else if (!data.empty()) {
            // Legacy raw-path blob (pre-versioning).
            path.assign(reinterpret_cast<const char*>(data.data()), data.size());
        }
        set_ir_path(std::move(path));
        return true;
    }

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
                 const format::ProcessContext& ctx) override {
        const std::size_t n = output.num_samples();
        const std::size_t ch_count = output.num_channels();

        // Offline / faster-than-real-time render (e.g. a Logic bounce): the GPU
        // engine's async, wall-clock-paced worker can't keep up with a render that
        // runs faster than real time, so the GPU path must be driven synchronously
        // to capture its actual output instead of dropping it. The CPU path is
        // already synchronous/inline, so it is unaffected.
        const bool offline = ctx.is_offline();

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
        // Publish the transport we're about to use as a hazard pointer, so the
        // worker can't free its stack out from under this block. The retry closes
        // the load/publish gap: after the loop gpu_in_use_ == the gpu_active_ we
        // use, so a concurrent retire+free on the worker is guaranteed to observe
        // the hazard and defer the free. Bounded — the worker swaps gpu_active_ at
        // most once per poll (~5 ms), far less often than a block.
        gpu_audio::GpuAudioTransport* tp = gpu_active_.load(std::memory_order_acquire);
        do {
            gpu_in_use_.store(tp, std::memory_order_release);
            gpu_audio::GpuAudioTransport* again = gpu_active_.load(std::memory_order_acquire);
            if (again == tp) break;
            tp = again;
        } while (true);

        if (tp)
            fill_wet_gpu(tp, input, n, offline);
        else
            fill_wet_cpu(input, n);

        // Release the hazard: the worker may now reclaim the stack we used.
        gpu_in_use_.store(nullptr, std::memory_order_release);

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
                      const audio::BufferView<const float>& input, std::size_t n,
                      bool offline = false) {
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
            // Offline render drives the GPU node synchronously (blocking readback
            // is fine — no RT deadline) so the bounce captures the real GPU reverb;
            // realtime stays on the async, lock-free path.
            if (offline)
                tp->process_offline(iv, ov, static_cast<uint32_t>(kInternalBlock));
            else
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
    std::unique_ptr<GpuStack> build_gpu_stack(int rooms, const std::vector<float>& base_ir) {
        auto stack = std::make_unique<GpuStack>();
        stack->rooms = 1;  // single-IR default; the multi branch sets the real count
        const std::size_t len = base_ir.empty() ? 1u : base_ir.size();
        gpu_audio::GpuAudioNode* node = nullptr;

        if (rooms > 1) {
            // The resident IR spectra are fft_size*2*num_ir floats; past the GPU's
            // per-binding storage limit the multi-convolver can't be built. Rather
            // than silently revert to CPU when Rooms is raised past what fits at
            // this IR length, CLAMP to the largest room count the device holds and
            // stay on the GPU. Estimate the fit (128 MiB validated on Metal), then
            // step down if a build still doesn't take.
            uint32_t fft = 1;
            while (fft < static_cast<uint32_t>(kInternalBlock) + static_cast<uint32_t>(len))
                fft <<= 1;
            const std::uint64_t per_room = static_cast<std::uint64_t>(fft) * 2u * sizeof(float);
            int max_fit = per_room > 0 ? static_cast<int>((128ull * 1024 * 1024) / per_room) - 1 : 1;
            if (max_fit < 1) max_fit = 1;
            int try_rooms = rooms < max_fit ? rooms : max_fit;
            while (try_rooms > 1) {
                std::vector<std::vector<float>> irs;
                irs.reserve(static_cast<std::size_t>(try_rooms));
                // Room 0 is the base IR verbatim; rooms 1..N-1 are deterministic
                // decorrelated variants of it (see make_room_variant).
                for (int k = 0; k < try_rooms; ++k)
                    irs.push_back(make_room_variant(base_ir, k));
                auto m = std::make_unique<gpu_audio::GpuMultiConvolver>(
                    static_cast<uint32_t>(kInternalBlock),
                    static_cast<uint32_t>(sample_rate_), std::move(irs));
                if (m->prepare() && m->gpu_available()) {
                    stack->multi = std::move(m);
                    stack->rooms = try_rooms;   // the ACTUAL (possibly clamped) count
                    node = stack->multi.get();
                    break;
                }
                try_rooms = try_rooms > 2 ? (try_rooms / 2 < 2 ? 2 : try_rooms / 2) : 1;
            }
        }
        if (!node) {
            std::vector<float> ir = base_ir.empty() ? std::vector<float>{0.0f} : base_ir;
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
    // Worker-thread only. Free retired GPU stacks the audio thread is provably no
    // longer using (its hazard pointer doesn't match), so a stack is never freed
    // while fill_wet_gpu still holds its transport. Stacks still in use stay queued
    // and are reclaimed on a later call once the audio thread releases the hazard.
    // Each freed stack's destructor joins the transport's worker thread, so we free
    // OUTSIDE stack_mutex_ to keep the UI accessors (which take it) from stalling.
    void reclaim_retired() {
        gpu_audio::GpuAudioTransport* in_use = gpu_in_use_.load(std::memory_order_acquire);
        std::vector<std::unique_ptr<GpuStack>> to_free;
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            for (auto& s : retired_stacks_) {
                if (s && (in_use == nullptr || s->transport.get() != in_use))
                    to_free.push_back(std::move(s));
            }
            retired_stacks_.erase(
                std::remove(retired_stacks_.begin(), retired_stacks_.end(), nullptr),
                retired_stacks_.end());
        }
        to_free.clear();  // GpuStack destructors run here, outside the lock
    }

    void rebuild_gpu_stack(int rooms, const std::vector<float>& base_ir) {
        // Stop the audio thread from using the current stack before retiring it.
        gpu_active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            if (current_stack_) retired_stacks_.push_back(std::move(current_stack_));
        }
        // Free retired stacks the audio thread is provably no longer using.
        reclaim_retired();

        auto fresh = build_gpu_stack(rooms, base_ir);
        gpu_built_rooms_ = rooms > 1 ? rooms : 1;
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

    // prepare-time: build the base IR (loaded file or synthetic) and load it into
    // both CPU channels synchronously. Caches it in worker_base_ir_ so the
    // prepare-time GPU pre-build and the worker share one base.
    void rebuild_ir_inline(float seconds) {
        worker_base_ir_ = build_base_ir(seconds);
        for (auto& c : conv_)
            c.load_ir(worker_base_ir_.data(), worker_base_ir_.size(), kInternalBlock);
        publish_display_ir(worker_base_ir_);
        worker_base_size_ = seconds;
        worker_base_gen_ = ir_path_gen_.load(std::memory_order_acquire);
        requested_size_.store(seconds, std::memory_order_relaxed);
    }

    // True when the base IR must be rebuilt: the loaded-file generation changed,
    // or (synthetic fallback only) the Size knob moved. Worker-thread only.
    bool base_needs_rebuild(std::uint32_t want_gen, float want_size) const {
        if (want_gen != worker_base_gen_) return true;
        if (!worker_has_file_ && want_size > 0.0f && want_size != worker_base_size_)
            return true;
        return false;
    }

    // Produce the base IR for the current source. With a readable IR path set the
    // base is the loaded file (summed to mono, resampled to the session SR,
    // unit-energy normalized); otherwise it is the synthetic reverb at `seconds`.
    // Sets worker_has_file_. Worker / prepare thread only (file IO + alloc).
    std::vector<float> build_base_ir(float seconds) {
        const std::string path = ir_path();
        if (!path.empty()) {
            // Any exception during decode/resample (e.g. bad_alloc on a huge or
            // corrupt file) must never escape onto the worker or prepare thread —
            // fall back to the synthetic IR rather than drop audio or crash.
            std::optional<std::vector<float>> loaded;
            try {
                loaded = load_ir_file(path);
            } catch (...) {
                loaded = std::nullopt;
            }
            if (loaded && !loaded->empty()) {
                worker_has_file_ = true;
                return std::move(*loaded);
            }
            runtime::log_warn(
                "SuperConvolver: IR file '{}' is unreadable, too large, or empty; "
                "falling back to the built-in synthetic IR.", path);
        }
        worker_has_file_ = false;
        return make_reverb_ir(ir_length_for(seconds));
    }

    // Read an IR audio file → mono → resampled to the session SR → unit-energy
    // normalized. Returns nullopt on any failure so the caller falls back to the
    // synthetic IR. Worker / prepare thread only.
    std::optional<std::vector<float>> load_ir_file(const std::string& path) const {
        // Shared loader (decode → mono → resample → unit-energy normalize). The
        // kMaxIrSeconds cap keeps the decode + GPU FFT bounded; unit-energy norm
        // matches make_reverb_ir so Mix stays a sane dry/wet balance.
        return audio::read_impulse_response(
            path, sample_rate_,
            {.max_seconds = kMaxIrSeconds, .normalize_unit_energy = true});
    }

    // A deterministic decorrelated room variant of the base IR. Room 0 returns
    // the base verbatim (so Rooms=1 is the pure loaded IR). Rooms 1..N-1 get a
    // per-room pre-delay (1..5 ms, distinct) plus a cascade of Schroeder all-pass
    // sections with seeded random delays/gains: the all-pass cascade leaves the
    // magnitude response (the loaded space's tone + decay) intact while scrambling
    // phase, so N rooms read as N distinct positions in the same real space, and
    // the pre-delay spreads their onsets. Output keeps the base length and is
    // unit-energy normalized. Seeded by the room index → reproducible (golden).
    std::vector<float> make_room_variant(const std::vector<float>& base, int room) const {
        if (room <= 0 || base.empty()) return base;
        const std::size_t n = base.size();
        std::uint32_t s = 0x51C04711u + static_cast<std::uint32_t>(room) * 2654435761u;
        auto rnd01 = [&]() {
            s = s * 1664525u + 1013904223u;
            return static_cast<float>(s >> 8) / 16777216.0f;  // [0,1)
        };

        // Per-room pre-delay, 1..5 ms — bounded to a fraction of the IR length so
        // a very short loaded IR can't be pushed entirely past its own tail (which
        // would yield a zero-energy variant and silently attenuate the base in the
        // GPU multi-convolver's 1/sqrt(N) panning).
        std::size_t pre = static_cast<std::size_t>(
            (1.0f + 4.0f * rnd01()) * 0.001f * static_cast<float>(sample_rate_));
        if (pre > n / 4) pre = n / 4;
        std::vector<float> cur(n, 0.0f);
        for (std::size_t i = 0; i + pre < n; ++i) cur[i + pre] = base[i];

        // Schroeder all-pass cascade: y[i] = -g*x[i] + x[i-M] + g*y[i-M].
        constexpr int kSections = 3;
        std::vector<float> y(n, 0.0f);
        for (int sec = 0; sec < kSections; ++sec) {
            const std::size_t M = 32u + static_cast<std::size_t>(rnd01() * 400.0f);
            const float g = 0.4f + 0.3f * rnd01();
            std::fill(y.begin(), y.end(), 0.0f);
            for (std::size_t i = 0; i < n; ++i) {
                const float xM = (i >= M) ? cur[i - M] : 0.0f;
                const float yM = (i >= M) ? y[i - M] : 0.0f;
                y[i] = -g * cur[i] + xM + g * yM;
            }
            cur.swap(y);
        }

        // Re-normalize to unit energy (the all-pass + pre-delay are near energy-
        // preserving, but truncation to the base length can shave a little). If
        // the variant came out silent or non-finite (degenerate short IR), fall
        // back to the base so the room never reads as a zero-energy slot.
        double energy = 0.0;
        for (float v : cur) energy += static_cast<double>(v) * v;
        if (!std::isfinite(energy) || energy <= 0.0) return base;
        const float gain = static_cast<float>(1.0 / std::sqrt(energy));
        for (float& v : cur) v *= gain;
        return cur;
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
            const std::uint32_t want_gen = ir_path_gen_.load(std::memory_order_acquire);

            // Rebuild the base IR whenever its source changed: a new file loaded
            // (path generation moved), or — for the synthetic fallback only — the
            // Size knob moved. The loaded file IS the base (it keeps its own
            // length); Size only governs the synthetic IR. Re-stage the CPU IR and
            // flag the GPU stack for a rebuild against the new base. All file IO,
            // resampling, and allocation happens here, never on the audio thread.
            if (base_needs_rebuild(want_gen, want_size)) {
                worker_base_ir_ = build_base_ir(want_size);
                worker_base_gen_ = want_gen;
                worker_base_size_ = want_size;
                for (auto& sw : swapper_)
                    sw.stage_ir(worker_base_ir_.data(), worker_base_ir_.size(), kInternalBlock);
                publish_display_ir(worker_base_ir_);
                gpu_base_dirty_ = true;
            }

            // GPU stack management (only meaningful when a device exists).
            if (device_available_)
                service_gpu_stack(want_engine, want_rooms);

            for (auto& sw : swapper_) sw.drain_old();
            // Free any retired stack the audio thread has since released (a stack
            // retired during a slow in-flight block is reclaimed here once the
            // hazard clears, without waiting for the next rebuild).
            if (!retired_stacks_.empty()) reclaim_retired();
            std::this_thread::sleep_for(5ms);
        }
        // Final drain so nothing leaks once the audio thread has stopped.
        for (auto& sw : swapper_) sw.drain_old();
    }

    // Worker-thread only. Reconcile the published GPU path with the requested
    // Engine/Rooms/Size: rebuild on a Rooms/Size change, republish the pre-built
    // stack on a CPU->GPU toggle (instant), and unpublish on a GPU->CPU toggle
    // while KEEPING the stack built so the next switch back is instant too.
    void service_gpu_stack(int want_engine, int want_rooms) {
        const int rooms = want_rooms > 1 ? want_rooms : 1;
        if (want_engine == 1) {
            const bool config_changed =
                !current_stack_ || gpu_built_rooms_ != rooms || gpu_base_dirty_;
            if (config_changed) {
                rebuild_gpu_stack(rooms, worker_base_ir_);
                gpu_base_dirty_ = false;
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
    // current_stack_ (the built stack) and retired_stacks_ (previous stacks pending
    // free). The audio thread routes the GPU path solely through gpu_active_ (an
    // atomic pointer into current_stack_'s transport, or null for the CPU path).
    // stack_mutex_ guards current_stack_ for the UI accessors vs the worker — the
    // audio thread NEVER takes it. gpu_wet_ is the per-channel B-sized scratch the
    // transport writes each stereo block.
    //
    // Reclamation is hazard-pointer protected: a retired stack is freed only once
    // the audio thread is provably no longer using it. The audio thread publishes
    // the transport it is about to use in gpu_in_use_ for the duration of a block;
    // the worker defers freeing any retired stack whose transport matches. Freeing
    // "one rebuild later" alone is unsafe — it counts worker polls, not audio
    // blocks, so a slow audio block (e.g. an over-budget GPU block at 96 kHz) can
    // still hold a transport the worker would otherwise free, a use-after-free.
    std::unique_ptr<GpuStack> current_stack_;
    std::vector<std::unique_ptr<GpuStack>> retired_stacks_;
    std::atomic<gpu_audio::GpuAudioTransport*> gpu_active_{nullptr};
    std::atomic<gpu_audio::GpuAudioTransport*> gpu_in_use_{nullptr};  // audio-thread hazard ptr
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
    int gpu_built_rooms_ = 0;          // worker-thread-local (current_stack_ config)

    // IR source. The path is opaque, persisted state (set on the UI/main thread,
    // read on the worker / prepare); ir_path_gen_ is the lock-free trigger the
    // worker polls so a new path rebuilds the base IR off the audio thread. The
    // worker_base_* fields are worker-thread-local: the current base IR plus the
    // (generation, size, has-file) tuple that produced it, so the worker only
    // rebuilds when the source actually changed. gpu_base_dirty_ flags that the
    // GPU stack must rebuild against a freshly produced base IR.
    mutable std::mutex ir_path_mutex_;
    std::string active_ir_path_;
    std::atomic<std::uint32_t> ir_path_gen_{0};
    std::vector<float> worker_base_ir_;             // worker/prepare-thread-local
    std::uint32_t worker_base_gen_ = 0;
    float worker_base_size_ = -1.0f;
    bool worker_has_file_ = false;
    bool gpu_base_dirty_ = false;

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

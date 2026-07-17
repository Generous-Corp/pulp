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
// The convolution IR has three sources. By default it is a built-in synthetic
// reverb (make_builtin_ir — a small family of named, deterministic rooms
// selectable with set_built_in_ir; id 0 is the default). Decoded PCM handed in
// with set_ir_pcm() — the source a host without a codec uses (in a browser the
// page decodes with AudioContext.decodeAudioData and posts raw floats) — takes
// precedence. On a host with a filesystem, loading an audio file (set_ir_path —
// WAV/AIFF/FLAC) makes that file the BASE IR. A file or PCM base is summed to
// mono, resampled to the session sample rate, and unit-energy normalized off the
// audio thread. The Rooms control then generates N DECORRELATED VARIANTS of that
// one base IR (room 0 is the base verbatim; rooms 1..N-1 are per-room
// phase-scrambled, pre-delayed copies), so a single real space becomes a lush
// N-room cloud; Rooms=1 is the pure base IR. An unreadable/missing path falls
// back to the synthetic IR so the plugin always produces audio. The IR source
// persists through serialize_plugin_state(), so a host restores it with the
// project and presets.
//
// The IR rebuild (decode, resample, window, FFT plan) runs off the audio thread.
// On a host with threads a background worker owns it and runs it as one blocking
// pass. A host without threads (a wasm build — neither web lane can spawn a
// thread) compiles the worker out, and the only non-render context it has is the
// host-driven on_non_realtime_tick(), which an AudioWorklet dispatches BETWEEN
// RENDER QUANTA ON THE RENDER THREAD. A blocking rebuild there is a dropout (it
// measured 15.0 ms against a 2.667 ms quantum budget), so that lane runs the
// rebuild TIME-SLICED instead — superconvolver::SlicedIrRebuild, a bounded,
// resumable job the tick pumps a fixed budget of work into per quantum while the
// OLD IR keeps producing audio. Either way the audio thread only ever picks the
// finished IR up through the lock-free signal::ConvolverIrSwapper, so process()
// stays allocation-free — and the pickup CROSSFADES (PartitionedConvolver's
// set_crossfade) so a Size change is audibly continuous rather than a hard cut.
//
// The native GPU front-end (live IR waveform + frequency display + controls,
// rendered through canvas/Skia/Dawn) is in super_convolver_ui.hpp. The GPU
// engine, the file loader, and the editor are all native-only surfaces: they
// compile out under PULP_WASM / PULP_HEADLESS, leaving the CPU convolution
// engine, the parameters, and the state format byte-identical across lanes.
//
// A THIRD engine exists for the browser (PULP_SC_WEB_GPU, below): the same
// fixed-block convolution, but the FFT runs as a WebGPU compute shader in a
// DedicatedWorker outside the module, reached through the single `pulp_gpu_xfer`
// import. It is a CAPABILITY demonstration, not a performance claim — a competent
// CPU convolver matches or beats the GPU at every musical setting measured — so
// the CPU engine remains the default and the always-available fallback there too.

#include <pulp/format/processor.hpp>
#include <pulp/signal/convolver.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/resampler.hpp>
#include <pulp/audio/impulse_response.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/runtime/log.hpp>

#include "sliced_ir_rebuild.hpp"

#if !defined(PULP_WASM)
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/platform/file_dialog.hpp>
#include "source_display.hpp"
#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_multi_convolver.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <thread>
#endif

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
#include <utility>
#include <vector>

// The browser GPU-audio engine. Compiled ONLY into the dedicated web GPU plugin
// variant (a wasm build with -DPULP_WEB_GPU_AUDIO); the shipped CPU-only WAM and
// WebCLAP modules must stay byte-identical, and a module's parameter list is
// fetched once when it is mounted, so an Engine toggle cannot appear
// conditionally inside one module — it needs its own artifact.
#if defined(PULP_WASM) && defined(PULP_WEB_GPU_AUDIO)
#define PULP_SC_WEB_GPU 1
#endif

#if defined(PULP_SC_WEB_GPU)
/// The whole seam between the plugin and the browser's GPU worker. Hands one
/// interleaved-by-plane stereo block (kChannels planes of `frames` samples) to
/// the SharedArrayBuffer ring the WebGPU worker drains, and pops one wet block
/// back. Returns 1 when a GPU wet block was delivered, 0 on a MISS (the worker
/// has not produced this block in time — normal, not exceptional: a backgrounded
/// tab throttles the worker while the AudioWorklet keeps running).
///
/// Every block carries its index across this seam (the ring stamps it on the JS
/// side), so the block popped here for input block n is ALWAYS the GPU wet of
/// input block n-L — never a wet that has drifted because an earlier block was
/// dropped or expired. That is what makes the CPU safety net, delayed by the same
/// L blocks, a sample-for-sample substitute for a missed GPU block.
///
/// `channels` is part of the ABI because the host writes channels * frames floats
/// back into `out_planar`: the host validates BOTH dimensions against its ring and
/// refuses (returns 0) on a mismatch, rather than overrunning this module's
/// staging buffer from the audio thread.
///
/// It must be called on EVERY internal block while the plugin is running the web
/// build — including while Engine=CPU. The push/pop pair is what advances the
/// shared block timeline; skipping it while on CPU would leave the ring holding
/// pre-flip wets, which the next flip to GPU would emit as if they were current.
///
/// The import attributes are wasm-target-only (native clang warns and ignores
/// them); a native build — which is how the whole engine is unit-tested — links
/// its own definition instead.
#if defined(__wasm__)
__attribute__((import_module("env"), import_name("pulp_gpu_xfer")))
#endif
extern "C" int pulp_gpu_xfer(const float* in_planar, float* out_planar, int frames,
                             int channels);
#endif  // PULP_SC_WEB_GPU

namespace pulp::view { class View; }

// The parameter ids, the spectrum bus and GpuStatus, plus the editor's host interface, now
// live in super_convolver_ui_host.hpp — so the EDITOR can be built without this header (its
// DSP), which is what lets the same editor run natively and on the web. This processor both
// owns the DSP and IS a SuperConvolverUiHost (its four host calls are exactly these members),
// so implementing the interface costs nothing and keeps one editor for both worlds.
#include "super_convolver_ui_host.hpp"

namespace pulp::examples {

/// Normalize `ir` so its maximum magnitude frequency response (max_f |H(f)|) is
/// `target` — i.e. 0 dB of steady-state gain at `target == 1`. This BOUNDS the
/// output peak: a full-scale sinusoid at ANY frequency comes out at ≤ target, so
/// at unity output gain the wet can never exceed full scale (and any convex
/// dry/wet Mix stays ≤ full scale too). Unit-ENERGY normalization does NOT give
/// this guarantee — it fixes the average (RMS) gain to 0 dB but leaves the
/// response's resonant peaks free to grow, and a longer / denser IR has taller
/// peaks (empirically ~+6 dB), so an energy-normalized synthetic reverb clips the
/// DAC as the Size knob is raised even though its RMS gain is unchanged. Peak-
/// response normalization keeps the clip headroom identical across every Size.
/// Off-thread work (one FFT); never call from the audio thread.
inline void normalize_peak_response(std::vector<float>& ir, float target = 1.0f) {
    if (ir.size() < 2) {
        // Too short for a meaningful spectrum — fall back to unit energy.
        double energy = 0.0;
        for (float v : ir) energy += static_cast<double>(v) * v;
        if (energy > 0.0) {
            const float g = static_cast<float>(target / std::sqrt(energy));
            for (float& v : ir) v *= g;
        }
        return;
    }
    std::size_t nfft = 1;
    while (nfft < ir.size()) nfft <<= 1;
    signal::Fft fft(static_cast<int>(nfft));
    std::vector<float> time(nfft, 0.0f);
    std::vector<std::complex<float>> freq(nfft);
    std::copy(ir.begin(), ir.end(), time.begin());
    fft.forward_real(time.data(), freq.data());
    float peak = 0.0f;
    for (std::size_t k = 0; k <= nfft / 2; ++k)
        peak = std::max(peak, std::abs(freq[k]));
    if (peak > 0.0f) {
        const float g = target / peak;
        for (float& v : ir) v *= g;
    }
}

/// Build a deterministic, plausible reverb IR: exponentially-decaying white
/// noise (seeded LCG → reproducible, so a golden test can rebuild it). `length`
/// samples, decaying by `decay_norm` nepers over the whole IR (6 ≈ -52 dB).
/// `density` < 1 gates taps off at random so the tail reads as discrete
/// reflections rather than a solid noise wash; at density 1 no gating draw is
/// taken, so the LCG stream (and therefore the IR) is exactly the classic one.
/// The first sample is 1 so a delta-ish direct onset is present.
inline std::vector<float> make_reverb_ir_shaped(std::size_t length, float decay_norm,
                                                float density, std::uint32_t seed) {
    std::vector<float> ir(length, 0.0f);
    if (length == 0) return ir;
    // The generator body is duplicated, deliberately and identically, in
    // superconvolver::SlicedIrRebuild::generate() so the web lane can produce the
    // SAME samples a chunk at a time (the LCG is sequential, so a resumable
    // version must carry its state rather than re-derive it). Any change to the
    // recurrence must be made in both, and the "sliced rebuild converges to the
    // one-shot IR" test is what holds them together.
    std::uint32_t s = seed;
    const float decay = decay_norm / static_cast<float>(length);
    for (std::size_t i = 0; i < length; ++i) {
        s = s * 1664525u + 1013904223u;
        const float white = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;  // [-1,1)
        float v = white * std::exp(-decay * static_cast<float>(i));
        if (density < 1.0f) {
            s = s * 1664525u + 1013904223u;
            const float u = static_cast<float>(s >> 8) / 16777216.0f;  // [0,1)
            if (u >= density) v = 0.0f;
        }
        ir[i] = v;
    }
    ir[0] = 1.0f;  // direct onset
    // Normalize to 0 dB MAX gain (peak magnitude response == 1), not unit energy.
    // Unit energy keeps the wet at a sane loudness but lets the response's peaks
    // exceed unity, so a fully-wet, full-scale input clips the DAC — and worse as
    // Size grows (taller peaks). Peak-response normalization keeps Mix a sensible
    // dry/wet balance (broadband wet RMS still lands within a few dB of the input)
    // AND guarantees no clipping at any Size. See normalize_peak_response.
    normalize_peak_response(ir, 1.0f);
    return ir;
}

/// The classic synthetic reverb IR (dense, ~-52 dB over the tail). Kept as the
/// zero-argument shape every existing preset and golden test expects.
inline std::vector<float> make_reverb_ir(std::size_t length, std::uint32_t seed = 0x51C04711u) {
    return make_reverb_ir_shaped(length, 6.0f, 1.0f, seed);
}

/// The built-in IR family: a handful of named synthetic spaces the plugin can
/// switch between with NO file I/O at all (the only IR source a browser build has
/// out of the box). Each is a distinct decay/density/seed of the same
/// deterministic generator, so they are reproducible and cost nothing to ship.
struct BuiltInIr {
    const char* name;
    float decay_norm;   // nepers of decay over the whole IR (larger = shorter tail)
    float density;      // 1 = solid noise tail; < 1 = discrete reflections
    std::uint32_t seed;
};
inline constexpr std::array<BuiltInIr, 3> kBuiltInIrs = {{
    {"Room",  6.0f, 1.00f, 0x51C04711u},   // id 0 — the classic default
    {"Hall",  2.5f, 1.00f, 0x2A1F63C7u},   // longer, smoother tail
    {"Plate", 9.0f, 0.35f, 0x7B41D9E5u},   // fast, sparse, brighter
}};
inline constexpr std::uint8_t kBuiltInIrCount =
    static_cast<std::uint8_t>(kBuiltInIrs.size());

/// Name of built-in IR `id` ("" for an unknown id).
inline const char* built_in_ir_name(std::uint8_t id) {
    return id < kBuiltInIrCount ? kBuiltInIrs[id].name : "";
}

/// Build built-in IR `id` at `length` samples. An unknown id falls back to id 0,
/// so a state blob from a newer build still produces audio.
inline std::vector<float> make_builtin_ir(std::uint8_t id, std::size_t length) {
    const BuiltInIr& b = kBuiltInIrs[id < kBuiltInIrCount ? id : 0];
    return make_reverb_ir_shaped(length, b.decay_norm, b.density, b.seed);
}

/// Window a loaded IR down to `target_len` samples with a short raised-cosine
/// fade-out over the last `fade_len` samples. This is what makes the Size knob
/// audibly do something with a LOADED file IR: shortening Size truncates the
/// loaded space's tail (shorter reverb) with a smooth fade so the cut never
/// clicks. A truncation with NO fade would leave a hard step at the cut sample
/// — an audible click on every tail — so the fade is not cosmetic. We do NOT
/// time-stretch (that would shift pitch); this is a pure length window.
///
/// If the IR is already ≤ target_len it is returned UNCHANGED — a real recorded
/// space is never zero-pad-extended into silence, so a Size longer than the file
/// is a no-op (the file is its own natural length). After a real cut the result
/// is re-normalized to 0 dB peak response so a shortened IR keeps the exact same
/// clip headroom as every other Size (see normalize_peak_response). Off-thread
/// only (one FFT in the normalize); never call from the audio thread.
inline std::vector<float> window_ir_to_length(const std::vector<float>& ir,
                                              std::size_t target_len,
                                              std::size_t fade_len) {
    if (target_len == 0 || ir.size() <= target_len) return ir;
    std::vector<float> out(ir.begin(), ir.begin() + static_cast<std::ptrdiff_t>(target_len));
    const std::size_t fade = fade_len < target_len ? fade_len : target_len;
    // Raised cosine from 1 (start of the fade) down to 0 (last sample).
    for (std::size_t i = 0; i < fade; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(fade);
        const float w = 0.5f * (1.0f + std::cos(3.14159265f * x));
        out[target_len - fade + i] *= w;
    }
    normalize_peak_response(out, 1.0f);
    return out;
}

class SuperConvolverProcessor : public format::Processor, public SuperConvolverUiHost {
public:
    // Fixed convolver block. Independent of the host's block size; the
    // re-blocking FIFO chunks the host stream into this. Power of two for the
    // radix-2 FFT. 256 samples ≈ 5.3 ms latency at 48 kHz — fine for a reverb.
    // 512-sample internal block: the GPU multi-room path amortizes its CPU<->GPU
    // round-trip over the whole block, so a larger block raises how many rooms
    // run within the real-time budget (the budget grows with the block too). The
    // re-block FIFO adds this much latency, reported for host PDC.
    static constexpr std::size_t kInternalBlock = 512;

    /// How long a live IR swap takes to BLEND. Public because it is part of the contract: for
    /// this long after a Size change or an IR upload the output is a MIX of the old IR and the
    /// new one, so anything measuring the IR must wait it out first.
    ///
    /// 150 ms, not 11 ms, because the incoming IR starts with an EMPTY delay line on the
    /// crossfade path — ConvolverIrSwapper skips the history carry there, since the displaced
    /// IR is still rendering from that history in parallel. A cold convolver has to build its
    /// output up from silence over the IR's own length, so a fade shorter than that build-up
    /// is heard as a HOLE in the sound rather than as a transition. Measured across a Size
    /// change: at 11 ms the envelope collapsed to 8.5% of its settled level; at 150 ms it
    /// stays above 65%. That hole, once per rebuild while dragging Size, was the crackle.
    static constexpr double kIrCrossfadeSeconds = 0.15;

    // Round-trip latency of the browser GPU engine, in internal blocks. Each pop
    // from the SharedArrayBuffer ring asks for the wet of the block this many
    // blocks ago, giving the WebGPU worker that long to turn a block around before
    // the audio thread needs it; the CPU safety net is delayed by the SAME amount
    // (gpu_extra_ / cpu_extra_ring_) so the two wets are interchangeable
    // sample-for-sample. 2 blocks = 1024 samples ≈ 21 ms at 48 kHz. WS-C/WS-E must
    // pass the same value as `gpuLatencyBlocks` in the worklet's processorOptions:
    // it is one contract, expressed in two languages.
    static constexpr std::size_t kWebGpuLatencyBlocks = 2;

    // Hard cap on a loaded IR's length (at the session rate). A multi-minute
    // file would otherwise drive an unbounded decode + resample + GPU FFT; we
    // truncate to this so the worst case stays bounded. 10 s is far longer than
    // any plausible convolution reverb tail.
    static constexpr double kMaxIrSeconds = 10.0;

    // Versioned plugin-state blob header: magic + 1-byte version, then a tagged
    // IR-source payload (see IrStateKind / serialize_plugin_state). v1 wrote a
    // raw filesystem path — meaningless off a filesystem — so v2 tags the source
    // and can carry a built-in id or the IR samples themselves. Both v1 and the
    // pre-versioning raw-path blob still deserialize.
    static constexpr char kStateMagic[4] = {'S', 'C', 'v', '2'};
    static constexpr char kStateMagicV1[4] = {'S', 'C', 'v', '1'};
    static constexpr std::uint8_t kStateVersion = 2;
    static constexpr std::size_t kStateHeaderSize = 5;

    /// The tagged IR source in a v2 state blob.
    enum class IrStateKind : std::uint8_t {
        Synthetic = 0,  // built-in id 0; no payload
        BuiltIn   = 1,  // u8 built-in id
        Pcm       = 2,  // u32 frames, u32 rate, frames × f32 (mono, LE)
        Path      = 3,  // UTF-8 filesystem path (native hosts only)
    };

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
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .kind = state::ParamKind::Toggle,
                             .value_labels = {"Off", "On"}});

        // ── GPU-engine parameters — NOT DECLARED ON THE WEB LANES. ─────────────
        // The whole GPU stack is `#if !defined(PULP_WASM)`, so in a WAM/WebCLAP
        // module these three controls could not do anything. Declaring them anyway
        // would put an Engine CPU/GPU toggle plus Rooms and Flow knobs on the
        // browser demo page (both the Pulp web UI and the shell's generated grid
        // build a control for every parameter the module reports) — a UI implying
        // a GPU audio engine that is not compiled into that module at all.
        //
        // State stays compatible in BOTH directions: StateStore::deserialize
        // ignores parameter ids it does not know, so a native preset that carries
        // Engine/Rooms/Flow loads fine in the browser (they are dropped, and the
        // browser runs the CPU engine, which is the desktop DEFAULT anyway), and a
        // browser-saved preset loads natively with those three left at their
        // defaults (Engine=CPU).
#if !defined(PULP_WASM)
        // Engine: 0 = CPU PartitionedConvolver (default), 1 = GPU runtime.
        // CPU is the default — the live GPU path is opt-in by governance.
        store.add_parameter({.id = kEngine, .name = "Engine", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .kind = state::ParamKind::Toggle,
                             .value_labels = {"CPU", "GPU"}});
        // Rooms: with Engine=GPU and Rooms>1, the input is convolved against
        // this many DISTINCT impulse responses ("rooms"), each panned to its own
        // stereo position, in ONE batched GPU submit per block — a dense
        // multi-room convolution reverb. The CPU would need Rooms× independent
        // convolvers to do the same thing; this is the structural difference the
        // batched GPU path exists to explore. (It is NOT a claim that the GPU is
        // faster: a competent CPU convolver matches or beats it at every musical
        // setting we have measured. Rooms/Flow are a capability + headroom demo.)
        // Rooms=1 keeps the single-IR GPU path. No effect when Engine=CPU.
        store.add_parameter({.id = kRooms, .name = "Rooms", .unit = "",
                             .range = {1.0f, 128.0f, 16.0f, 1.0f}});
        // Flow: 0 = static rooms (an ordinary reverb); >0 makes each room's pan
        // drift on its own rate per block — the batch becomes a MOVING field, the
        // time-varying case the batched GPU path is built for. GPU multi-room
        // only; default 0 leaves existing presets and the current sound unchanged.
        store.add_parameter({.id = kFlow, .name = "Flow", .unit = "%",
                             .range = {0.0f, 100.0f, 0.0f, 0.1f}});
#endif

#if defined(PULP_SC_WEB_GPU)
        // The browser GPU variant declares exactly TWO controls — and declares
        // them UNCONDITIONALLY, for the whole life of the module. A WAM host
        // fetches getParameterInfo() once when the module is mounted, so a
        // parameter list that grew or shrank later would be ABI-hostile; that is
        // why this is a separate artifact rather than a runtime branch inside the
        // CPU-only module.
        //
        // Engine: 0 = CPU PartitionedConvolver (default), 1 = the WebGPU compute
        // path in the worker. CPU is the default and stays a working fallback —
        // GPU is never a correctness dependency, and is NOT a speed claim.
        store.add_parameter({.id = kEngine, .name = "Engine", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .kind = state::ParamKind::Toggle,
                             .value_labels = {"CPU", "GPU"}});
        // GPU only (no CPU safety net): with Engine=GPU, a block the GPU worker
        // misses is normally covered by the latency-aligned CPU wet, so a miss is
        // inaudible — which also means the ear cannot tell whether the GPU is
        // really carrying the audio. Turning the net OFF makes a miss SILENT, so
        // the GPU is provably the only thing producing sound. It is a real user
        // affordance, not a hidden test hook: background the tab with it on and
        // the audio drops out, which is the honest demonstration.
        store.add_parameter({.id = kGpuOnly, .name = "GPU only", .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f},
                             .kind = state::ParamKind::Toggle,
                             .value_labels = {"Off", "On"}});
        // Rooms and Flow drive the living field — the editor reads them to set how
        // DENSE the field is (Rooms → emitter count) and how much it MOVES (Flow).
        // Both are declared on the web GPU variant so the editor shows the same five
        // controls as the native editor (Mix / Size / Gain / Rooms / Flow) and the
        // field responds to them. The audio side of Rooms/Flow — the batched
        // multi-room moving-pan reverb — stays native-only (the whole GpuMultiConvolver
        // stack is `#if !defined(PULP_WASM)`); on the web these two steer the field
        // VISUALIZATION, which is exactly what that field IS. Rooms>20 flips the engine
        // to GPU, mirroring the native "past the CPU's room cap → GPU" behaviour.
        store.add_parameter({.id = kRooms, .name = "Rooms", .unit = "",
                             .range = {1.0f, 128.0f, 16.0f, 1.0f}});
        store.add_parameter({.id = kFlow, .name = "Flow", .unit = "%",
                             .range = {0.0f, 100.0f, 0.0f, 0.1f}});
#endif

        // The WAM lane gets no GPU engine at all: it is served from GitHub Pages,
        // which cannot set COOP/COEP, so SharedArrayBuffer — the only way an
        // AudioWorklet can reach a WebGPU worker — throws there. That lane declares
        // only Mix/Size/Gain (see the shared block above), so its editor shows three
        // sliders and no engine chip.
    }

    // Total latency, FIXED at prepare() for the prepared lifetime: the re-block
    // FIFO adds kInternalBlock, plus — whenever a GPU device exists — the GPU
    // transport's fixed delay, reported for BOTH engines so a live Engine switch
    // never moves the reported latency (the host's PDC stays correct and dry/wet
    // stay phase-aligned regardless of which engine is currently active).
    int latency_samples() const override { return latency_samples_; }

#if !defined(PULP_WASM)

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
    // GpuStatus is declared in super_convolver_ui_host.hpp (same namespace) so the editor's
    // host interface can name it without pulling in the DSP.
    GpuStatus gpu_status() const override {
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

#elif defined(PULP_SC_WEB_GPU)

    /// True when the GPU engine is the requested live path. There is no device to
    /// probe from C++ here — the WebGPU device lives in the JS worker on the other
    /// side of pulp_gpu_xfer — so this reports the REQUEST. Whether the GPU is
    /// actually carrying the audio is what web_gpu_stats() answers.
    bool gpu_engine_active() const { return requested_engine_value() == 1; }

    /// Live {GPU blocks delivered, blocks missed} since prepare(), so the page can
    /// show whether the GPU is really carrying the work. A miss is NORMAL (a
    /// throttled tab), not an error. Any thread; relaxed counters.
    std::pair<std::uint64_t, std::uint64_t> web_gpu_stats() const {
        return {web_gpu_blocks_.load(std::memory_order_relaxed),
                web_gpu_misses_.load(std::memory_order_relaxed)};
    }

#else   // PULP_WASM: no GPU engine — the CPU convolution engine is the only path.

    bool gpu_engine_active() const { return false; }

#endif

#if defined(PULP_WASM)
    // gpu_status() satisfies SuperConvolverUiHost in the wasm build. The browser GPU engine's
    // live counters live in the DedicatedWorker and the SharedArrayBuffer, NOT in this
    // processor — so it answers honestly with an inactive status rather than inventing
    // numbers. The PAGE reads the real GPU status out of the SAB and drives the editor.
    GpuStatus gpu_status() const override { return {}; }
#endif

    /// Disable the background rebuild worker. A host with no thread facility (a
    /// wasm build; also the lane the native tests use to reproduce it) must then
    /// pump service_ir_rebuild() itself from a non-RT control thread. Call before
    /// prepare(); under PULP_WASM the worker does not exist and this is already
    /// the only mode.
    void set_background_worker_enabled(bool enabled) { worker_enabled_ = enabled; }

    /// The browser GPU round-trip pipeline depth, in internal blocks — how many
    /// blocks of latency the plugin budgets for the GPU worker's async submit →
    /// readback round trip. It sets gpu_extra_ (and the equal CPU-net delay) at
    /// prepare(), so it MUST be called before prepare() and MUST match the JS side's
    /// `latencyBlocks` (deadline budget + ring). Defaulting to kWebGpuLatencyBlocks;
    /// a deeper value trades latency for fewer misses on a jittery device. This is
    /// the runtime knob the adaptive/per-device depth logic drives (was a hardcoded
    /// compile-time constant — see planning/2026-07-16-adaptive-gpu-pipeline-depth.md).
    void set_web_gpu_latency_blocks(std::size_t blocks) {
        web_gpu_latency_blocks_ = blocks == 0 ? 1 : blocks;
    }
    std::size_t web_gpu_latency_blocks() const { return web_gpu_latency_blocks_; }

    /// One pass of the off-audio-thread IR/engine reconciliation: rebuild the base
    /// IR when its source or Size changed, stage it for the audio thread through
    /// the lock-free swapper, (native) reconcile the GPU stack, and reclaim what
    /// the audio thread has released. Non-RT — it decodes, resamples, and
    /// allocates. NEVER call it from process(); call it from the control thread
    /// (a host callback such as CLAP's on_main_thread, or a non-RT tick). With the
    /// background worker enabled (the native default) the worker calls this and a
    /// host must not call it concurrently.
    void service_ir_rebuild() {
        const float want_size = requested_size_.load(std::memory_order_relaxed);
        const std::uint32_t want_gen = ir_path_gen_.load(std::memory_order_acquire);

        // Rebuild the base IR whenever its source changed: a new source selected
        // (generation moved) or the Size knob moved. Size governs the synthetic
        // IR's length AND windows a loaded base's tail, so both cases rebuild. All
        // decoding, resampling, and allocation happens here, never on the audio
        // thread; the audio thread picks the result up through the swapper.
        if (base_needs_rebuild(want_gen, want_size)) {
            worker_base_ir_ = build_base_ir(want_size);
            worker_base_gen_ = want_gen;
            worker_base_size_ = want_size;
            for (auto& sw : swapper_)
                sw.stage_ir(worker_base_ir_.data(), worker_base_ir_.size(), kInternalBlock);
            publish_display_ir(worker_base_ir_);
#if !defined(PULP_WASM)
            gpu_base_dirty_ = true;
#endif
        }

#if !defined(PULP_WASM)
        service_gpu();
#endif

        for (auto& sw : swapper_) sw.drain_old();
    }

    /// Work items one non-realtime tick may spend on the IR rebuild.
    ///
    /// THE tuning knob for the web lanes. The tick runs between render quanta on
    /// the render thread, so the whole rebuild has to fit inside the slack of a
    /// 128-frame quantum — 2.667 ms at 48 kHz, of which the convolution itself
    /// already takes a share. 32768 items was chosen by measuring the slowest
    /// phase at this budget on the scalar radix-2 FFT path the browser actually
    /// runs (no vDSP): ~0.25 ms, i.e. under 10% of a quantum, against a
    /// pre-existing worst case of 15.0 ms. The cost of the choice is latency, not
    /// glitches: a full 4 s IR needs ~90 ticks (~240 ms of wall clock) to finish
    /// rebuilding, during which the OLD IR keeps playing. Raising the budget
    /// tightens that lag and eats quantum headroom; lowering it does the reverse.
    /// It is a constant, and deliberately NOT a function of IR length — that is
    /// the whole point.
    static constexpr std::size_t kRebuildSliceItems = 32768;

    /// Host-driven pump for hosts with no thread facility (the browser: a WAM
    /// module lives entirely inside an AudioWorklet, and WebCLAP's only control
    /// context is `on_main_thread`). The format adapter calls this from a
    /// NON-RENDER context after a parameter write / state restore / re-prepare;
    /// without it, `Size` and a restored IR source would never reach the audio
    /// thread on the web lanes, because nothing rebuilds from process() by
    /// design.
    ///
    /// "Non-render" is NOT "not the render thread": an AudioWorklet has no other
    /// thread, so this runs BETWEEN quanta ON the render thread. It therefore does
    /// a BOUNDED SLICE of the rebuild and returns — see kRebuildSliceItems and
    /// SlicedIrRebuild. A Size change that lands mid-rebuild SUPERSEDES the job in
    /// flight (start_rebuild_job restarts it) rather than queueing a second one.
    ///
    /// A no-op whenever the background worker owns the reconcile pass — the
    /// worker and the host must never run it concurrently.
    void on_non_realtime_tick() override {
#if !defined(PULP_WASM)
        if (worker_enabled_) return;   // the worker owns the reconcile pass
#endif
        // Publish the CURRENT parameter value before reconciling. `requested_size_`
        // is normally refreshed from the store by process() — the audio thread is
        // what sees host automation — so a control-thread tick that trusted it
        // would reconcile the PREVIOUS Size and lag one parameter change behind.
        // That is exactly what kept the Size knob looking inert on the web lanes
        // even once the tick was being called at all. Reading the store here is a
        // relaxed atomic load, and process() writing the same derived value
        // concurrently is benign (both write what the store already says).
        requested_size_.store(current_size(), std::memory_order_relaxed);
        service_ir_rebuild_sliced(kRebuildSliceItems);
    }

    /// RT-safe: two atomic loads and a compare against what the last reconcile
    /// pass built, plus "is a sliced job still in flight". CLAP (and therefore
    /// WebCLAP) only reaches the control thread by requesting a host callback from
    /// process(), so the adapter needs to know an IR rebuild is outstanding
    /// without doing any of the work — and a job that has STARTED but not finished
    /// is exactly as outstanding as one that has not started, or the CLAP host
    /// would stop calling the tick and strand it half-built. Reads the parameter
    /// store (not `requested_size_`) for the same reason the tick does: the store
    /// is the truth, the mirror lags.
    bool non_realtime_tick_pending() const override {
#if !defined(PULP_WASM)
        if (worker_enabled_) return false;   // the worker owns the reconcile
#endif
        if (rebuild_job_.active()) return true;
        return base_needs_rebuild(ir_path_gen_.load(std::memory_order_acquire),
                                  current_size());
    }

    /// One BOUNDED slice of the off-audio-thread reconcile: at most `budget` work
    /// items of IR synthesis / normalization / FFT re-partition, then return. The
    /// audio thread is untouched by any of it — when the job completes, the
    /// finished per-channel state is handed over through the lock-free
    /// signal::ConvolverIrSwapper exactly as the native worker's one-shot pass
    /// does, and the audio thread's only job stays try_swap_ir().
    ///
    /// Public so a test (and a host that wants a smaller/larger slice) can drive
    /// it directly. Never call it from process().
    void service_ir_rebuild_sliced(std::size_t budget) {
        const float want_size = requested_size_.load(std::memory_order_relaxed);
        const std::uint32_t want_gen = ir_path_gen_.load(std::memory_order_acquire);

        if (base_needs_rebuild(want_gen, want_size)) {
            // SUPERSEDE, don't queue: a knob drag delivers a stream of Size values,
            // and only the latest one is ever going to be heard. A job already
            // building exactly what is wanted keeps going; anything else restarts.
            if (!rebuild_job_.active() || job_size_ != want_size || job_gen_ != want_gen)
                start_rebuild_job(want_size, want_gen);
        } else if (!rebuild_job_.active()) {
            for (auto& sw : swapper_) sw.drain_old();
            return;
        }

        if (rebuild_job_.active() && rebuild_job_.step(budget)) {
            worker_base_ir_ = rebuild_job_.take_ir();
            worker_base_gen_ = job_gen_;
            worker_base_size_ = job_size_;
            for (std::size_t ch = 0; ch < kChannels; ++ch)
                swapper_[ch].stage_ir(rebuild_job_.take_state(ch));
            publish_display_ir(worker_base_ir_);
            rebuild_job_.cancel();
#if !defined(PULP_WASM)
            gpu_base_dirty_ = true;
#endif
        }

#if !defined(PULP_WASM)
        // The GPU half of the reconcile, exactly as the one-shot pass runs it. It
        // is unreachable on the lanes this sliced path exists for (a wasm build
        // compiles the whole GPU stack out), and is here so that turning the worker
        // off on a NATIVE host — which is how the tests reproduce the web lane —
        // does not silently lose the live Engine/Rooms switch.
        service_gpu();
#endif

        for (auto& sw : swapper_) sw.drain_old();
    }

    void prepare(const format::PrepareContext& ctx) override {
        // Re-seed the ramps from the live parameter values on the next block (see mix_z_).
        ramp_primed_ = false;
        stop_worker();
        rebuild_job_.cancel();   // worker stopped: nothing else can be mid-rebuild
        job_size_ = -1.0f;
#if !defined(PULP_WASM)
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
#endif

        sample_rate_ = ctx.sample_rate;
        const std::size_t max_block =
            ctx.max_buffer_size > 0 ? static_cast<std::size_t>(ctx.max_buffer_size) : 512;

        // FIFO scratch sized for the worst-case host block plus headroom for the
        // primed output latency and an in-flight internal block.
        const std::size_t cap = max_block + 4 * kInternalBlock;
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            in_buf_[ch].assign(cap, 0.0f);   in_len_[ch] = 0;
            out_buf_[ch].assign(cap, 0.0f);  out_len_[ch] = kInternalBlock;  // primed: B zeros
#if !defined(PULP_WASM)
            gpu_wet_[ch].assign(kInternalBlock, 0.0f);
#endif
            // Drop any IR a previous lifecycle's worker staged but the audio
            // thread never consumed (it was built at the old block size).
            while (swapper_[ch].try_consume()) { /* freed here, off the audio thread */ }
        }
        wet_.assign(kInternalBlock, 0.0f);

        // Drop any decoded-file cache from a previous prepare — it was resampled
        // to the OLD session rate, so a re-prepare must re-decode at ctx.sample_rate.
        worker_file_raw_.clear();
        worker_file_raw_gen_ = 0;

        // The first IR is built SYNCHRONOUSLY, here in prepare() — which is not the
        // audio thread — and loaded straight into both convolvers. That is what
        // makes the plugin ready to convolve on its very first block: it never
        // renders a dry block while it waits for an IR to appear, on any lane. (The
        // first `latency_samples()` of output are the re-block FIFO's priming
        // silence, which the host's PDC accounts for; after that, wet.)
        rebuild_ir_inline(current_size());   // first IR loaded synchronously (CPU)

        // Crossfade every LIVE IR swap, and give it TIME.
        //
        // This used to be kInternalBlock — 512 samples, ~11 ms — justified as "long enough to
        // be inaudible as a transition". That reasoning assumed the incoming IR starts from
        // the CARRIED input history. On the crossfade path it does NOT: ConvolverIrSwapper
        // deliberately skips the carry there, because the displaced IR is still rendering
        // from that history in parallel and carrying it would swap it out from under the
        // fade-out. So the new convolver starts COLD — its frequency-domain delay line is
        // empty, and its output has to build up from silence over the IR's own length.
        //
        // Fading a full old tail into a cold new one in 11 ms is therefore not a transition,
        // it is a DIP: the wet drops out and climbs back. Drag Size and you get one of those
        // per completed rebuild — which is exactly the crackle a user hears.
        //
        // The fix is to let the fade last long enough that the incoming IR has meaningfully
        // filled its delay line while the outgoing one is still carrying the sound. ~150 ms
        // is the usual figure for morphing convolution IRs and it is what this uses. The cost
        // is a doubled convolution for that window, which is a blip against a knob drag.
        const auto fade_len = static_cast<std::size_t>(sample_rate_ * kIrCrossfadeSeconds);
        for (auto& c : conv_) c.set_crossfade(std::max<std::size_t>(kInternalBlock, fade_len));

        const float init_size = current_size();
        gpu_extra_ = 0;
#if !defined(PULP_WASM)
        const int init_rooms = requested_rooms_value();
        // Learn the GPU transport latency by pre-building the stack once when a
        // device exists. Latency is rooms-independent, but pre-building at the
        // currently-requested Rooms/Size also makes the first live switch to GPU
        // instant. The stack stays built even when Engine=CPU; gpu_active_ is
        // only published when Engine=GPU.
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
#endif
#if defined(PULP_SC_WEB_GPU)
        // The browser GPU round-trip latency is a COMPILE-TIME constant, not a
        // probed device property: the worker's ring depth is fixed by the JS side
        // and the module cannot ask it. It is applied to BOTH engines through the
        // existing cpu_extra_ring_, so flipping Engine never moves the reported
        // latency (which would jump the host's PDC mid-stream).
        gpu_extra_ = web_gpu_latency_blocks_ * kInternalBlock;
        for (std::size_t ch = 0; ch < kChannels; ++ch)
            cpu_wet_[ch].assign(kInternalBlock, 0.0f);
        xfer_in_.assign(kChannels * kInternalBlock, 0.0f);
        xfer_out_.assign(kChannels * kInternalBlock, 0.0f);
        web_gpu_blocks_.store(0, std::memory_order_relaxed);
        web_gpu_misses_.store(0, std::memory_order_relaxed);
#endif

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

        requested_size_.store(init_size, std::memory_order_relaxed);
#if !defined(PULP_WASM)
        requested_engine_.store(requested_engine_value(), std::memory_order_relaxed);
        requested_rooms_.store(init_rooms, std::memory_order_relaxed);
        // Publish the pre-built stack immediately when Engine=GPU so the GPU path
        // carries audio from the first block.
        if (requested_engine_.load(std::memory_order_relaxed) == 1) {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            if (current_stack_) {
                gpu_active_.store(current_stack_->transport.get(),
                                  std::memory_order_release);
                gpu_engine_active_.store(true, std::memory_order_release);
            }
        }
#endif

        start_worker();
    }

    void release() override {
        stop_worker();   // worker stopped → audio thread stopped → safe to free
        rebuild_job_.cancel();
        job_size_ = -1.0f;
#if !defined(PULP_WASM)
        gpu_active_.store(nullptr, std::memory_order_release);
        gpu_engine_active_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(stack_mutex_);
            current_stack_.reset();
        }
        retired_stacks_.clear();
        gpu_in_use_.store(nullptr, std::memory_order_release);
#endif
        for (auto& c : conv_) c.reset();
    }

    /// A snapshot of the current impulse response for the GPU UI waveform /
    /// spectrogram. UI-thread only — never call from the audio thread.
    std::vector<float> impulse_response_snapshot() const override {
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
    /// and unit-energy normalized ENTIRELY off the audio thread by the IR rebuild
    /// (the background worker, or service_ir_rebuild() on a thread-less host); the
    /// audio thread only ever receives the finished IR through the existing
    /// lock-free swap (CPU) / atomic GPU-stack publish. Setting the path is the
    /// trigger — the rebuild picks the change up from the generation counter.
    /// Persisted via serialize_plugin_state().
    void set_ir_path(std::string path) {
        {
            std::lock_guard<std::mutex> lock(ir_path_mutex_);
            if (path == active_ir_path_ && pcm_.samples.empty() && built_in_id_ == 0)
                return;
            active_ir_path_ = std::move(path);
            pcm_ = IrPcm{};
            built_in_id_ = 0;
        }
        ir_path_gen_.fetch_add(1, std::memory_order_release);
    }

    /// Force a (re)load of `path` even when it equals the current path — the
    /// user re-picking the same file from the button (e.g. it changed on disk,
    /// or was missing and has since appeared). Always bumps the generation so
    /// the IR rebuilds. Main/UI thread only. Use this for explicit user
    /// "Load IR" actions; set_ir_path() (dedup) is for state restore.
    void load_ir_path(std::string path) override {
        {
            std::lock_guard<std::mutex> lock(ir_path_mutex_);
            active_ir_path_ = std::move(path);
            pcm_ = IrPcm{};
            built_in_id_ = 0;
        }
        ir_path_gen_.fetch_add(1, std::memory_order_release);
    }

    /// Set the IR from already-decoded PCM — the source a host with no codec uses
    /// (in a browser, AudioContext.decodeAudioData hands back exactly this). Takes
    /// precedence over the path and the built-in id. `samples` is PLANAR:
    /// `channels` consecutive planes of `frames` samples each. It is summed to
    /// mono, resampled from `src_rate` to the session rate, and normalized by the
    /// SAME pipeline the file loader uses (audio::read_impulse_response_pcm), and
    /// like every other source that work happens off the audio thread on the next
    /// rebuild — this call only copies the samples and bumps the trigger.
    /// `label` is what a UI shows for the source. Main/UI thread only.
    ///
    /// The copy kept here is BOUNDED AT THE SOURCE: the planes are summed to mono
    /// (the convolver runs a mono IR anyway) and truncated to kMaxIrSeconds of
    /// source time. Without that, dropping a 3-minute WAV in would park tens of MB
    /// on the processor and — because serialize_plugin_state() writes the retained
    /// PCM inline — emit a state blob of the same size into every host project,
    /// even though the IR actually used is capped at kMaxIrSeconds downstream. The
    /// tail past the cap can never reach the audio path, so retaining it buys
    /// nothing. A non-finite / non-positive `src_rate` is left untruncated: it is
    /// rejected later by read_impulse_response_pcm, and no frame count can be
    /// derived from it.
    void set_ir_pcm(const float* samples, std::size_t frames, std::uint32_t channels,
                    double src_rate, std::string label) {
        IrPcm next;
        if (samples != nullptr && frames > 0 && channels > 0) {
            // The caller's planes are strided by the ORIGINAL frame count; the
            // truncated count is only how many of them we keep.
            const std::size_t src_frames = frames;
            std::size_t keep = src_frames;
            if (std::isfinite(src_rate) && src_rate > 0.0) {
                const double max_frames = kMaxIrSeconds * src_rate;
                if (static_cast<double>(keep) > max_frames)
                    keep = static_cast<std::size_t>(max_frames);
            }
            // Mono-sum (average) the planes into one plane.
            next.samples.resize(keep);
            const float inv = channels > 1
                                  ? 1.0f / static_cast<float>(channels)
                                  : 1.0f;
            for (std::size_t i = 0; i < keep; ++i) {
                float v = 0.0f;
                for (std::uint32_t c = 0; c < channels; ++c)
                    v += samples[static_cast<std::size_t>(c) * src_frames + i];
                next.samples[i] = v * inv;
            }
            next.frames = keep;
            next.channels = 1;
            next.rate = src_rate;
            next.label = std::move(label);
        }
        {
            std::lock_guard<std::mutex> lock(ir_path_mutex_);
            pcm_ = std::move(next);
            active_ir_path_.clear();
            built_in_id_ = 0;
        }
        ir_path_gen_.fetch_add(1, std::memory_order_release);
    }

    /// Select a built-in synthetic IR (see kBuiltInIrs). Clears any PCM/file
    /// source, so the plugin needs no I/O at all to change spaces. An unknown id
    /// is stored verbatim and rendered as id 0. Main/UI thread only.
    void set_built_in_ir(std::uint8_t id) {
        {
            std::lock_guard<std::mutex> lock(ir_path_mutex_);
            if (id == built_in_id_ && pcm_.samples.empty() && active_ir_path_.empty())
                return;
            built_in_id_ = id;
            pcm_ = IrPcm{};
            active_ir_path_.clear();
        }
        ir_path_gen_.fetch_add(1, std::memory_order_release);
    }

    /// The selected built-in IR id (meaningful only when no file/PCM source is
    /// set). UI/main-thread only (takes a mutex).
    std::uint8_t built_in_ir() const {
        std::lock_guard<std::mutex> lock(ir_path_mutex_);
        return built_in_id_;
    }

    /// Frames of PCM currently set as the IR source (0 when none). UI/main-thread
    /// only (takes a mutex).
    std::size_t ir_pcm_frames() const {
        std::lock_guard<std::mutex> lock(ir_path_mutex_);
        return pcm_.frames;
    }

    /// The current IR source path ("" when using PCM or a built-in IR).
    /// UI/main-thread only (takes a mutex).
    std::string ir_path() const override {
        std::lock_guard<std::mutex> lock(ir_path_mutex_);
        return active_ir_path_;
    }

    /// How many times a PCM/file source has actually been decoded + resampled.
    /// A Size change re-windows the cached decode instead of redoing it, so this
    /// stays put across Size moves; exposed for tests / tooling.
    std::uint32_t source_decode_count() const {
        return source_decodes_.load(std::memory_order_relaxed);
    }

    /// Monotonic counter advanced each time the IR source is (re)selected. The
    /// background worker rebuilds when it moves; exposed for tests / tooling.
    std::uint32_t ir_path_generation() const {
        return ir_path_gen_.load(std::memory_order_acquire);
    }

#if !defined(PULP_WASM)

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

#endif  // !PULP_WASM

    /// Persist the IR source alongside StateStore so a host restores the impulse
    /// response with the project / preset. The source is opaque non-parameter
    /// state, exactly what serialize_plugin_state() is for.
    ///
    /// v2 blob: magic + version + a 1-byte IrStateKind tag + that kind's payload.
    /// A raw filesystem path (all v1 could express) is meaningless on a host with
    /// no filesystem, so the tag lets the same blob carry a built-in id — the
    /// portable choice — or the IR samples inline. Inline PCM is bounded because
    /// set_ir_pcm() mono-sums and truncates its input to kMaxIrSeconds AT THE
    /// SOURCE, so the worst case here is ~1.9 MB (10 s / 48 kHz mono) no matter how
    /// long the file the user dropped in was. It is still by far the largest kind,
    /// so it is written only when PCM is actually the live source.
    std::vector<uint8_t> serialize_plugin_state() const override {
        std::vector<uint8_t> out;
        out.insert(out.end(), kStateMagic, kStateMagic + 4);
        out.push_back(kStateVersion);

        std::lock_guard<std::mutex> lock(ir_path_mutex_);
        if (!pcm_.samples.empty()) {
            // Mono-sum the source planes: the convolver runs a mono IR anyway, so
            // storing one plane keeps the blob as small as the format allows.
            const std::size_t frames = pcm_.frames;
            const std::uint32_t ch = pcm_.channels;
            out.push_back(static_cast<std::uint8_t>(IrStateKind::Pcm));
            append_u32(out, static_cast<std::uint32_t>(frames));
            append_u32(out, static_cast<std::uint32_t>(std::lround(pcm_.rate)));
            const float inv = ch > 1 ? 1.0f / static_cast<float>(ch) : 1.0f;
            for (std::size_t i = 0; i < frames; ++i) {
                float v = 0.0f;
                for (std::uint32_t c = 0; c < ch; ++c)
                    v += pcm_.samples[static_cast<std::size_t>(c) * frames + i];
                append_f32(out, v * inv);
            }
        } else if (!active_ir_path_.empty()) {
            out.push_back(static_cast<std::uint8_t>(IrStateKind::Path));
            out.insert(out.end(), active_ir_path_.begin(), active_ir_path_.end());
        } else if (built_in_id_ != 0) {
            out.push_back(static_cast<std::uint8_t>(IrStateKind::BuiltIn));
            out.push_back(built_in_id_);
        } else {
            out.push_back(static_cast<std::uint8_t>(IrStateKind::Synthetic));
        }
        return out;
    }

    /// Restore the IR source. Reads the v2 tagged blob; a v1 blob (magic "SCv1")
    /// and a pre-versioning raw-path blob both still restore their path, so an
    /// existing native project loads unchanged. A truncated / unknown payload
    /// falls back to the default synthetic IR rather than failing the load — the
    /// same fail-safe as a missing file. Always returns true.
    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        const auto* bytes = reinterpret_cast<const char*>(data.data());
        const bool v2 = data.size() >= kStateHeaderSize &&
                        std::equal(kStateMagic, kStateMagic + 4, bytes);
        const bool v1 = data.size() >= kStateHeaderSize &&
                        std::equal(kStateMagicV1, kStateMagicV1 + 4, bytes);

        if (!v2) {
            // v1: the payload after the header is the path. No magic at all: the
            // pre-versioning raw-path blob. An empty blob means "no loaded IR".
            std::string path;
            if (v1)
                path.assign(bytes + kStateHeaderSize, data.size() - kStateHeaderSize);
            else if (!data.empty())
                path.assign(bytes, data.size());
            set_ir_path(std::move(path));
            return true;
        }

        std::span<const uint8_t> p = data.subspan(kStateHeaderSize);
        if (p.empty()) {
            set_built_in_ir(0);
            return true;
        }
        const auto kind = static_cast<IrStateKind>(p[0]);
        p = p.subspan(1);
        switch (kind) {
        case IrStateKind::BuiltIn:
            set_built_in_ir(p.empty() ? 0 : p[0]);
            return true;
        case IrStateKind::Pcm: {
            if (p.size() < 8) break;
            const std::uint32_t frames = read_u32(p.data());
            const std::uint32_t rate = read_u32(p.data() + 4);
            const std::size_t need = static_cast<std::size_t>(frames) * sizeof(float);
            if (frames == 0 || p.size() - 8 < need) break;
            std::vector<float> pcm(frames);
            for (std::uint32_t i = 0; i < frames; ++i)
                pcm[i] = read_f32(p.data() + 8 + i * sizeof(float));
            set_ir_pcm(pcm.data(), frames, 1, static_cast<double>(rate), std::string());
            return true;
        }
        case IrStateKind::Path:
            set_ir_path(std::string(reinterpret_cast<const char*>(p.data()), p.size()));
            return true;
        case IrStateKind::Synthetic:
            break;
        }
        set_built_in_ir(0);
        return true;
    }

    /// Lock-free latest wet-output spectrum for the UI (UI is sole reader).
    SpectrumBus& spectrum_bus() { return spectrum_bus_; }

#if !defined(PULP_HEADLESS)

    /// Native GPU front-end (live IR waveform + frequency display + controls).
    /// Defined in super_convolver_view.cpp — a headless build has no view/canvas
    /// to link it against, so the override (and its vtable slot) compiles out and
    /// the base class's "no editor" default stands.
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

#endif  // !PULP_HEADLESS

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const std::size_t n = output.num_samples();
        const std::size_t ch_count = output.num_channels();

        // MIX AND GAIN ARE RAMPED, NOT STEPPED.
        //
        // These used to be per-block constants: `out = (1-mix)*dry + mix*gain*wet` with mix
        // jumping straight to its new value at the block boundary. Toggling Bypass therefore
        // slammed mix from (say) 0.35 to 0 between one sample and the next — a step
        // discontinuity in the output, which is audible as a CLICK. The same edge gives Mix
        // and Gain zipper noise on a fast drag.
        //
        // So each block interpolates from where the last one ended to where this one wants to
        // be. One block at 512 frames / 48 kHz is ~10.7 ms — long enough that the transition
        // is inaudible, short enough that the control still feels immediate.
        const bool bypass = state().get_value(kBypass) >= 0.5f;
        const float mix_target = bypass ? 0.0f : state().get_value(kMix) / 100.0f;
        const float gain_target = std::pow(10.0f, state().get_value(kGain) / 20.0f);
        if (!ramp_primed_) {   // first block after prepare(): start AT the target, not at 0
            mix_z_ = mix_target;
            gain_z_ = gain_target;
            ramp_primed_ = true;
        }
        const float mix_step = n > 0 ? (mix_target - mix_z_) / static_cast<float>(n) : 0.0f;
        const float gain_step = n > 0 ? (gain_target - gain_z_) / static_cast<float>(n) : 0.0f;

        // Publish the desired live config to the IR/engine rebuild (cheap atomic
        // stores, no alloc). The rebuild runs off the audio thread; the audio
        // thread only ever LOADS what it publishes.
        requested_size_.store(current_size(), std::memory_order_relaxed);
#if !defined(PULP_WASM)
        // Engine / Rooms / Flow only exist on the native lanes (see
        // define_parameters) — there is no GPU engine to steer in a wasm module.
        requested_engine_.store(requested_engine_value(), std::memory_order_relaxed);
        requested_rooms_.store(requested_rooms_value(), std::memory_order_relaxed);
        requested_flow_.store(
            static_cast<float>(state().get_value(kFlow)) * 0.01f,
            std::memory_order_relaxed);
#endif

#if !defined(PULP_WASM)
        // Offline / faster-than-real-time render (e.g. a Logic bounce): the GPU
        // engine's async, wall-clock-paced worker can't keep up with a render that
        // runs faster than real time, so the GPU path must be driven synchronously
        // to capture its actual output instead of dropping it. The CPU path is
        // already synchronous/inline, so it is unaffected.
        const bool offline = ctx.is_offline();

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
#elif defined(PULP_SC_WEB_GPU)
        (void)ctx;
        // ONE path for both engines, and it always drives the GPU bridge — the
        // Engine parameter only chooses which wet is EMITTED.
        //
        // The bridge's push/pop pair is what advances the block timeline the ring
        // stamps every block with, so it has to run on every block for the same
        // reason the CPU convolver does: a convolver (or a ring) that stops running
        // while the other engine carries the audio is not in a state you can flip
        // back to. If Engine=CPU skipped the bridge, the out-ring would freeze
        // holding the wets it had already produced, and the first blocks after a
        // flip back to GPU would emit THOSE — pre-flip audio, presented as current.
        // (They would not "simply miss": a non-empty ring is a hit.) Driving it
        // every block instead means the flip is exactly a change of which buffer is
        // copied out, with no dropout, no stale burst, and no resync machinery.
        //
        // The cost is honest and bounded: while Engine=CPU the GPU worker keeps
        // convolving blocks whose wet is discarded. The emitted audio is unaffected
        // — byte-identical to the CPU-only module's — and the GPU block counters
        // only advance while the GPU engine is actually the one being heard.
        fill_wet_web(input, n, requested_engine_value() == 1);
#else
        (void)ctx;
        fill_wet_cpu(input, n);
#endif

        // Emit n samples per channel: wet from the primed output FIFO, dry
        // delayed by the active engine's total latency so they stay aligned.
        for (std::size_t ch = 0; ch < ch_count && ch < kChannels; ++ch) {
            const float* in =
                ch < input.num_channels() ? input.channel(ch).data() : nullptr;
            float* out = output.channel(ch).data();
            const std::size_t avail = out_len_[ch];
            const std::size_t delay = dry_ring_[ch].size();
            // Every channel walks the SAME ramp, so it starts from the block-entry value
            // rather than from wherever the previous channel left off.
            float mix = mix_z_, gain = gain_z_;
            for (std::size_t i = 0; i < n; ++i) {
                const float wet_i = i < avail ? out_buf_[ch][i] : 0.0f;
                const float dry_i = dry_ring_[ch][dry_pos_[ch]];
                dry_ring_[ch][dry_pos_[ch]] = in ? in[i] : 0.0f;
                dry_pos_[ch] = (dry_pos_[ch] + 1) % delay;
                out[i] = (1.0f - mix) * dry_i + mix * gain * wet_i;
                mix += mix_step;
                gain += gain_step;
            }
            const std::size_t consumed = n < avail ? n : avail;
            std::memmove(out_buf_[ch].data(), out_buf_[ch].data() + consumed,
                         (out_len_[ch] - consumed) * sizeof(float));
            out_len_[ch] -= consumed;
        }
        // Land exactly on the target: accumulating `+= step` n times drifts in float, and a
        // ramp that never quite arrives is a ramp that is still moving on the next block.
        mix_z_ = mix_target;
        gain_z_ = gain_target;

        if (ch_count > 0) publish_spectrum(output.channel(0).data(), static_cast<int>(n));
    }

private:
    static constexpr std::size_t kChannels = 2;

    // Decoded PCM handed straight in as the IR source (set_ir_pcm). Planar:
    // `channels` consecutive planes of `frames` samples each, at `rate`. Guarded
    // by ir_path_mutex_ alongside the other source selectors, so they stay one
    // coherent choice.
    struct IrPcm {
        std::vector<float> samples;
        std::size_t frames = 0;
        std::uint32_t channels = 0;
        double rate = 0.0;
        std::string label;
    };

    static void append_u32(std::vector<uint8_t>& out, std::uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    }
    static void append_f32(std::vector<uint8_t>& out, float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        append_u32(out, bits);
    }
    static std::uint32_t read_u32(const uint8_t* p) {
        return static_cast<std::uint32_t>(p[0])
             | (static_cast<std::uint32_t>(p[1]) << 8)
             | (static_cast<std::uint32_t>(p[2]) << 16)
             | (static_cast<std::uint32_t>(p[3]) << 24);
    }
    static float read_f32(const uint8_t* p) {
        const std::uint32_t bits = read_u32(p);
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

#if !defined(PULP_WASM) || defined(PULP_SC_WEB_GPU)

    // The engine the audio path should run. The CPU-only web lanes neither compile
    // a GPU runtime nor declare the Engine parameter (see define_parameters), so
    // they always run the CPU convolver — which is the DEFAULT engine everywhere
    // else too, not a reduced substitute.
    int requested_engine_value() const {
        return state().get_value(kEngine) >= 0.5f ? 1 : 0;
    }

#endif

#if !defined(PULP_WASM)

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

#endif  // !PULP_WASM

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

#if defined(PULP_SC_WEB_GPU)

    // The web build's ONE audio path, for both engines. The same fixed re-blocking
    // as fill_wet_cpu(), plus: every internal block is handed across the
    // pulp_gpu_xfer bridge to the WebGPU worker, and `gpu_engine` chooses whether
    // the GPU's wet or the CPU's is what gets emitted. RT-safe: preallocated
    // scratch, one call, no alloc, no lock, no wait.
    //
    // BOTH the CPU convolver and the bridge run on EVERY block, whichever engine is
    // selected, and for the same reason: state. (1) An overlap-save convolver's
    // output depends on its input history, so a convolver that stopped running
    // while the GPU carried the audio would emit garbage for its whole tail on the
    // first block after a miss or an Engine flip — and it is the safety net itself
    // (this is the argument behind the native path's continuously-primed fallback,
    // gpu_audio::GpuConvolver's prime_fallback). (2) The ring is a TIMELINE: push
    // and pop are what advance the block index every wet is stamped with, so a
    // bridge that idled during CPU mode would hand the next GPU block a ring full
    // of pre-flip audio.
    void fill_wet_web(const audio::BufferView<const float>& input, std::size_t n,
                      bool gpu_engine) {
        // Block-boundary IR handoff and the Size request, same as the CPU engine —
        // the CPU convolvers stay live under both engines, so they must stay current.
        for (std::size_t ch = 0; ch < conv_.size(); ++ch)
            conv_[ch].try_swap_ir(swapper_[ch]);
        requested_size_.store(current_size(), std::memory_order_relaxed);

        // No net: a missed block is SILENT rather than CPU-covered, so the GPU is
        // provably the only thing making sound. Read once per host block. Only
        // meaningful while the GPU engine is the one being emitted.
        const bool gpu_only = gpu_engine && state().get_value(kGpuOnly) >= 0.5f;

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
            // (1) CPU wet for this block, delayed by the GPU round-trip so it is a
            // sample-for-sample substitute for the GPU wet of the same block.
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                conv_[ch].process(in_buf_[ch].data(), wet_.data(), kInternalBlock);
                delay_cpu_wet(ch);
                std::memcpy(cpu_wet_[ch].data(), wet_.data(),
                            kInternalBlock * sizeof(float));
                // Pack the dry input planes for the bridge while the block is still
                // at the head of the FIFO.
                std::memcpy(xfer_in_.data() + ch * kInternalBlock,
                            in_buf_[ch].data(), kInternalBlock * sizeof(float));
            }

            // (2) Hand the block to the GPU worker and take back the wet it has for
            // input block n-kWebGpuLatencyBlocks (the ring matches wets to block
            // indices, so this is that block's wet or nothing — never a stale one).
            // This runs under BOTH engines: it is what keeps the shared timeline
            // advancing. The counters, though, describe the GPU ENGINE's audio, so
            // they only move while the GPU engine is the one being emitted.
            const int wet_ready = pulp_gpu_xfer(xfer_in_.data(), xfer_out_.data(),
                                                static_cast<int>(kInternalBlock),
                                                static_cast<int>(kChannels));
            const bool hit = gpu_engine && wet_ready != 0;
            if (gpu_engine) {
                if (hit)
                    web_gpu_blocks_.fetch_add(1, std::memory_order_relaxed);
                else
                    web_gpu_misses_.fetch_add(1, std::memory_order_relaxed);
            }

            // (3) Emit: on the GPU engine, the GPU wet on a hit; on a miss the
            // L-delayed CPU wet, or silence when the user has taken the net away.
            // On the CPU engine, always the CPU wet — byte-identical to what the
            // CPU-only module emits, whatever the GPU worker happened to produce.
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                float* dst = out_buf_[ch].data() + out_len_[ch];
                if (hit)
                    std::memcpy(dst, xfer_out_.data() + ch * kInternalBlock,
                                kInternalBlock * sizeof(float));
                else if (gpu_only)
                    std::memset(dst, 0, kInternalBlock * sizeof(float));
                else
                    std::memcpy(dst, cpu_wet_[ch].data(),
                                kInternalBlock * sizeof(float));
                out_len_[ch] += kInternalBlock;

                std::memmove(in_buf_[ch].data(), in_buf_[ch].data() + kInternalBlock,
                             (in_len_[ch] - kInternalBlock) * sizeof(float));
                in_len_[ch] -= kInternalBlock;
            }
        }
    }

#endif  // PULP_SC_WEB_GPU

#if !defined(PULP_WASM)

    // GPU engine: same fixed re-blocking, but each B-block is processed as ONE
    // stereo block through the GPU transport (RT-safe by contract — the GPU FFT
    // runs on the transport's non-RT worker; on a worker miss the node's miss
    // policy fills the block — CpuFallback for the single-IR GpuConvolver,
    // Silence for the many-rooms GpuMultiConvolver). Both channels advance in lockstep
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
        if (fresh && fresh->multi)
            fresh->multi->set_flow(requested_flow_.load(std::memory_order_relaxed));
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

    // Native only — the Rooms parameter is not declared on the web lanes.
    int requested_rooms_value() const {
        const int r = static_cast<int>(std::lround(state().get_value(kRooms)));
        return r > 1 ? r : 1;
    }

#endif  // !PULP_WASM

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
    // or the Size knob moved. Size governs the synthetic IR's length AND (via
    // window_ir_to_length) truncates a loaded file's tail, so a Size change must
    // rebuild in BOTH cases — the earlier `!worker_has_file_` guard is exactly
    // why the Size knob felt dead once a file IR was loaded. Worker-thread only.
    bool base_needs_rebuild(std::uint32_t want_gen, float want_size) const {
        if (want_gen != worker_base_gen_) return true;
        if (want_size > 0.0f && want_size != worker_base_size_) return true;
        return false;
    }

    // Produce the base IR for the current source, in precedence order: decoded PCM
    // (set_ir_pcm), then a readable IR file (native only), then the selected
    // built-in synthetic IR. A PCM/file base is summed to mono and resampled to
    // the session SR, then WINDOWED by the Size knob to `seconds` with a short
    // fade tail so Size stays live for a loaded base too (see
    // window_ir_to_length); the built-in IR is simply generated at `seconds`. The
    // decoded base is cached in worker_file_raw_ keyed by ir_path_gen_, so a Size
    // change only re-windows (cheap) — the expensive decode/resample runs once per
    // source, not per Size step. Sets worker_has_file_. Worker / prepare thread
    // only (IO + alloc); never the audio thread.
    std::vector<float> build_base_ir(float seconds) {
        const std::uint32_t gen = ir_path_gen_.load(std::memory_order_acquire);
        const bool stale = worker_file_raw_.empty() || worker_file_raw_gen_ != gen;

        // One coherent read of the source selectors. The PCM samples are copied
        // only when a decode is actually due, so a Size re-window costs nothing.
        std::string path;
        std::uint8_t built_in = 0;
        IrPcm pcm;
        bool has_pcm = false;
        {
            std::lock_guard<std::mutex> lock(ir_path_mutex_);
            path = active_ir_path_;
            built_in = built_in_id_;
            has_pcm = !pcm_.samples.empty();
            if (has_pcm && stale) pcm = pcm_;
        }

        if (has_pcm) {
            if (stale) decode_pcm_base(pcm, gen);
            if (!worker_file_raw_.empty()) return window_loaded_base(seconds);
        }
#if !defined(PULP_WASM)
        else if (!path.empty()) {
            if (stale) decode_file_base(path, gen);
            if (!worker_file_raw_.empty()) return window_loaded_base(seconds);
        }
#endif
        (void)path;

        worker_has_file_ = false;
        worker_file_raw_.clear();
        worker_file_raw_gen_ = 0;
        return make_builtin_ir(built_in, ir_length_for(seconds));
    }

    // Start (or RESTART, superseding whatever was in flight) a bounded rebuild of
    // the base IR + both channels' convolver states for `seconds` / `gen`.
    //
    // The source is resolved here, exactly as build_base_ir() resolves it: decoded
    // PCM first, then a readable file (native only), then the selected built-in.
    // The DECODE itself (resample + mono-sum + unit-energy normalize, inside
    // audio::read_impulse_response_pcm) is the one step that is NOT sliced — but
    // it is cached per SOURCE, not per Size, so it runs once when a user loads an
    // IR and never again while they work the Size knob. Since a browser page
    // decodes with AudioContext.decodeAudioData, its PCM already arrives at the
    // session rate and the resampler is skipped, leaving an O(n) pass. The Size
    // knob — the actual reported defect — is bounded end to end.
    void start_rebuild_job(float seconds, std::uint32_t gen) {
        rebuild_job_.cancel();
        job_size_ = seconds;
        job_gen_ = gen;

        const bool stale = worker_file_raw_.empty() || worker_file_raw_gen_ != gen;
        std::string path;
        std::uint8_t built_in = 0;
        IrPcm pcm;
        bool has_pcm = false;
        {
            std::lock_guard<std::mutex> lock(ir_path_mutex_);
            path = active_ir_path_;
            built_in = built_in_id_;
            has_pcm = !pcm_.samples.empty();
            if (has_pcm && stale) pcm = pcm_;
        }

        if (has_pcm) {
            if (stale) decode_pcm_base(pcm, gen);
            if (!worker_file_raw_.empty()) { start_windowed_job(seconds); return; }
        }
#if !defined(PULP_WASM)
        else if (!path.empty()) {
            if (stale) decode_file_base(path, gen);
            if (!worker_file_raw_.empty()) { start_windowed_job(seconds); return; }
        }
#endif
        (void)path;

        worker_has_file_ = false;
        worker_file_raw_.clear();
        worker_file_raw_gen_ = 0;
        const BuiltInIr& b = kBuiltInIrs[built_in < kBuiltInIrCount ? built_in : 0];
        rebuild_job_.start_synthetic(ir_length_for(seconds), b.decay_norm, b.density,
                                     b.seed, kInternalBlock, kChannels);
    }

    void start_windowed_job(float seconds) {
        worker_has_file_ = true;
        std::size_t target = ir_length_for(seconds);
        const std::size_t cap = static_cast<std::size_t>(kMaxIrSeconds * sample_rate_);
        if (target > cap) target = cap;
        const std::size_t fade = static_cast<std::size_t>(0.030 * sample_rate_);
        rebuild_job_.start_windowed(worker_file_raw_, target, fade, kInternalBlock,
                                    kChannels);
    }

    // Decode + resample + normalize supplied PCM into the cached base. Shared by
    // the one-shot build_base_ir() and the sliced job's source resolution.
    void decode_pcm_base(const IrPcm& pcm, std::uint32_t gen) {
        auto loaded = audio::read_impulse_response_pcm(
            pcm.samples.data(), pcm.frames, pcm.channels, pcm.rate, sample_rate_,
            {.max_seconds = kMaxIrSeconds, .normalize_unit_energy = true});
        source_decodes_.fetch_add(1, std::memory_order_relaxed);
        if (loaded && !loaded->empty()) {
            worker_file_raw_ = std::move(*loaded);
            worker_file_raw_gen_ = gen;
        } else {
            worker_file_raw_.clear();
            runtime::log_warn(
                "SuperConvolver: the supplied IR PCM is unusable (silent, "
                "non-finite, or an implausible rate); falling back to the "
                "built-in synthetic IR.");
        }
    }

#if !defined(PULP_WASM)
    void decode_file_base(const std::string& path, std::uint32_t gen) {
        // Any exception during decode/resample (e.g. bad_alloc on a huge or
        // corrupt file) must never escape onto the worker or prepare thread — fall
        // back to the synthetic IR rather than crash.
        std::optional<std::vector<float>> loaded;
        try {
            loaded = load_ir_file(path);
        } catch (...) {
            loaded = std::nullopt;
        }
        source_decodes_.fetch_add(1, std::memory_order_relaxed);
        if (loaded && !loaded->empty()) {
            worker_file_raw_ = std::move(*loaded);
            worker_file_raw_gen_ = gen;
        } else {
            worker_file_raw_.clear();
            runtime::log_warn(
                "SuperConvolver: IR file '{}' is unreadable, too large, or "
                "empty; falling back to the built-in synthetic IR.", path);
        }
    }
#endif

    // Window the cached decoded base (PCM or file) to the Size knob. Size truncates
    // the loaded space's tail (bounded by kMaxIrSeconds, which the loader already
    // enforces on decode). A ~30 ms fade tail (capped to the target) smooths the
    // cut. If the base is shorter than Size the window is a no-op (never
    // zero-pad-extended). Sets worker_has_file_.
    std::vector<float> window_loaded_base(float seconds) {
        worker_has_file_ = true;
        std::size_t target = ir_length_for(seconds);
        const std::size_t cap = static_cast<std::size_t>(kMaxIrSeconds * sample_rate_);
        if (target > cap) target = cap;
        const std::size_t fade = static_cast<std::size_t>(0.030 * sample_rate_);
        return window_ir_to_length(worker_file_raw_, target, fade);
    }

#if !defined(PULP_WASM)

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

        // Guard against a degenerate (silent / non-finite) variant, then peak-
        // response normalize to the SAME 0 dB max-gain rule as the base IR so no
        // room can push the batched multi-room sum past unity either.
        double energy = 0.0;
        for (float v : cur) energy += static_cast<double>(v) * v;
        if (!std::isfinite(energy) || energy <= 0.0) return base;
        normalize_peak_response(cur, 1.0f);
        return cur;
    }

#endif  // !PULP_WASM

    void publish_display_ir(const std::vector<float>& ir) {
        {
            std::lock_guard<std::mutex> lock(ir_display_mutex_);
            ir_display_ = ir;
        }
        ir_generation_.fetch_add(1, std::memory_order_relaxed);
    }

#if !defined(PULP_WASM)

    void start_worker() {
        if (!worker_enabled_) return;
        worker_run_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { worker_loop(); });
    }

    void stop_worker() {
        worker_run_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    // Background thread: drive service_ir_rebuild() on a 5 ms poll so IR and
    // engine changes reconcile off the audio thread. A host without threads
    // compiles this out and calls service_ir_rebuild() itself.
    void worker_loop() {
        using namespace std::chrono_literals;
        while (worker_run_.load(std::memory_order_acquire)) {
            service_ir_rebuild();
            std::this_thread::sleep_for(5ms);
        }
        // Final drain so nothing leaks once the audio thread has stopped.
        for (auto& sw : swapper_) sw.drain_old();
    }

    // The GPU half of one rebuild pass: reconcile the live stack with the
    // requested Engine/Rooms, push the live Flow depth, and reclaim what the audio
    // thread has released. Off-audio-thread only.
    void service_gpu() {
        const int want_engine = requested_engine_.load(std::memory_order_relaxed);
        const int want_rooms = requested_rooms_.load(std::memory_order_relaxed);
        const float want_flow = requested_flow_.load(std::memory_order_relaxed);

        // Debounce Rooms: rebuilding the GPU stack retires the live stack and
        // builds a fresh one, so the audio drops to the CPU/single-IR path for
        // the gap. Dragging the Rooms slider changes the value every poll, which
        // would rebuild dozens of times and glitch the sound in and out. Hold
        // the currently-built count until Rooms has been STABLE for a moment,
        // then rebuild once. Engine toggles and IR/Size changes are NOT
        // debounced (they route through the other branches immediately).
        if (want_rooms != rooms_last_seen_) {
            rooms_last_seen_ = want_rooms;
            rooms_stable_polls_ = 0;
        } else if (rooms_stable_polls_ < kRoomsSettlePolls) {
            ++rooms_stable_polls_;
        }
        const bool rooms_settled = rooms_stable_polls_ >= kRoomsSettlePolls;
        const int effective_rooms =
            rooms_settled ? want_rooms
                          : (gpu_built_rooms_ > 0 ? gpu_built_rooms_ : want_rooms);

        // GPU stack management (only meaningful when a device exists).
        if (device_available_)
            service_gpu_stack(want_engine, effective_rooms);

        // Push the live Flow depth into the multi-room node (this thread owns the
        // stack, so the deref is safe here). The node applies it on its next block
        // on the transport worker — ~5 ms latency on Flow *changes*, inaudible for
        // a slow pan LFO.
        if (current_stack_ && current_stack_->multi)
            current_stack_->multi->set_flow(want_flow);

        // Free any retired stack the audio thread has since released (a stack
        // retired during a slow in-flight block is reclaimed here once the hazard
        // clears, without waiting for the next rebuild).
        if (!retired_stacks_.empty()) reclaim_retired();
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

#else   // PULP_WASM: no threads (neither web lane can spawn one) and no GPU stack.

    void start_worker() {}
    void stop_worker() {}

#endif

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
#if !defined(PULP_WASM)
    std::unique_ptr<GpuStack> current_stack_;
    std::vector<std::unique_ptr<GpuStack>> retired_stacks_;
    std::atomic<gpu_audio::GpuAudioTransport*> gpu_active_{nullptr};
    std::atomic<gpu_audio::GpuAudioTransport*> gpu_in_use_{nullptr};  // audio-thread hazard ptr
    mutable std::mutex stack_mutex_;
    std::array<std::vector<float>, kChannels> gpu_wet_{};
    std::atomic<bool> gpu_engine_active_{false};   // mirrors (gpu_active_ != null)
    bool device_available_ = false;                // a GPU device exists (set at prepare)
#endif
#if defined(PULP_SC_WEB_GPU)
    // Browser GPU engine scratch (audio thread only, preallocated at prepare):
    // the L-delayed CPU wet held as the safety net for the block in flight, and
    // the two planar staging buffers the pulp_gpu_xfer bridge reads/writes.
    std::array<std::vector<float>, kChannels> cpu_wet_{};
    std::vector<float> xfer_in_;
    std::vector<float> xfer_out_;
    std::atomic<std::uint64_t> web_gpu_blocks_{0};   // GPU wets delivered
    std::atomic<std::uint64_t> web_gpu_misses_{0};   // blocks the worker missed
#endif
    std::size_t gpu_extra_ = 0;                     // GPU transport latency, samples (fixed)
    // Runtime pipeline depth (blocks) → gpu_extra_ at prepare(). Default is the
    // historical compile-time constant; the adaptive depth logic sets it per device.
    std::size_t web_gpu_latency_blocks_ = kWebGpuLatencyBlocks;
    int latency_samples_ = static_cast<int>(kInternalBlock);

    // Off-audio-thread rebuild + live-engine state. The worker exists only where a
    // thread does; worker_enabled_ lets a host opt out of it and pump
    // service_ir_rebuild() from its own control thread instead.
#if !defined(PULP_WASM)
    std::thread worker_;
    std::atomic<bool> worker_run_{false};
    bool worker_enabled_ = true;
#else
    bool worker_enabled_ = false;
#endif
    std::atomic<float> requested_size_{-1.0f};
    std::atomic<int> requested_engine_{0};          // 0 = CPU, 1 = GPU
    std::atomic<int> requested_rooms_{1};
    std::atomic<float> requested_flow_{0.0f};        // 0..1 moving-field depth
#if !defined(PULP_WASM)
    int gpu_built_rooms_ = 0;          // rebuild-thread-local (current_stack_ config)
    // Rooms debounce (rebuild-thread-local): hold the built count until the Rooms
    // request has been stable for kRoomsSettlePolls worker polls (~120 ms at the
    // 5 ms poll), so dragging the slider rebuilds the GPU stack once on release
    // instead of glitching the audio in and out on every intermediate value.
    static constexpr int kRoomsSettlePolls = 24;
    int rooms_last_seen_ = -1;
    int rooms_stable_polls_ = 0;
    bool gpu_base_dirty_ = false;
#endif

    // IR source: decoded PCM, a filesystem path, or a built-in id — mutually
    // exclusive, all guarded by ir_path_mutex_ and all set from the UI/main thread
    // and read on the rebuild thread. ir_path_gen_ is the lock-free trigger the
    // rebuild polls so a new source rebuilds the base IR off the audio thread. The
    // worker_base_* fields are rebuild-thread-local: the current base IR plus the
    // (generation, size) tuple that produced it, so the rebuild only runs when the
    // source actually changed. gpu_base_dirty_ flags that the GPU stack must
    // rebuild against a freshly produced base IR.
    mutable std::mutex ir_path_mutex_;
    std::string active_ir_path_;
    IrPcm pcm_;
    std::uint8_t built_in_id_ = 0;
    std::atomic<std::uint32_t> ir_path_gen_{0};
    std::atomic<std::uint32_t> source_decodes_{0};
    std::vector<float> worker_base_ir_;             // rebuild/prepare-thread-local
    std::uint32_t worker_base_gen_ = 0;
    float worker_base_size_ = -1.0f;
    bool worker_has_file_ = false;
    // The decoded base (mono, session SR), cached so a Size change only re-windows
    // instead of re-decoding. Keyed by the ir_path generation that produced it; a
    // new source (generation moves) forces a fresh decode.
    std::vector<float> worker_file_raw_;
    std::uint32_t worker_file_raw_gen_ = 0;

    // The bounded, resumable rebuild the WORKER-LESS lanes (both web ABIs, and any
    // test that turns the worker off) run instead of a blocking pass. job_size_ /
    // job_gen_ are what the in-flight job is BUILDING, so a request that differs
    // from them supersedes it. Rebuild-thread-local; never touched by the audio
    // thread, which only ever calls try_swap_ir().
    superconvolver::SlicedIrRebuild rebuild_job_;
    float job_size_ = -1.0f;
    std::uint32_t job_gen_ = 0;

    // UI display snapshot (UI + worker thread only; never audio thread).
    // Dry/wet and output gain, ramped per-sample across a block so Bypass does not click and
    // a fast Mix/Gain drag does not zipper. `ramp_primed_` makes the first block after
    // prepare() start AT the target rather than sliding up from zero (which would fade the
    // plugin in every time the host re-prepares it).
    float mix_z_ = 0.0f;
    float gain_z_ = 1.0f;
    bool ramp_primed_ = false;

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

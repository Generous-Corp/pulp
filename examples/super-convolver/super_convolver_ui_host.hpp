#pragma once

// The surface SuperConvolver's editor needs — and NOTHING ELSE.
//
// The editor used to `#include "super_convolver.hpp"` and hold a
// `SuperConvolverProcessor&`. Natively that is free: the editor and the DSP live in one
// binary, so reaching into the processor costs nothing.
//
// ON THE WEB THEY ARE NOT IN THE SAME PLACE. The DSP runs inside an AudioWorklet (a
// separate wasm module, on the audio thread); the editor runs on the PAGE, in a
// DSP-free wasm module of its own. There is no processor to hold a reference to — the
// two can only speak by passing messages across that boundary. So an editor welded to
// the processor cannot be the web editor, and the browser demo shipped a separate,
// far simpler panel instead.
//
// Splitting them turned out to be small, because the coupling was never wide: past the
// StateStore (parameters) and the SpectrumBus (already injected separately), the editor
// asked the processor for exactly FOUR things. They are the interface below.
//
// This header therefore carries the editor's whole non-parameter dependency surface —
// the parameter IDs, the spectrum bus, the GPU status, and the host interface — with no
// FFT, no convolver, no audio thread. `super_convolver.hpp` includes it and implements
// it; the web module includes it and implements it from JS.

#include <pulp/runtime/runtime.hpp>
#include <pulp/state/parameter.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::examples {

enum SuperConvolverParams : state::ParamID {
    kMix     = 1,  // dry/wet, %
    kSize    = 2,  // IR length, seconds
    kGain    = 3,  // output gain, dB
    kBypass  = 4,
    kEngine  = 5,  // 0 = CPU (default), 1 = GPU
    kRooms   = 6,  // GPU multi-room reverb: # of distinct IRs in one GPU batch
    kFlow    = 7,  // 0 = static field; >0 = per-block moving pans (living field)
    kGpuOnly = 8,  // web GPU engine: 0 = CPU safety net (default), 1 = no net
};

// Live wet-output magnitude spectrum (dB), published lock-free from the audio
// thread to the GPU UI's frequency display. 256 log-ready bins.
inline constexpr int kSpectrumBins = 256;
using SpectrumFrame = std::array<float, kSpectrumBins>;
using SpectrumBus = pulp::runtime::TripleBuffer<SpectrumFrame>;

/// A snapshot of the GPU engine, taken under a SINGLE lock so the fields cannot disagree
/// across a repaint. `budget_us` is how long one GPU block has to finish on THIS device +
/// sample rate, and `rt_percent` is the measured average cost as a percentage of it — so
/// 100 − rt_percent is the headroom left. UI/main-thread only.
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

/// Everything the editor needs from the plugin that is not a parameter and not the
/// spectrum. Four things — and on the web every one of them crosses a thread and a
/// module boundary, which is exactly why they are an interface and not a reference.
///
/// Native (`super_convolver.hpp`) implements this by simply forwarding to itself.
/// The browser implements it from data the page pushes in: the IR arrives over the
/// plugin's `pulp_ir_*` exports, the GPU status over `pulp_ui_set_gpu_status`, and
/// "load this IR" goes back out as a request the page fulfils with a file picker —
/// because a wasm module on a page cannot open one, and must not pretend it can.
struct SuperConvolverUiHost {
    virtual ~SuperConvolverUiHost() = default;

    /// The GPU engine's live counters, or a default-constructed (inactive) status.
    virtual GpuStatus gpu_status() const = 0;

    /// Path (or display name) of the IR currently loaded; empty for the built-in.
    virtual std::string ir_path() const = 0;

    /// Replace the base IR. Natively this reads the file; on the web the module has no
    /// filesystem, so the implementation hands the request to the page.
    virtual void load_ir_path(std::string path) = 0;

    /// The IR the plugin is ACTUALLY convolving with — post-normalize, post-window, not
    /// the source file. This is what the hero waveform draws, which is why it morphs as
    /// Size rebuilds the tail rather than only when a new file is loaded.
    virtual std::vector<float> impulse_response_snapshot() const = 0;
};

}  // namespace pulp::examples

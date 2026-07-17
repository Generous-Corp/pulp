#pragma once

// SuperConvolverUiHost, implemented for a browser tab.
//
// Natively the four host calls are free: the editor and the DSP are one binary, so
// gpu_status() / ir_path() / impulse_response_snapshot() just read the processor and
// load_ir_path() opens a modal dialog. NONE of that is available here. The DSP lives
// in an AudioWorklet (another wasm module, on the audio thread), this module lives on
// the page, and neither can reach into the other — so every one of the four is served
// from a snapshot the PAGE pushed in, and the one that is a COMMAND goes back out to
// the page as a request.
//
// The snapshots are pushed, never pulled, because the page is the only side that can
// see both halves: it owns the adapter that carries the plugin's live IR and stats
// across the worklet boundary. This class is therefore deliberately dumb — it holds
// the last thing it was told and answers with it. Freshness is the page's contract
// (see pulp-ui.js), not a guess made here.

#include "super_convolver_ui_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// "The user asked for a new impulse response." Defined with EM_JS in ui_entry.cpp
// (the module's one outbound command, alongside the param/gesture callbacks) and
// fulfilled by the page opening its own file picker. Declared — not defined — here
// so this header stays a plain header.
extern "C" void pulp_ui_js_request_ir();

namespace pulp::webui {

class SuperConvolverWebHost final : public pulp::examples::SuperConvolverUiHost {
public:
    // ── SuperConvolverUiHost ──────────────────────────────────────────────────
    pulp::examples::GpuStatus gpu_status() const override { return status_; }

    std::string ir_path() const override { return ir_name_; }

    /// The editor calls this with an EMPTY path: in a browser it cannot pick a file
    /// itself, so the call IS the request. The page answers it out-of-band — it
    /// decodes the audio and writes it into the plugin's state — and the new IR
    /// comes back to us the same way every other IR does, through set_ir(). We do
    /// not load anything, and must not pretend to: this module has no filesystem.
    void load_ir_path(std::string) override { pulp_ui_js_request_ir(); }

    std::vector<float> impulse_response_snapshot() const override { return ir_; }

    // ── pushed in from the page ───────────────────────────────────────────────

    /// The plugin's live IR — post-normalize, post-window, as it is being convolved
    /// with — decimated to a peak envelope of at most kMaxIrSamples.
    ///
    /// The decimation is display-lossless and is what keeps the per-frame copy in
    /// impulse_response_snapshot() cheap: paint peak-picks the IR into ONE COLUMN
    /// PER PIXEL over |sample|, so folding each source bucket down to its
    /// largest-magnitude sample first leaves the drawn column maxima identical for
    /// any panel narrower than kMaxIrSamples pixels (they all are, by three orders
    /// of magnitude). A 10 s 48 kHz IR arrives as ~480k floats; copying that every
    /// frame at 60 Hz to draw an 800-px waveform is 115 MB/s of pure waste.
    void set_ir(const float* samples, int count) {
        ir_.clear();
        if (!samples || count <= 0) return;
        const std::size_t n = static_cast<std::size_t>(count);
        if (n <= kMaxIrSamples) {
            ir_.assign(samples, samples + n);
            return;
        }
        ir_.resize(kMaxIrSamples);
        for (std::size_t i = 0; i < kMaxIrSamples; ++i) {
            const std::size_t lo = i * n / kMaxIrSamples;
            std::size_t hi = (i + 1) * n / kMaxIrSamples;
            if (hi <= lo) hi = lo + 1;
            if (hi > n) hi = n;
            float peak = 0.0f;
            for (std::size_t s = lo; s < hi; ++s)
                if (std::abs(samples[s]) > std::abs(peak)) peak = samples[s];
            ir_[i] = peak;   // sign preserved — the envelope is still a waveform
        }
    }

    /// Display name of the loaded IR ("Chapel St-Vitus.wav"); empty = the built-in
    /// synthetic room. This is a NAME, not a path: nothing in the browser can open
    /// it, and the editor only ever prints it.
    void set_ir_name(std::string name) { ir_name_ = std::move(name); }

    /// The page's GPU stats blob, parsed into the struct the editor's interface
    /// asks for. `line` is the same numbers rendered as one sentence — derived from
    /// the parsed status, so the string and the struct cannot disagree.
    void set_gpu_status(pulp::examples::GpuStatus status, std::string line) {
        status_ = std::move(status);
        status_line_ = std::move(line);
    }

    /// The formatted one-line engine readout. The editor's header chip is driven by
    /// the Engine PARAMETER (it is a control, not a readout), so nothing paints this
    /// today; it is kept because the stats blob is the module's only non-parameter
    /// host->UI surface and dropping the formatting would silently narrow the ABI.
    const std::string& status_line() const { return status_line_; }

private:
    // ~16k samples: far more than any panel has pixels, far less than an IR.
    static constexpr std::size_t kMaxIrSamples = 16384;

    pulp::examples::GpuStatus status_{};
    std::string status_line_;
    std::string ir_name_;
    std::vector<float> ir_;
};

}  // namespace pulp::webui

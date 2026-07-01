#pragma once

// NamRuntime — a loaded NAM model of any supported architecture behind one
// streaming-inference surface. The processor's engine machinery (per-channel CPU
// engine, live reload, transfer-curve sweep, dry/wet alignment) is written
// against this type, so it stays architecture-agnostic instead of hard-coding one
// model everywhere. Only feedforward architectures (WaveNet today) have a fused
// GPU kernel, so gpu_eligible() gates the GPU transport; recurrent models (LSTM)
// run on the CPU oracle, which is always available and RT-safe.
//
// Value semantics (holds its models by value) so the processor can keep the
// per-channel copies it already made — the inactive architectures' models stay
// empty, so a copy is cheap.
//
// Per-architecture build toggles: every architecture below can be compiled out so
// a project can build GPU NAM for just the subset it needs (e.g. only A1 WaveNet,
// or only Linear) — a smaller binary and faster compile. Each toggle defaults ON;
// define GPU_NAM_WITH_<ARCH>=0 (CMake: -DGPU_NAM_WITH_<ARCH>=0) to drop that
// architecture's loader + inference. At least one must remain enabled. The full
// shipped build enables all of them.

#ifndef GPU_NAM_WITH_WAVENET
#define GPU_NAM_WITH_WAVENET 1
#endif
#ifndef GPU_NAM_WITH_A2
#define GPU_NAM_WITH_A2 1
#endif
#ifndef GPU_NAM_WITH_CONVNET
#define GPU_NAM_WITH_CONVNET 1
#endif
#ifndef GPU_NAM_WITH_LSTM
#define GPU_NAM_WITH_LSTM 1
#endif
#ifndef GPU_NAM_WITH_LINEAR
#define GPU_NAM_WITH_LINEAR 1
#endif

#if !(GPU_NAM_WITH_WAVENET || GPU_NAM_WITH_A2 || GPU_NAM_WITH_CONVNET || GPU_NAM_WITH_LSTM || GPU_NAM_WITH_LINEAR)
#error "GPU NAM: at least one architecture must be enabled (WaveNet, A2, ConvNet, LSTM, or Linear)"
#endif

#if GPU_NAM_WITH_WAVENET
#include "nam_model.hpp"
#endif
#if GPU_NAM_WITH_A2
#include "nam_a2.hpp"
#endif
#if GPU_NAM_WITH_CONVNET
#include "nam_convnet.hpp"
#endif
#if GPU_NAM_WITH_LSTM
#include "nam_lstm.hpp"
#endif
#if GPU_NAM_WITH_LINEAR
#include "nam_linear.hpp"
#endif

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include <choc/text/choc_JSON.h>

namespace pulp::examples::nam {

class NamRuntime {
public:
    // Enum tags are always present (they name architectures regardless of which
    // are compiled in); a disabled architecture simply can never be produced by
    // the loader, which reports it as not built in this configuration.
    enum class Arch { None, WaveNet, WaveNetA2, ConvNet, Lstm, Linear };

    NamRuntime() = default;

    bool ok() const { return arch_ != Arch::None; }
    Arch arch() const { return arch_; }
    const char* arch_name() const {
        switch (arch_) {
            case Arch::WaveNet:   return "WaveNet";
            case Arch::WaveNetA2: return "WaveNet-A2";
            case Arch::ConvNet:   return "ConvNet";
            case Arch::Lstm:      return "LSTM";
            case Arch::Linear:    return "Linear";
            default:              return "none";
        }
    }
    double sample_rate() const {
        switch (arch_) {
#if GPU_NAM_WITH_LSTM
            case Arch::Lstm:   return lstm_.sample_rate();
#endif
#if GPU_NAM_WITH_LINEAR
            case Arch::Linear: return linear_.sample_rate();
#endif
#if GPU_NAM_WITH_A2
            case Arch::WaveNetA2: return a2_.sample_rate();
#endif
#if GPU_NAM_WITH_CONVNET
            case Arch::ConvNet: return convnet_.sample_rate();
#endif
#if GPU_NAM_WITH_WAVENET
            case Arch::WaveNet: return wavenet_.sample_rate();
#endif
            default: return -1.0;
        }
    }

    void reset() {
#if GPU_NAM_WITH_WAVENET
        if (arch_ == Arch::WaveNet) { wavenet_.reset(); return; }
#endif
#if GPU_NAM_WITH_A2
        if (arch_ == Arch::WaveNetA2) { a2_.reset(); return; }
#endif
#if GPU_NAM_WITH_CONVNET
        if (arch_ == Arch::ConvNet) { convnet_.reset(); return; }
#endif
#if GPU_NAM_WITH_LSTM
        if (arch_ == Arch::Lstm) { lstm_.reset(); return; }
#endif
#if GPU_NAM_WITH_LINEAR
        if (arch_ == Arch::Linear) { linear_.reset(); return; }
#endif
    }

    // Settle at the silence steady-state so the first live block matches the
    // reference (which prewarms on load) instead of a cold-start DC transient.
    // Off the audio thread only — runs a receptive-field's worth of silence.
    void prewarm() {
#if GPU_NAM_WITH_WAVENET
        if (arch_ == Arch::WaveNet) { wavenet_.prewarm(); return; }
#endif
#if GPU_NAM_WITH_A2
        if (arch_ == Arch::WaveNetA2) { a2_.prewarm(); return; }
#endif
#if GPU_NAM_WITH_CONVNET
        if (arch_ == Arch::ConvNet) { convnet_.prewarm(); return; }
#endif
#if GPU_NAM_WITH_LSTM
        if (arch_ == Arch::Lstm) { lstm_.prewarm(); return; }
#endif
#if GPU_NAM_WITH_LINEAR
        if (arch_ == Arch::Linear) { linear_.prewarm(); return; }
#endif
    }

    // One mono sample in → one out. Pass-through when no model is loaded, so a
    // failed load degrades to dry rather than silence.
    float process_sample(float x) {
#if GPU_NAM_WITH_WAVENET
        if (arch_ == Arch::WaveNet) return wavenet_.process_sample(x);
#endif
#if GPU_NAM_WITH_A2
        if (arch_ == Arch::WaveNetA2) return a2_.process_sample(x);
#endif
#if GPU_NAM_WITH_CONVNET
        if (arch_ == Arch::ConvNet) return convnet_.process_sample(x);
#endif
#if GPU_NAM_WITH_LSTM
        if (arch_ == Arch::Lstm) return lstm_.process_sample(x);
#endif
#if GPU_NAM_WITH_LINEAR
        if (arch_ == Arch::Linear) return linear_.process_sample(x);
#endif
        return x;
    }

    void process(const float* in, float* out, std::uint32_t n) {
#if GPU_NAM_WITH_WAVENET
        if (arch_ == Arch::WaveNet) { wavenet_.process(in, out, n); return; }
#endif
#if GPU_NAM_WITH_A2
        if (arch_ == Arch::WaveNetA2) { a2_.process(in, out, n); return; }
#endif
#if GPU_NAM_WITH_CONVNET
        if (arch_ == Arch::ConvNet) { convnet_.process(in, out, n); return; }
#endif
#if GPU_NAM_WITH_LSTM
        if (arch_ == Arch::Lstm) { lstm_.process(in, out, n); return; }
#endif
#if GPU_NAM_WITH_LINEAR
        if (arch_ == Arch::Linear) { linear_.process(in, out, n); return; }
#endif
        for (std::uint32_t i = 0; i < n; ++i) out[i] = in[i];
    }

    // Only feedforward architectures with a fused GPU forward are GPU-eligible.
    // WaveNet is today; recurrent LSTM (and, until its GPU path lands, Linear)
    // run CPU-only.
    bool gpu_eligible() const {
#if GPU_NAM_WITH_WAVENET
        return arch_ == Arch::WaveNet;
#else
        return false;
#endif
    }
#if GPU_NAM_WITH_WAVENET
    // Non-null iff WaveNet — the GPU node uploads this exact NamModel's weights.
    // Only declared when WaveNet is compiled in; the GPU stack (its sole caller)
    // is likewise WaveNet-gated, so a WaveNet-off build never references it. There
    // is deliberately no type-erased fallback — returning a void* would let a
    // dereference type-check away the guarantee that GPU == WaveNet.
    const NamModel* wavenet() const { return arch_ == Arch::WaveNet ? &wavenet_ : nullptr; }
#endif

    const std::string& error() const { return error_; }

    friend bool load_nam_runtime(const std::string& path, NamRuntime& out, std::string* error);

private:
    Arch arch_ = Arch::None;
#if GPU_NAM_WITH_WAVENET
    NamModel wavenet_;
#endif
#if GPU_NAM_WITH_A2
    NamA2 a2_;
#endif
#if GPU_NAM_WITH_CONVNET
    NamConvNet convnet_;
#endif
#if GPU_NAM_WITH_LSTM
    NamLstmModel lstm_;
#endif
#if GPU_NAM_WITH_LINEAR
    NamLinearModel linear_;
#endif
    std::string error_;
};

// Peek the ``architecture`` field and dispatch to the matching loader. Returns
// false + sets ``error`` (and leaves ``out`` as Arch::None) on any failure —
// including an architecture that is real but compiled out of this build. The
// small .nam header is re-read by the chosen loader — negligible for a one-shot
// load off the audio thread.
inline bool load_nam_runtime(const std::string& path, NamRuntime& out, std::string* error) {
    auto fail = [&](const std::string& msg) {
        out.arch_ = NamRuntime::Arch::None;
        out.error_ = msg;
        if (error) *error = msg;
        return false;
    };

    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("could not open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return fail("empty file: " + path);

    std::string architecture;
    bool a2_shaped = false;
    try {
        const choc::value::Value root = choc::json::parse(text);
        if (root.isObject() && root.hasObjectMember("architecture"))
            architecture = std::string(root["architecture"].getString());
        // A2 shares the "WaveNet" string but a SlimmableContainer / per-layer
        // kernel_sizes / windowed head distinguish it, so classify before routing.
#if GPU_NAM_WITH_A2
        a2_shaped = is_nam_a2(root);
#endif
    } catch (const std::exception& e) {
        return fail(std::string("JSON parse error: ") + e.what());
    }

    std::string err;
    // A2 (SlimmableContainer, or an A2-shaped WaveNet) takes priority over the A1
    // WaveNet loader, which shares the architecture string but rejects the shape.
    if (a2_shaped) {
#if GPU_NAM_WITH_A2
        if (!load_nam_a2(path, out.a2_, &err)) return fail(err);
        out.arch_ = NamRuntime::Arch::WaveNetA2;
        out.error_.clear();
        return true;
#else
        return fail("architecture 'WaveNet-A2' is not compiled into this build");
#endif
    }
#if GPU_NAM_WITH_WAVENET
    if (architecture == "WaveNet") {
        if (!load_nam(path, out.wavenet_, &err)) return fail(err);
        out.arch_ = NamRuntime::Arch::WaveNet;
        out.error_.clear();
        return true;
    }
#endif
#if GPU_NAM_WITH_LSTM
    if (architecture == "LSTM") {
        if (!load_nam_lstm(path, out.lstm_, &err)) return fail(err);
        out.arch_ = NamRuntime::Arch::Lstm;
        out.error_.clear();
        return true;
    }
#endif
#if GPU_NAM_WITH_LINEAR
    if (architecture == "Linear") {
        if (!load_nam_linear(path, out.linear_, &err)) return fail(err);
        out.arch_ = NamRuntime::Arch::Linear;
        out.error_.clear();
        return true;
    }
#endif
#if GPU_NAM_WITH_CONVNET
    if (architecture == "ConvNet") {
        if (!load_nam_convnet(path, out.convnet_, &err)) return fail(err);
        out.arch_ = NamRuntime::Arch::ConvNet;
        out.error_.clear();
        return true;
    }
#endif
    // A known architecture that is compiled out lands here with a clear message.
    if (architecture == "WaveNet" || architecture == "LSTM" || architecture == "Linear"
        || architecture == "ConvNet" || architecture == "SlimmableContainer")
        return fail("architecture '" + architecture + "' is not compiled into this build");
    return fail("unsupported architecture: '" + architecture + "'");
}

}  // namespace pulp::examples::nam

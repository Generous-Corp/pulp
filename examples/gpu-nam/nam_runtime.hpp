#pragma once

// NamRuntime — a loaded NAM model of any supported architecture (WaveNet or
// LSTM) behind one streaming-inference surface. The processor's engine machinery
// (per-channel CPU engine, live reload, transfer-curve sweep, dry/wet alignment)
// is written against this type, so it stays architecture-agnostic instead of
// hard-coding NamModel (WaveNet) everywhere. Only WaveNet has a fused GPU kernel
// today, so gpu_eligible() gates the GPU transport; LSTM runs on the CPU oracle,
// which is always available and RT-safe.
//
// Value semantics (holds its models by value) so the processor can keep the
// per-channel copies it already made with NamModel — the inactive architecture's
// model stays empty, so a copy is cheap.

#include "nam_lstm.hpp"
#include "nam_model.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include <choc/text/choc_JSON.h>

namespace pulp::examples::nam {

class NamRuntime {
public:
    enum class Arch { None, WaveNet, Lstm };

    NamRuntime() = default;

    bool ok() const { return arch_ != Arch::None; }
    Arch arch() const { return arch_; }
    const char* arch_name() const {
        switch (arch_) {
            case Arch::WaveNet: return "WaveNet";
            case Arch::Lstm:    return "LSTM";
            default:            return "none";
        }
    }
    double sample_rate() const {
        return arch_ == Arch::Lstm ? lstm_.sample_rate() : wavenet_.sample_rate();
    }

    void reset() {
        if (arch_ == Arch::WaveNet) wavenet_.reset();
        else if (arch_ == Arch::Lstm) lstm_.reset();
    }

    // Settle at the silence steady-state so the first live block matches the
    // reference (which prewarms on load) instead of a cold-start DC transient.
    // Off the audio thread only — runs a receptive-field's worth of silence.
    void prewarm() {
        if (arch_ == Arch::WaveNet) wavenet_.prewarm();
        else if (arch_ == Arch::Lstm) lstm_.prewarm();
    }

    // One mono sample in → one out. Pass-through when no model is loaded, so a
    // failed load degrades to dry rather than silence.
    float process_sample(float x) {
        if (arch_ == Arch::WaveNet) return wavenet_.process_sample(x);
        if (arch_ == Arch::Lstm) return lstm_.process_sample(x);
        return x;
    }

    void process(const float* in, float* out, std::uint32_t n) {
        if (arch_ == Arch::WaveNet) { wavenet_.process(in, out, n); return; }
        if (arch_ == Arch::Lstm) { lstm_.process(in, out, n); return; }
        for (std::uint32_t i = 0; i < n; ++i) out[i] = in[i];
    }

    // Only WaveNet has a fused GPU forward; LSTM (recurrent) runs CPU-only.
    bool gpu_eligible() const { return arch_ == Arch::WaveNet; }
    // Non-null iff WaveNet — the GPU node uploads this exact NamModel's weights.
    const NamModel* wavenet() const { return arch_ == Arch::WaveNet ? &wavenet_ : nullptr; }

    const std::string& error() const { return error_; }

    friend bool load_nam_runtime(const std::string& path, NamRuntime& out, std::string* error);

private:
    Arch arch_ = Arch::None;
    NamModel wavenet_;
    NamLstmModel lstm_;
    std::string error_;
};

// Peek the ``architecture`` field and dispatch to the matching loader. Returns
// false + sets ``error`` (and leaves ``out`` as Arch::None) on any failure. The
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
    try {
        const choc::value::Value root = choc::json::parse(text);
        if (root.isObject() && root.hasObjectMember("architecture"))
            architecture = std::string(root["architecture"].getString());
    } catch (const std::exception& e) {
        return fail(std::string("JSON parse error: ") + e.what());
    }

    std::string err;
    if (architecture == "WaveNet") {
        if (!load_nam(path, out.wavenet_, &err)) return fail(err);
        out.arch_ = NamRuntime::Arch::WaveNet;
        out.error_.clear();
        return true;
    }
    if (architecture == "LSTM") {
        if (!load_nam_lstm(path, out.lstm_, &err)) return fail(err);
        out.arch_ = NamRuntime::Arch::Lstm;
        out.error_.clear();
        return true;
    }
    // ConvNet, Linear, and the experimental WaveNet variants (grouped convs,
    // FiLM conditioning, head1x1, SlimmableContainer) are not modeled — the
    // architecture-specific loader below would also reject their shape.
    return fail("unsupported architecture: '" + architecture + "' (supported: WaveNet, LSTM)");
}

}  // namespace pulp::examples::nam

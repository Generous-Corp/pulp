#pragma once

// Verdict logic for the Vellum-backed GPU adapter probe. The probe renders a
// fixed scene on the installed Vellum GPU runtime and hands the adapter
// identity and the captured frame here. Nothing in this header names a Vellum
// or Dawn type, so the rules are testable without a GPU.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::tooling::gpu_adapter {

/// What the GPU runtime reports about the device a frame was drawn on.
struct Evidence {
    bool available = false;
    bool native_surface = false;
    bool fallback = false;
    std::string renderer;
    std::string backend;
    std::string adapter;
    std::string texture_format;
};

/// A captured frame: RGBA8, row-major, top row first.
struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

/// One interior pixel of the probe scene and the colour it must have.
struct Swatch {
    std::uint32_t x;
    std::uint32_t y;
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

/// Size of the probe scene in device pixels at scale 1.
inline constexpr std::uint32_t kSceneWidth = 120;
inline constexpr std::uint32_t kSceneHeight = 80;
/// Largest per-channel difference an expected swatch tolerates.
inline constexpr int kChannelTolerance = 3;

/// Background, a teal card and an amber card, each sampled inside its fill.
inline constexpr std::array<Swatch, 3> kSwatches{{
    {2, 2, 0x0F, 0x17, 0x2A},
    {30, 25, 0x14, 0xB8, 0xA6},
    {80, 25, 0xF5, 0x9E, 0x0B},
}};

enum class Verdict { pass, fail, unavailable };

struct Result {
    Verdict verdict = Verdict::unavailable;
    /// Short machine-readable cause: "ok", "gpu-unavailable", "software-fallback",
    /// "empty-adapter", "empty-renderer", "no-frame", "oracle-mismatch".
    std::string reason;
    Evidence evidence;
    std::uint32_t swatches_checked = 0;
    std::uint32_t swatches_mismatched = 0;
};

/// Judge a probe run. A device that never came up is `unavailable`; a device
/// that came up but cannot name its adapter, is a software fallback, or drew
/// the wrong pixels is `fail`. `frame` is null when no frame was captured.
Result classify(const Evidence& evidence, const Frame* frame);

/// Process exit status for a verdict: 0 pass, 1 fail, 2 unavailable.
int exit_code(Verdict verdict);

/// One-line JSON object, schema `pulp.gpu-adapter-probe.v1`.
std::string to_json(const Result& result);

} // namespace pulp::tooling::gpu_adapter

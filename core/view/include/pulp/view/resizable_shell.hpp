#pragma once

// ResizableShell — plugin-window size constraints + aspect-ratio lock +
// saved-size persistence (workstream 07 slice 7.5).
//
// Plugin shells (VST3 IPlugViewContentScaleSupport, AU fit-in-window,
// CLAP clap_plugin_gui.adjust_size) all need the same three behaviours:
//  1. Clamp requested sizes to a min/max.
//  2. Optionally lock to an aspect ratio so resize handles move in step.
//  3. Round-trip the last accepted size across session reloads.
//
// This component owns the arithmetic. Format adapters delegate to it
// from their native resize callbacks and persist the last size blob
// alongside parameter state.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pulp::view {

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const Size& o) const {
        return width == o.width && height == o.height;
    }
};

struct ResizableShellConfig {
    Size min_size{200, 150};
    Size max_size{4096, 4096};
    Size initial_size{640, 480};
    /// When > 0, aspect ratio = width / height is held during resize.
    /// Typical values: 16.0/9.0, 4.0/3.0, initial_size.width/height.
    double aspect_ratio = 0.0;
};

class ResizableShell {
public:
    /// Construct with a config. `current()` is seeded by passing
    /// `cfg.initial_size` through negotiate(), so a misconfigured
    /// initial (outside min/max or violating aspect) reports the
    /// clamped size from the very first call — callers can never
    /// observe an invalid state. Fix per #206 review.
    explicit ResizableShell(ResizableShellConfig cfg = {})
        : cfg_(cfg), current_(negotiate_static_(cfg, cfg.initial_size)) {}

    /// Intended size → clamped to [min, max] and (when aspect is locked)
    /// snapped to the nearest aspect-correct rectangle.
    Size negotiate(Size requested) const;

    /// Commit the new size. Returns the clamped size actually applied.
    Size apply(Size requested) {
        current_ = negotiate(requested);
        return current_;
    }

    Size current() const { return current_; }
    const ResizableShellConfig& config() const { return cfg_; }

    /// Serialize the saved size to 8 bytes (little-endian u32 w, h).
    /// Plugins concatenate this with their parameter blob so the host's
    /// state round-trip restores the window size too.
    std::vector<uint8_t> serialize() const;

    /// Restore from a blob produced by serialize(). Returns false on
    /// short buffer or malformed payload. On success the size is also
    /// renegotiated through the current config so out-of-range saves
    /// (e.g. exported from a wider min/max) are clamped cleanly.
    bool deserialize(std::span<const uint8_t> blob);

private:
    // Free-function version of negotiate used by the ctor (negotiate()
    // depends on cfg_ which isn't initialised yet at member-init time).
    static Size negotiate_static_(const ResizableShellConfig& cfg, Size requested);

    ResizableShellConfig cfg_;
    Size current_;
};

}  // namespace pulp::view

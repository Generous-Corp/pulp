#include <pulp/view/resizable_shell.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::view {

Size ResizableShell::negotiate_static_(const ResizableShellConfig& cfg,
                                       Size requested) {
    // Keep implementation in sync with the member negotiate() — the ctor
    // needs a version that doesn't depend on cfg_ being initialised.
    uint32_t w = std::clamp(requested.width, cfg.min_size.width, cfg.max_size.width);
    uint32_t h = std::clamp(requested.height, cfg.min_size.height, cfg.max_size.height);
    if (cfg.aspect_ratio <= 0.0) return {w, h};
    const double target = cfg.aspect_ratio;
    const double cur = static_cast<double>(w) / std::max<uint32_t>(h, 1);
    if (cur > target)      w = static_cast<uint32_t>(std::round(h * target));
    else if (cur < target) h = static_cast<uint32_t>(std::round(w / target));
    if (w < cfg.min_size.width)  { w = cfg.min_size.width;  h = static_cast<uint32_t>(std::round(w / target)); }
    if (h < cfg.min_size.height) { h = cfg.min_size.height; w = static_cast<uint32_t>(std::round(h * target)); }
    if (w > cfg.max_size.width || h > cfg.max_size.height) {
        w = std::min(w, cfg.max_size.width);
        h = std::min(h, cfg.max_size.height);
    }
    return {w, h};
}

Size ResizableShell::negotiate(Size requested) const {
    // Clamp to min/max first. This is the bound every plugin adapter
    // must honour regardless of aspect lock.
    uint32_t w = std::clamp(requested.width,
                            cfg_.min_size.width,
                            cfg_.max_size.width);
    uint32_t h = std::clamp(requested.height,
                            cfg_.min_size.height,
                            cfg_.max_size.height);

    if (cfg_.aspect_ratio <= 0.0) {
        return {w, h};
    }

    // Aspect-lock: snap to the nearest ratio-correct rectangle. Pick the
    // dimension that shrank the most (relative to the target ratio) and
    // rebuild the other from it so the window never grows beyond the
    // user's drag.
    const double target = cfg_.aspect_ratio;
    const double current = static_cast<double>(w) / std::max<uint32_t>(h, 1);

    if (current > target) {
        // Width is too large; shrink it to match h*target.
        w = static_cast<uint32_t>(std::round(h * target));
    } else if (current < target) {
        // Height is too large; shrink it to match w/target.
        h = static_cast<uint32_t>(std::round(w / target));
    }

    // The snap may have pushed a dimension below the min; re-clamp and
    // rebuild the other side from the clamped value.
    if (w < cfg_.min_size.width) {
        w = cfg_.min_size.width;
        h = static_cast<uint32_t>(std::round(w / target));
    }
    if (h < cfg_.min_size.height) {
        h = cfg_.min_size.height;
        w = static_cast<uint32_t>(std::round(h * target));
    }
    // The min-correction above can push the rebuilt dimension beyond
    // max. Re-apply the max clamp so the advertised bounds always win
    // over the aspect lock. Without this, negotiate({210,160}) with
    // min={200,150}, max={210,160}, aspect=2.0 returned {300,150} —
    // outside max. Fix per #206 review.
    if (w > cfg_.max_size.width || h > cfg_.max_size.height) {
        w = std::min(w, cfg_.max_size.width);
        h = std::min(h, cfg_.max_size.height);
    }
    return {w, h};
}

std::vector<uint8_t> ResizableShell::serialize() const {
    std::vector<uint8_t> out(8);
    uint32_t w = current_.width;
    uint32_t h = current_.height;
    for (int i = 0; i < 4; ++i) out[i]     = static_cast<uint8_t>((w >> (i * 8)) & 0xFF);
    for (int i = 0; i < 4; ++i) out[i + 4] = static_cast<uint8_t>((h >> (i * 8)) & 0xFF);
    return out;
}

bool ResizableShell::deserialize(std::span<const uint8_t> blob) {
    if (blob.size() < 8) return false;
    uint32_t w = 0, h = 0;
    for (int i = 0; i < 4; ++i) {
        w |= static_cast<uint32_t>(blob[i]) << (i * 8);
        h |= static_cast<uint32_t>(blob[i + 4]) << (i * 8);
    }
    current_ = negotiate({w, h});
    return true;
}

}  // namespace pulp::view

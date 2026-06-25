// Settle-probe responsive auto-sizing for imported designs.
//
// Header-only, pure-Yoga (no paint) helper that resolves the natural design
// viewport for an imported view tree when the user did NOT pass --size and the
// design did not self-declare a `globalThis.__pulpDesignViewport__`.
//
// Problem it fixes: ui-preview used to lay the imported tree out ONCE at the
// requested render width (default 360) and measure the content extent — so a
// fill-width design (flex-grow children with no intrinsic width, e.g. the
// Chainer bundle) collapsed into a 360px portrait column, because 360 was the
// only width it ever saw. The settle-probe instead lays the tree out at a
// GENEROUS width and binary-searches for the narrowest width at which the
// layout "settles" (content height stops shrinking) — the design's natural
// max-content width. Mirrors the design-tool resolver in examples/design-tool/main.cpp
// so the two hosts size imports identically.
//
// Deterministic: pure Yoga layout, runs once at startup. Extracted to a header
// so it can be unit-tested headlessly (test/test_ui_preview_viewport.cpp)
// without linking the ui-preview executable.

#pragma once

#include <pulp/view/view.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace pulp::ui_preview {

struct DesignViewport {
    float width = 0.0f;
    float height = 0.0f;
    bool responsive = false;     ///< true when narrow vs wide reflowed (wrap)
    float h_wide = 0.0f;         ///< probe height at the wide probe (debug)
    float h_narrow = 0.0f;       ///< probe height at the narrow probe (debug)
    bool reliable = true;        ///< false → caller should use a fallback
};

/// Resolve the natural design viewport for `root` via the settle-probe.
/// Mutates `root`'s bounds as a side effect of the layout passes (the caller
/// re-lays-out at the final design size afterwards). Pure layout, no paint.
inline DesignViewport probe_design_viewport(
    pulp::view::View& root,
    float min_dim = 240.0f,
    float max_dim = 4096.0f,
    float fallback_w = 1280.0f,
    float fallback_h = 720.0f) {
    using pulp::view::View;

    constexpr float kWideProbe = 4000.0f;
    constexpr float kNarrowProbe = 200.0f;
    constexpr float kSettlingTolerance = 4.0f;   // px slack vs h_wide
    constexpr float kBinarySearchEpsilon = 16.0f;

    // Lay the WHOLE tree out at probe_w and return the deepest child bottom
    // edge (captures absolutely-positioned overflow like a footer / export
    // bar that sits below its flex wrapper).
    auto probe_height = [&root](float probe_w) -> float {
        root.set_bounds({0.0f, 0.0f, probe_w, 99999.0f});
        root.layout_children();
        float h = 0.0f;
        std::function<void(const View*, float)> walk =
            [&](const View* v, float parent_abs_y) {
                if (!v) return;
                for (std::size_t i = 0; i < v->child_count(); ++i) {
                    const View* c = v->child_at(i);
                    if (!c || !c->visible()) continue;
                    const auto b = c->bounds();
                    const float child_abs_y = parent_abs_y + b.y;
                    h = std::max(h, child_abs_y + b.height);
                    walk(c, child_abs_y);
                }
            };
        walk(&root, 0.0f);
        return h;
    };

    DesignViewport out;
    out.h_wide = probe_height(kWideProbe);
    out.h_narrow = probe_height(kNarrowProbe);
    out.responsive = (out.h_narrow > out.h_wide + kSettlingTolerance);

    float settled_w = fallback_w;
    float settled_h = fallback_h;
    if (out.responsive) {
        // Binary-search the smallest width where height stays ~ h_wide
        // (layout fully settled — no further reflow rows).
        float lo = kNarrowProbe, hi = kWideProbe;
        while (hi - lo > kBinarySearchEpsilon) {
            const float mid = (lo + hi) * 0.5f;
            if (probe_height(mid) > out.h_wide + kSettlingTolerance) lo = mid;
            else                                                     hi = mid;
        }
        settled_w = hi;
        settled_h = probe_height(settled_w);
    } else {
        // Non-responsive (fixed-size design): measure the rightmost child edge
        // at a generous width so the true authored width is reported, not the
        // requested render width.
        root.set_bounds({0.0f, 0.0f, kWideProbe, 99999.0f});
        root.layout_children();
        float content_right = 0.0f;
        std::function<void(const View*, float)> measure_w =
            [&](const View* v, float parent_abs_x) {
                if (!v) return;
                for (std::size_t i = 0; i < v->child_count(); ++i) {
                    const View* c = v->child_at(i);
                    if (!c || !c->visible()) continue;
                    const auto b = c->bounds();
                    const float child_abs_x = parent_abs_x + b.x;
                    content_right = std::max(content_right, child_abs_x + b.width);
                    measure_w(c, child_abs_x);
                }
            };
        measure_w(&root, 0.0f);
        settled_w = (content_right >= min_dim && content_right < max_dim)
                        ? content_right
                        : fallback_w;
        settled_h = probe_height(settled_w);
    }

    const bool w_ok = settled_w >= 32.0f && settled_w < max_dim;
    const bool h_ok = settled_h >= 32.0f && settled_h < max_dim;
    if (w_ok && h_ok) {
        out.width = std::clamp(std::ceil(settled_w), min_dim, max_dim);
        out.height = std::clamp(std::ceil(settled_h), min_dim, max_dim);
        out.reliable = true;
    } else {
        out.width = fallback_w;
        out.height = fallback_h;
        out.reliable = false;
    }
    return out;
}

}  // namespace pulp::ui_preview

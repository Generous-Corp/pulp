#pragma once

/// @file svg_dom_cache.hpp
/// Process-wide cache for parsed SVG documents (SkSVGDOM).
///
/// `Canvas::draw_svg` parses its SVG string into an `SkSVGDOM` every call
/// (`SkSVGDOM::Builder().make(stream)`). For a large faithful design frame —
/// the faithful-vector design-import lane's `DesignFrameView`, which can carry
/// a 100+ KB SVG — rebuilding that DOM every repaint is wasteful when the
/// document string has not changed: a static design frame, a frame that only
/// has native-overlay children animating, or a high-refresh repaint of
/// unchanged chrome.
///
/// This cache stores the parsed DOM keyed on the *exact* SVG document bytes
/// (FNV-1a hash of the string, plus the length, with a stored copy of the
/// source string to defend against hash collisions). The render itself —
/// `dom->render(canvas_)` after `setContainerSize` / scale — is unchanged, so
/// the rasterized output is byte-identical to the uncached path; only the parse
/// step is skipped on a hit.
///
/// Crucially the key is the document content, NOT a logical asset id. The
/// `DesignFrameView` render-patch path (knob needle rotate, fader / xy_pad
/// thumb translate, toggle-switch slide) mutates the SVG string *before*
/// handing it to `draw_svg` — wrapping the patched element in a `<g
/// transform=...>`. A patched string is therefore a different document, so it
/// misses the cache and is re-parsed. That is exactly what keeps a dragged knob
/// live: each value produces a distinct document, each rebuilds its DOM. The
/// cache only ever reuses a DOM when the bytes are identical, so it can never
/// freeze a moving needle. A size change reuses the same DOM and re-applies the
/// scale at draw time (the scale lives in the SkCanvas transform, not the DOM),
/// so resizing is also a cache hit — correctly.
///
/// Painting is on the UI thread (not the realtime audio thread), so a small
/// mutex is acceptable; the cache is process-global with an LRU cap. SkSVGDOM
/// is immutable once built and `render()` only reads it, so a shared `sk_sp`
/// across frames is safe.

#ifdef PULP_HAS_SKIA

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "include/core/SkRefCnt.h"

class SkSVGDOM;

namespace pulp::canvas {

class SvgDomCache {
public:
    static SvgDomCache& instance();

    /// Return the parsed DOM for @p svg_document, parsing it on a miss (via
    /// SkSVGDOM::Builder over the document bytes) and storing the result. The key
    /// is the full document string, so a patched (render-patched) document is a
    /// distinct key and is re-parsed — keeping live overlays live. Counts hits
    /// and builds. A null parse result is returned but not stored (so a transient
    /// parse failure does not poison the cache). When disabled, always re-parses
    /// and stores nothing.
    sk_sp<SkSVGDOM> get_or_build(const std::string& svg_document);

    /// Same, but with a caller-supplied build closure (for tests that want to
    /// inject a parser, or callers that already hold a stream). The closure runs
    /// only on a cache miss.
    sk_sp<SkSVGDOM> get_or_build(
        const std::string& svg_document,
        const std::function<sk_sp<SkSVGDOM>()>& build);

    struct Stats {
        std::uint64_t hits = 0;    ///< Cache hits (DOM rebuild skipped).
        std::uint64_t builds = 0;  ///< Cache misses (DOM rebuilt).
        std::size_t size = 0;      ///< Live entries.
    };
    Stats stats() const;
    void reset_stats();
    void clear();

    /// Toggle caching globally. When false, get_or_build still works but always
    /// rebuilds and stores nothing (used to A/B against the uncached path and to
    /// keep the golden / micro-benchmark harness deterministic).
    void set_enabled(bool enabled);
    bool enabled() const;

    void set_capacity(std::size_t capacity);

private:
    SvgDomCache() = default;
    struct Impl;
    // Defined in svg_dom_cache.cpp; stateful members live there to keep this
    // header free of <mutex>/<list>/<unordered_map> and the full SkSVGDOM type.
    static Impl& impl();
};

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA

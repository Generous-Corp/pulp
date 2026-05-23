#pragma once

// atlas_inventory.hpp — read-only GPU texture-atlas inventory.
//
// Phase 6.2 of the inspector GPU-perf roadmap
// (planning/2026-05-19-inspector-phase6-gpu-perf-spike.md § Phase 6.2).
//
// The render layer owns several shelf-packed texture atlases — the
// glyph atlas (SDF text), the image atlas, the gradient ramp atlas,
// and the path atlas (texture_atlas.hpp). Each one is a GPU texture
// page that small bitmaps are packed into; when an atlas fills up,
// rendering either thrashes (evict + re-pack churn) or spills to a
// fresh page. "Is my SDF atlas thrashing?" is a real perf question.
//
// `AtlasInventory` is the answer surface: a small, render-layer-owned
// collection of `AtlasInfo` snapshots that the inspector overlay's
// atlas viewer renders. It deliberately holds *value snapshots*, not
// pointers to live atlases — the inspector reads it on the UI thread
// while the render thread keeps mutating the real atlases, so a
// snapshot taken at a known point is the safe contract.
//
// What it surfaces is exactly what the Pulp-owned atlas classes can
// honestly report: per-atlas pixel dimensions, a shelf-packer
// occupancy estimate, the live entry (packed-region) count, and a
// page count. Skia manages its own internal SkStrike glyph cache
// separately and does not expose page-level introspection through a
// stable public API; this inventory does NOT fabricate numbers for
// it — it reports the atlases Pulp itself packs.

#include <pulp/render/texture_atlas.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace pulp::render {

/// Which kind of atlas an AtlasInfo describes. Mirrors the atlas
/// classes in texture_atlas.hpp so the inspector can label rows and
/// (later) pick per-kind colors.
enum class AtlasKind {
    glyph,     ///< GlyphAtlas — SDF / custom-text glyph cache.
    image,     ///< ImageAtlas — packed small images.
    gradient,  ///< GradientAtlas — evaluated gradient ramps (row-packed).
    path,      ///< PathAtlas — rasterized vector paths.
};

/// Human-readable name for an AtlasKind — used by the inspector panel
/// and by tests.
inline const char* atlas_kind_name(AtlasKind kind) {
    switch (kind) {
        case AtlasKind::glyph:    return "glyph";
        case AtlasKind::image:    return "image";
        case AtlasKind::gradient: return "gradient";
        case AtlasKind::path:     return "path";
    }
    return "unknown";
}

/// A value-snapshot of one texture atlas's state, taken at a known
/// point. Everything here is a plain copy — safe to read on the UI
/// thread after the render thread has moved on.
struct AtlasInfo {
    AtlasKind kind = AtlasKind::image;
    std::string label;       ///< Display label (defaults to the kind name).
    int width = 0;           ///< Atlas page width in texels.
    int height = 0;          ///< Atlas page height in texels.
    int pages = 1;           ///< Number of GPU texture pages.
    std::size_t entries = 0; ///< Live packed regions (glyphs/images/etc).
    float occupancy = 0.0f;  ///< Shelf-packer fill fraction, [0, 1].

    /// Total addressable texels across every page.
    std::size_t texel_capacity() const {
        const int safe_width = width < 0 ? 0 : width;
        const int safe_height = height < 0 ? 0 : height;
        return static_cast<std::size_t>(safe_width) *
               static_cast<std::size_t>(safe_height) *
               static_cast<std::size_t>(pages < 1 ? 1 : pages);
    }

    /// Occupancy clamped to [0, 1] and expressed as a 0-100 percentage,
    /// rounded to the nearest integer — the form the panel prints.
    int occupancy_percent() const {
        float clamped = occupancy;
        if (clamped < 0.0f) clamped = 0.0f;
        if (clamped > 1.0f) clamped = 1.0f;
        return static_cast<int>(clamped * 100.0f + 0.5f);
    }
};

/// A read-only collection of atlas snapshots for the inspector.
///
/// The render layer (or a test) builds an inventory by calling
/// `snapshot()` on each live atlas; the inspector overlay is then
/// handed a pointer to the inventory and renders one row per entry.
/// The inventory invents no data — every field of every AtlasInfo
/// comes straight from an atlas's own introspection accessors.
class AtlasInventory {
public:
    /// Capture a snapshot of an ImageAtlas / GlyphAtlas / PathAtlas —
    /// any atlas type exposing width()/height()/occupancy()/
    /// entry_count(). `pages` defaults to 1 (Pulp's atlas classes are
    /// single-page today); a caller that spills across pages can pass
    /// the real count.
    template <typename Atlas>
    static AtlasInfo snapshot(const Atlas& atlas, AtlasKind kind,
                              std::string label = {}, int pages = 1) {
        AtlasInfo info;
        info.kind = kind;
        info.label = label.empty() ? atlas_kind_name(kind)
                                    : std::move(label);
        info.width = atlas.width();
        info.height = atlas.height();
        info.pages = pages < 1 ? 1 : pages;
        info.entries = atlas.entry_count();
        info.occupancy = atlas.occupancy();
        return info;
    }

    /// GradientAtlas is row-packed (no width/height) — give it an
    /// explicit dedicated snapshot. The ramp width is a caller-supplied
    /// convention (gradient ramps are typically 256 texels wide); the
    /// page height is the row budget.
    static AtlasInfo snapshot_gradient(const GradientAtlas& atlas,
                                       std::string label = {},
                                       int ramp_width = 256) {
        AtlasInfo info;
        info.kind = AtlasKind::gradient;
        info.label = label.empty() ? atlas_kind_name(AtlasKind::gradient)
                                    : std::move(label);
        info.width = ramp_width < 0 ? 0 : ramp_width;
        info.height = atlas.row_capacity();
        info.pages = 1;
        info.entries = atlas.entry_count();
        info.occupancy = atlas.occupancy();
        return info;
    }

    /// Append a pre-built AtlasInfo snapshot.
    void add(AtlasInfo info) { atlases_.push_back(std::move(info)); }

    /// Snapshot any width/height/occupancy atlas straight into the
    /// inventory.
    template <typename Atlas>
    void add_atlas(const Atlas& atlas, AtlasKind kind,
                   std::string label = {}, int pages = 1) {
        atlases_.push_back(snapshot(atlas, kind, std::move(label), pages));
    }

    /// Snapshot a GradientAtlas straight into the inventory.
    void add_gradient(const GradientAtlas& atlas, std::string label = {},
                      int ramp_width = 256) {
        atlases_.push_back(
            snapshot_gradient(atlas, std::move(label), ramp_width));
    }

    /// Drop every snapshot — call before re-populating each frame the
    /// inspector refreshes.
    void clear() { atlases_.clear(); }

    /// All atlas snapshots, in insertion order.
    const std::vector<AtlasInfo>& atlases() const { return atlases_; }

    /// Number of atlas snapshots held.
    std::size_t size() const { return atlases_.size(); }

    /// True when no atlas has been registered — the inspector renders
    /// the "GPU atlas unavailable" empty state in this case.
    bool empty() const { return atlases_.empty(); }

    /// Total GPU texture pages across every registered atlas.
    int total_pages() const {
        int total = 0;
        for (const auto& a : atlases_)
            total += (a.pages < 1 ? 1 : a.pages);
        return total;
    }

    /// Total live packed regions (glyphs / images / ramps / paths)
    /// across every atlas.
    std::size_t total_entries() const {
        std::size_t total = 0;
        for (const auto& a : atlases_) total += a.entries;
        return total;
    }

    /// Mean occupancy across all atlases, in [0, 1]; 0 when empty.
    float average_occupancy() const {
        if (atlases_.empty()) return 0.0f;
        float sum = 0.0f;
        for (const auto& a : atlases_) {
            float o = a.occupancy;
            if (o < 0.0f) o = 0.0f;
            if (o > 1.0f) o = 1.0f;
            sum += o;
        }
        return sum / static_cast<float>(atlases_.size());
    }

private:
    std::vector<AtlasInfo> atlases_;
};

} // namespace pulp::render

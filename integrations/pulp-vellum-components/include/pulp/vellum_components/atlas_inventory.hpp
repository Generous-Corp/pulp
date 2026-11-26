#pragma once

/// @file atlas_inventory.hpp
/// A read-only inventory of GPU texture atlases for the inspector's atlas
/// viewer: one value snapshot per atlas (its size, page count, how many
/// regions are packed in it and how full it is).
///
/// The inventory holds copies, not pointers to live atlases, so the inspector
/// can read it on the UI thread while the render thread keeps changing the real
/// ones. It invents nothing: a host builds it by snapshotting each atlas it
/// owns, and this header has no dependency on any atlas class. `snapshot()`
/// takes any type with `width()`, `height()`, `entry_count()` and
/// `occupancy()`.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace pulp::vellum_components::overlay {

/// What an atlas stores.
enum class AtlasKind {
    glyph,    ///< SDF or custom-text glyphs
    image,    ///< packed small images
    gradient, ///< evaluated gradient ramps
    path,     ///< rasterized vector paths
};

/// The display name of `kind`.
const char* atlas_kind_name(AtlasKind kind);

/// One atlas at the moment it was snapshotted.
struct AtlasInfo {
    AtlasKind kind = AtlasKind::image;
    std::string label;       ///< shown on the row; the kind's name by default
    int width = 0;           ///< page width in texels
    int height = 0;          ///< page height in texels
    int pages = 1;           ///< GPU texture pages
    std::size_t entries = 0; ///< packed regions
    float occupancy = 0.0f;  ///< fill fraction

    /// Addressable texels across every page.
    std::size_t texel_capacity() const;
    /// Occupancy clamped to [0, 1] as a whole percentage.
    int occupancy_percent() const;
};

class AtlasInventory {
  public:
    /// A snapshot of `atlas`. `pages` below one counts as one, and an empty
    /// `label` becomes the kind's name.
    template <typename Atlas>
    static AtlasInfo snapshot(const Atlas& atlas, AtlasKind kind, std::string label = {},
                              int pages = 1) {
        AtlasInfo info;
        info.kind = kind;
        info.label = label.empty() ? atlas_kind_name(kind) : std::move(label);
        info.width = atlas.width();
        info.height = atlas.height();
        info.pages = pages < 1 ? 1 : pages;
        info.entries = atlas.entry_count();
        info.occupancy = atlas.occupancy();
        return info;
    }

    void add(AtlasInfo info) {
        atlases_.push_back(std::move(info));
    }
    template <typename Atlas>
    void add_atlas(const Atlas& atlas, AtlasKind kind, std::string label = {}, int pages = 1) {
        add(snapshot(atlas, kind, std::move(label), pages));
    }
    /// Drop every snapshot, before repopulating for a refresh.
    void clear() {
        atlases_.clear();
    }

    const std::vector<AtlasInfo>& atlases() const {
        return atlases_;
    }
    std::size_t size() const {
        return atlases_.size();
    }
    /// True when nothing is registered, which the viewer shows as unavailable.
    bool empty() const {
        return atlases_.empty();
    }

    /// Texture pages and packed regions across every atlas.
    int total_pages() const;
    std::size_t total_entries() const;
    /// Mean occupancy, each clamped to [0, 1]; 0 when empty.
    float average_occupancy() const;

  private:
    std::vector<AtlasInfo> atlases_;
};

/// The height of the atlas panel for `rows` atlases: a heading line and, for
/// each atlas, a label line over an occupancy bar. A panel with no rows is the
/// heading and one message line.
float atlas_panel_height(std::size_t rows);

} // namespace pulp::vellum_components::overlay

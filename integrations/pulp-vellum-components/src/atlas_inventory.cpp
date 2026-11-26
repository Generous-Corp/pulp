#include <pulp/vellum_components/atlas_inventory.hpp>

#include <algorithm>

namespace pulp::vellum_components::overlay {

const char* atlas_kind_name(AtlasKind kind) {
    switch (kind) {
    case AtlasKind::glyph:
        return "glyph";
    case AtlasKind::image:
        return "image";
    case AtlasKind::gradient:
        return "gradient";
    case AtlasKind::path:
        return "path";
    }
    return "unknown";
}

std::size_t AtlasInfo::texel_capacity() const {
    return static_cast<std::size_t>(std::max(width, 0)) *
           static_cast<std::size_t>(std::max(height, 0)) *
           static_cast<std::size_t>(std::max(pages, 1));
}

int AtlasInfo::occupancy_percent() const {
    const float clamped = std::clamp(occupancy, 0.0f, 1.0f);
    return static_cast<int>(clamped * 100.0f + 0.5f);
}

int AtlasInventory::total_pages() const {
    int total = 0;
    for (const auto& a : atlases_)
        total += std::max(a.pages, 1);
    return total;
}

std::size_t AtlasInventory::total_entries() const {
    std::size_t total = 0;
    for (const auto& a : atlases_)
        total += a.entries;
    return total;
}

float AtlasInventory::average_occupancy() const {
    if (atlases_.empty())
        return 0.0f;
    float sum = 0.0f;
    for (const auto& a : atlases_)
        sum += std::clamp(a.occupancy, 0.0f, 1.0f);
    return sum / static_cast<float>(atlases_.size());
}

float atlas_panel_height(std::size_t rows) {
    constexpr float kLine = 15.0f;
    constexpr float kHeading = 4.0f + kLine;
    constexpr float kRow = kLine * 2.0f + 6.0f;
    constexpr float kBottom = 8.0f;
    if (rows == 0)
        return kHeading + kLine + kBottom;
    return kHeading + static_cast<float>(rows) * kRow + kBottom;
}

} // namespace pulp::vellum_components::overlay

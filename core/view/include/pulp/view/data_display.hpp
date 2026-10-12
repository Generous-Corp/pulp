#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {
/// Rendering style for widgets.
/// `standard` = interactive widget (arcs, thumbs, fills).
/// `minimal` = design-preview mode (simple shapes matching design tools).
/// `silver` = skeuomorphic chrome body, radial-gradient highlight, indicator
///           notch. Vector all the way — works on CPU raster + GPU Graphite.
///           Used by the figma-import lane as a native-vector alternative to
///           sprite-strip PNGs (no PNG bleed, crisp at any scale).
enum class WidgetRenderStyle { standard, minimal, silver };

/// Generic level, series and bin display seams owned by view-core. Implementations
/// may live in optional providers such as pulp-view-audio.
class LevelDisplay {
  public:
    enum class Orientation { vertical, horizontal };
    virtual ~LevelDisplay();
    virtual void set_levels(float primary, float secondary) = 0;
    virtual void set_level_orientation(Orientation) = 0;
    /// Installs a fill gradient (stops low to high). A background, when given,
    /// replaces the display's own; std::nullopt leaves it untouched.
    virtual void set_level_fill(std::optional<canvas::Color> background,
                                std::vector<canvas::Color> gradient) = 0;
    virtual void clear_level_fill() = 0;
    virtual void set_level_bar_ratio(float ratio) = 0;
    virtual void set_level_render_style(WidgetRenderStyle) = 0;
};

class SeriesDisplay {
  public:
    virtual ~SeriesDisplay();
    virtual void set_series(const float* values, std::size_t count) = 0;
    /// Opaque design-time preview token; the vocabulary belongs to the provider.
    virtual void set_series_preview(std::string_view token) = 0;
};

class BinDisplay {
  public:
    virtual ~BinDisplay();
    /// Values use the producer's convention for units.
    virtual void set_bins(const float* values, std::size_t count) = 0;
};

struct DataDisplayCatalogRow {
    std::string id;
    std::string class_name;
    std::string header;
};

using DataDisplayFactory = std::unique_ptr<View> (*)();

struct DataDisplayRegistration {
    std::string id;
    DataDisplayFactory make = nullptr;
    std::vector<DataDisplayCatalogRow> catalog_rows;
};

void register_data_display(DataDisplayRegistration registration);
bool has_data_display(std::string_view id);
std::unique_ptr<View> make_data_display(std::string_view id);
void clear_data_displays();
std::size_t data_display_registration_count();

} // namespace pulp::view

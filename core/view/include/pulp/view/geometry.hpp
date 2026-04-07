#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pulp::view {

struct Point {
    float x = 0, y = 0;

    Point operator+(const Point& p) const { return {x + p.x, y + p.y}; }
    Point operator-(const Point& p) const { return {x - p.x, y - p.y}; }
    bool operator==(const Point& p) const { return x == p.x && y == p.y; }
};

struct Size {
    float width = 0, height = 0;

    bool operator==(const Size& s) const { return width == s.width && height == s.height; }
    bool is_empty() const { return width <= 0 || height <= 0; }
};

struct Rect {
    float x = 0, y = 0, width = 0, height = 0;

    float right() const { return x + width; }
    float bottom() const { return y + height; }
    Point origin() const { return {x, y}; }
    Size size() const { return {width, height}; }
    Point center() const { return {x + width * 0.5f, y + height * 0.5f}; }

    bool contains(Point p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }

    bool is_empty() const { return width <= 0 || height <= 0; }

    Rect inset(float amount) const {
        return {x + amount, y + amount,
                std::max(0.0f, width - 2 * amount),
                std::max(0.0f, height - 2 * amount)};
    }

    Rect inset(float h, float v) const {
        return {x + h, y + v,
                std::max(0.0f, width - 2 * h),
                std::max(0.0f, height - 2 * v)};
    }

    bool operator==(const Rect& r) const {
        return x == r.x && y == r.y && width == r.width && height == r.height;
    }
};

// Layout mode
enum class LayoutMode { flex, grid };

// Flex layout direction
enum class FlexDirection { row, column };

// Flex alignment (auto_ = inherit from parent's align_items)
enum class FlexAlign { start, center, end, stretch, auto_ };

/// Justify content modes (main axis space distribution)
enum class FlexJustify {
    start,          ///< Pack items to start (default)
    center,         ///< Center items
    end_,           ///< Pack items to end
    space_between,  ///< Equal space between items, no space at edges
    space_around,   ///< Equal space around each item
    space_evenly,   ///< Equal space between items AND at edges
};

/// Overflow behavior
enum class FlexOverflow {
    visible,    ///< Content renders beyond bounds (default for views)
    hidden,     ///< Content clipped to bounds
    scroll,     ///< Content clipped, scrollbar shown when needed
    auto_,      ///< Like scroll but scrollbar only when content overflows
};

// ── Viewport-Relative Dimension Units ───────────────────────────────────────

enum class DimensionUnit {
    px,      ///< Absolute pixels
    percent, ///< Percentage of parent dimension
    vw,      ///< Percentage of viewport width
    vh,      ///< Percentage of viewport height
    vmin,    ///< Percentage of min(viewport width, height)
    vmax,    ///< Percentage of max(viewport width, height)
    auto_    ///< Auto-sized (let layout decide)
};

struct Dimension {
    float value = 0.0f;
    DimensionUnit unit = DimensionUnit::px;

    float resolve(float parent_size, float viewport_w, float viewport_h,
                  float dpi_scale = 1.0f) const {
        switch (unit) {
            case DimensionUnit::px:      return value * dpi_scale;
            case DimensionUnit::percent: return value / 100.0f * parent_size;
            case DimensionUnit::vw:      return value / 100.0f * viewport_w;
            case DimensionUnit::vh:      return value / 100.0f * viewport_h;
            case DimensionUnit::vmin:    return value / 100.0f * std::min(viewport_w, viewport_h);
            case DimensionUnit::vmax:    return value / 100.0f * std::max(viewport_w, viewport_h);
            case DimensionUnit::auto_:   return 0.0f;
        }
        return value;
    }

    static Dimension parse(const std::string& str) {
        if (str == "auto") return {0, DimensionUnit::auto_};
        Dimension d;
        size_t pos = 0;
        try { d.value = std::stof(str, &pos); } catch (...) { return d; }
        auto suffix = str.substr(pos);
        if (suffix == "vw") d.unit = DimensionUnit::vw;
        else if (suffix == "vh") d.unit = DimensionUnit::vh;
        else if (suffix == "vmin") d.unit = DimensionUnit::vmin;
        else if (suffix == "vmax") d.unit = DimensionUnit::vmax;
        else if (suffix == "%") d.unit = DimensionUnit::percent;
        else d.unit = DimensionUnit::px;
        return d;
    }
};

// Flex layout properties for a view
struct FlexStyle {
    FlexDirection direction = FlexDirection::column;
    FlexAlign align_items = FlexAlign::stretch;
    FlexAlign align_self = FlexAlign::auto_;  ///< Override parent's align_items for this child
    FlexJustify justify_content = FlexJustify::start;

    float gap = 0;              ///< Shorthand for both row_gap and column_gap
    float row_gap = -1;         ///< Gap between rows (-1 = use `gap`)
    float column_gap = -1;      ///< Gap between columns (-1 = use `gap`)

    float padding = 0;
    float padding_top = -1;     ///< Per-side padding (-1 = use uniform `padding`)
    float padding_right = -1;
    float padding_bottom = -1;
    float padding_left = -1;

    float margin = 0;           ///< Uniform margin around this view
    float margin_top = -1;      ///< Per-side margin (-1 = use uniform `margin`)
    float margin_right = -1;
    float margin_bottom = -1;
    float margin_left = -1;

    float flex_grow = 0;        ///< 0 = fixed size, >0 = share remaining space
    float flex_shrink = 1;      ///< 1 = shrink proportionally if overflow
    float flex_basis = -1;      ///< Initial main size before grow/shrink (-1 = use preferred)

    float min_width = 0;
    float min_height = 0;
    float preferred_width = 0;
    float preferred_height = 0;
    float max_width = 0;        ///< 0 = no maximum
    float max_height = 0;       ///< 0 = no maximum

    /// Viewport-relative dimension overrides. When set (unit != px with value 0),
    /// these are resolved before layout and override the corresponding float fields.
    Dimension dim_width;
    Dimension dim_height;
    Dimension dim_min_width;
    Dimension dim_min_height;

    /// Resolve viewport-relative dimensions and apply to float fields.
    /// Call before layout pass with the viewport size.
    void resolve_dimensions(float parent_w, float parent_h,
                            float viewport_w, float viewport_h, float dpi = 1.0f) {
        if (dim_width.unit != DimensionUnit::px || dim_width.value != 0)
            preferred_width = dim_width.resolve(parent_w, viewport_w, viewport_h, dpi);
        if (dim_height.unit != DimensionUnit::px || dim_height.value != 0)
            preferred_height = dim_height.resolve(parent_h, viewport_w, viewport_h, dpi);
        if (dim_min_width.unit != DimensionUnit::px || dim_min_width.value != 0)
            min_width = dim_min_width.resolve(parent_w, viewport_w, viewport_h, dpi);
        if (dim_min_height.unit != DimensionUnit::px || dim_min_height.value != 0)
            min_height = dim_min_height.resolve(parent_h, viewport_w, viewport_h, dpi);
    }

    bool flex_wrap = false;     ///< Wrap to next line when main axis overflows
    int order = 0;              ///< Layout order (lower values first, default 0)

    // Helper: resolve per-side margin
    float margin_t() const { return margin_top >= 0 ? margin_top : margin; }
    float margin_r() const { return margin_right >= 0 ? margin_right : margin; }
    float margin_b() const { return margin_bottom >= 0 ? margin_bottom : margin; }
    float margin_l() const { return margin_left >= 0 ? margin_left : margin; }

    // Helper: resolve directional gap
    float effective_gap(FlexDirection dir) const {
        if (dir == FlexDirection::row) return column_gap >= 0 ? column_gap : gap;
        return row_gap >= 0 ? row_gap : gap;
    }

    // Helper: resolve flex_basis or preferred size
    float basis_or_preferred(bool is_row) const {
        if (flex_basis >= 0) return flex_basis;
        return is_row ? preferred_width : preferred_height;
    }
};

/// Grid track size: fixed px, fractional (fr), or auto
struct GridTrack {
    enum class Type { fixed, fr, auto_ };
    Type type = Type::auto_;
    float value = 1.0f;  ///< px for fixed, fraction for fr, ignored for auto

    static GridTrack fixed_px(float px) { return {Type::fixed, px}; }
    static GridTrack fractional(float fr) { return {Type::fr, fr}; }
    static GridTrack auto_size() { return {Type::auto_, 0}; }
};

/// Grid layout properties (CSS Grid Level 1 subset)
struct GridStyle {
    std::vector<GridTrack> template_columns;  ///< grid-template-columns
    std::vector<GridTrack> template_rows;     ///< grid-template-rows
    float column_gap = 0;                     ///< grid-column-gap
    float row_gap = 0;                        ///< grid-row-gap

    // Per-child grid placement
    int grid_column_start = 0;  ///< 0 = auto placement
    int grid_column_end = 0;    ///< 0 = span 1
    int grid_row_start = 0;
    int grid_row_end = 0;

    /// Parse "1fr 2fr auto 100px" into track list
    static std::vector<GridTrack> parse_template(const std::string& tmpl);
};

} // namespace pulp::view

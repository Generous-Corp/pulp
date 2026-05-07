#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
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

// Flex layout direction.
// pulp #1434 (rn batch B) — added row_reverse / column_reverse so RN
// exports that emit `flexDirection: 'row-reverse' | 'column-reverse'`
// route to YGFlexDirectionRowReverse / ColumnReverse instead of falling
// through to YGFlexDirectionColumn.
enum class FlexDirection { row, column, row_reverse, column_reverse };

/// pulp #1434 Triage #14 — flex-wrap tri-state. Yoga has YGWrapNoWrap /
/// YGWrapWrap / YGWrapWrapReverse natively. Before this slice, the
/// FlexStyle field was a bool, so the wrap-reverse case was
/// inexpressible — `flex-wrap: wrap-reverse` silently fell through
/// to plain wrap.
enum class FlexWrap { no_wrap, wrap, wrap_reverse };

/// pulp #1516 — CSS `box-sizing`. Yoga 3.x has `YGNodeStyleSetBoxSizing`
/// which natively honors the spec: with `border-box`, the declared
/// width/height includes padding + border (the inner content area
/// shrinks); with `content-box` (CSS default), padding + border are
/// outside the declared dimensions. Web designs almost universally
/// reset `* { box-sizing: border-box }`, so this matters more than
/// the +1 catalog entry suggests — many imports only lay out
/// correctly under border-box.
enum class BoxSizing { content_box, border_box };

// Flex alignment (auto_ = inherit from parent's align_items).
// pulp #1434 (rn batch B) — added `baseline`. Yoga has YGAlignBaseline
// natively; before this batch, RN exports emitting
// `alignItems: 'baseline'` silently fell through to stretch.
enum class FlexAlign { start, center, end, stretch, auto_, baseline };

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

    /// pulp #1434 (sub-agent #12 follow-up) — align-content is the
    /// CSS / Yoga multi-line flex cross-axis distribution control.
    /// Yoga supports it natively via YGNodeStyleSetAlignContent; the
    /// only gap was a missing FlexStyle field + setter wiring. Default
    /// matches Yoga's default (FlexStart) — note this differs from
    /// CSS's `normal`/`stretch` defaults but matches Yoga / RN. The
    /// usual `space-between`/`space-around`/`space-evenly` /
    /// `flex-start`/`flex-end`/`center`/`stretch` values all map to
    /// the existing FlexAlign enum + the FlexJustify space-* values
    /// via `to_yg_align_content` in yoga_layout.cpp.
    FlexAlign align_content = FlexAlign::start;
    /// True when align_content was set to one of the space-* values
    /// (space-between / space-around / space-evenly). FlexAlign has
    /// no space variants because they are nonsensical for align_items
    /// / align_self; we encode them on a sibling enum here so the
    /// dispatcher can route to YGAlignSpaceBetween / SpaceAround /
    /// SpaceEvenly without overloading FlexAlign across two surfaces.
    enum class AlignContentSpace { none, space_between, space_around, space_evenly };
    AlignContentSpace align_content_space = AlignContentSpace::none;

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
    /// Yoga's native percent / auto APIs are dispatched on these in
    /// `yoga_layout.cpp::apply_flex_style` when `unit != px`.
    /// pulp #1434 (rn batch C) — added `dim_max_width` / `dim_max_height` /
    /// `dim_flex_basis` so percent and `auto` strings on max-* and flex-basis
    /// reach Yoga's `YGNodeStyleSet{MaxWidth,MaxHeight,FlexBasis}Percent` and
    /// `YGNodeStyleSetFlexBasisAuto` instead of being truncated to plain
    /// floats. min_* already had Dimension fields from earlier work.
    Dimension dim_width;
    Dimension dim_height;
    Dimension dim_min_width;
    Dimension dim_min_height;
    Dimension dim_max_width;
    Dimension dim_max_height;
    Dimension dim_flex_basis;

    /// pulp #1434 (cross-surface mega-batch) — per-edge margin / padding
    /// accept percent strings (and `auto` for margin only). Mirrors the
    /// width/height (#1426) and top/right/bottom/left (#1451) percent
    /// patterns. yoga_layout.cpp dispatches on `dim_*.unit` to
    /// `YGNodeStyleSetMargin{Percent,Auto}` /
    /// `YGNodeStyleSetPaddingPercent` for the non-px paths. Yoga's
    /// padding does not support `auto` (only margin does — see Yoga
    /// docs), so the bridge rejects `auto` on padding edges.
    Dimension dim_margin_top;
    Dimension dim_margin_right;
    Dimension dim_margin_bottom;
    Dimension dim_margin_left;
    Dimension dim_padding_top;
    Dimension dim_padding_right;
    Dimension dim_padding_bottom;
    Dimension dim_padding_left;

    /// pulp #1542 — yoga logical-edge fan-out. CSS / RN logical edges
    /// (`marginStart` / `marginEnd` / `paddingStart` / `paddingEnd` /
    /// `start` / `end`) flip with the writing direction: in LTR, `start`
    /// is the left edge; in RTL, `start` is the right edge. Yoga
    /// resolves this natively via `YGEdgeStart` / `YGEdgeEnd` once the
    /// node's writing direction is set (see `writing_direction` below).
    /// yoga_layout.cpp dispatches on `dim_*.unit`:
    ///   • px      → YGNodeStyleSetMargin/Padding/Position(YGEdgeStart|End)
    ///   • percent → YGNodeStyleSetMargin/Padding/PositionPercent(...)
    ///   • auto_   → YGNodeStyleSetMarginAuto(...) (margin only; Yoga
    ///                does not support auto on padding or position)
    /// These fields supplement, not replace, the per-side
    /// `dim_margin_left` / `dim_margin_right` etc. — yoga applies both
    /// and the *_start/end pair wins for the resolved start/end edge.
    Dimension dim_margin_start;
    Dimension dim_margin_end;
    Dimension dim_padding_start;
    Dimension dim_padding_end;
    Dimension dim_start;
    Dimension dim_end;

    /// pulp #1542 — node writing direction. Controls how Yoga resolves
    /// `YGEdgeStart` / `YGEdgeEnd` (and how it lays out row-axis
    /// children when no explicit start/end edge is set). Defaults to
    /// `inherit` so the layout root's direction propagates down. The
    /// bridge accepts the `direction_writing` sub-key on `setFlex`
    /// (avoids collision with the existing `direction` key for
    /// `flex-direction`) and the canonical CSS / RN values
    /// `'ltr'` / `'rtl'` / `'inherit'`.
    enum class WritingDirection { inherit, ltr, rtl };
    WritingDirection writing_direction = WritingDirection::inherit;

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

    /// pulp #1434 Triage #14 — tri-state wrap. CSS / RN allow
    /// `wrap-reverse` (overflows wrap UP instead of DOWN, or RIGHT
    /// instead of LEFT depending on flex-direction). Yoga has
    /// YGWrapWrapReverse for this; previously flex_wrap was a bool
    /// expressible only as wrap / no-wrap. Bridge accepts numeric (0/1
    /// for backward compat) and the CSS keyword strings ("wrap" /
    /// "wrap-reverse" / "nowrap" / "no-wrap").
    FlexWrap flex_wrap = FlexWrap::no_wrap;
    /// pulp #1516 — CSS `box-sizing`. Default is `border_box` to match
    /// Yoga 3.x's own default AND pulp's prior implicit behavior (no
    /// existing fixture / consumer was prepared for content-box layout
    /// math, even though that's the CSS spec default). Web designs
    /// almost universally reset to `border-box` via
    /// `* { box-sizing: border-box }`, so this matches what consumers
    /// pasting JSX from Figma / v0 / Claude Design HTML expect.
    /// Setting `content-box` opts in to CSS-spec behavior:
    /// `width = 100; padding = 10` → outer width = 120 instead of 100.
    BoxSizing box_sizing = BoxSizing::border_box;
    int order = 0;              ///< Layout order (lower values first, default 0)

    /// Aspect ratio (width / height). When set, Yoga sizes the cross axis
    /// to match `main_axis / aspect_ratio` (or the inverse, depending on
    /// which dimension is constrained). pulp #1434 — RN exports use
    /// `aspectRatio` for image cards / video tiles; Figma exports for
    /// fixed-ratio frames; v0/Claude Design generate it for hero images.
    /// std::optional distinguishes "unset" from a literal 0 value (which
    /// would be invalid anyway, but the optional makes the intent explicit
    /// at the dispatcher boundary). Accepts plain numbers (1.5),
    /// `width/height` parsed by web-compat-style-decl.js (16/9 -> 1.778),
    /// and `auto` which clears the slot.
    std::optional<float> aspect_ratio;

    // Helper: resolve per-side margin
    float margin_t() const { return margin_top >= 0 ? margin_top : margin; }
    float margin_r() const { return margin_right >= 0 ? margin_right : margin; }
    float margin_b() const { return margin_bottom >= 0 ? margin_bottom : margin; }
    float margin_l() const { return margin_left >= 0 ? margin_left : margin; }

    // Helper: resolve directional gap.
    // pulp #1434 (rn batch B) — row_reverse counts as a row-axis
    // container; the visual reversal doesn't change which gap edge
    // (column-gap) sits between siblings.
    float effective_gap(FlexDirection dir) const {
        if (dir == FlexDirection::row || dir == FlexDirection::row_reverse)
            return column_gap >= 0 ? column_gap : gap;
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

/// Grid layout properties (CSS Grid Level 1 subset).
///
/// pulp #1434 Phase A2-2 extends the existing infrastructure with:
///   • grid-auto-columns / grid-auto-rows  — implicit-track sizing
///   • grid-auto-flow                       — row / column / dense
///   • grid-template-areas                  — named-area string parser
///   • grid-area shorthand                  — single token reference
///     into the named-area map (resolves to the matching cell range)
struct GridStyle {
    std::vector<GridTrack> template_columns;  ///< grid-template-columns
    std::vector<GridTrack> template_rows;     ///< grid-template-rows

    /// pulp #1434 Phase A2-2 — implicit-track sizing for auto-placed
    /// items that overflow the explicit grid. CSS spec: a single
    /// track template that's repeated for every implicit row/column.
    GridTrack auto_columns = GridTrack::auto_size();
    GridTrack auto_rows    = GridTrack::auto_size();

    /// pulp #1434 Phase A2-2 — auto-flow direction.
    enum class AutoFlow {
        row,           ///< default; fill rows left-to-right then wrap
        column,        ///< fill columns top-to-bottom then wrap
        row_dense,     ///< row + dense-packing (fill earlier holes)
        column_dense,  ///< column + dense-packing
    };
    AutoFlow auto_flow = AutoFlow::row;

    float column_gap = 0;                     ///< grid-column-gap
    float row_gap = 0;                        ///< grid-row-gap

    /// pulp #1434 Phase A2-2 — named-area grid. Each entry is
    /// `{name, col_start, col_end, row_start, row_end}` (1-based,
    /// matching CSS line-numbering). Populated by
    /// `parse_template_areas("'h h h' 'm c c' 'f f f'")`. The
    /// per-child `grid_area` field below references one of these
    /// names to resolve placement.
    struct NamedArea {
        std::string name;
        int col_start = 1;
        int col_end = 1;
        int row_start = 1;
        int row_end = 1;
    };
    std::vector<NamedArea> template_areas;

    // Per-child grid placement
    int grid_column_start = 0;  ///< 0 = auto placement
    int grid_column_end = 0;    ///< 0 = span 1
    int grid_row_start = 0;
    int grid_row_end = 0;
    /// pulp #1434 Phase A2-2 — `grid-area: header` references the
    /// parent's NamedArea by name. Empty string means "no named-area
    /// reference; use the explicit start/end fields above."
    std::string grid_area_name;

    /// Parse "1fr 2fr auto 100px" into track list
    static std::vector<GridTrack> parse_template(const std::string& tmpl);

    /// pulp #1434 Phase A2-2 — parse the CSS named-area grid string
    /// (e.g. `"'h h h' 'm c c' 'f f f'"`) into the NamedArea list.
    /// Each row is wrapped in single quotes; cells within a row are
    /// space-separated. Cells that share a name across adjacent rows
    /// or columns merge into a single rectangular area. `'.'` is the
    /// CSS spec spacer token (skipped entirely).
    static std::vector<NamedArea> parse_template_areas(const std::string& css);

    /// Parse the auto-flow keyword string. Unrecognized → row.
    static AutoFlow parse_auto_flow(const std::string& s) {
        if (s == "column")             return AutoFlow::column;
        if (s == "dense" || s == "row dense") return AutoFlow::row_dense;
        if (s == "column dense")       return AutoFlow::column_dense;
        return AutoFlow::row;
    }
};

} // namespace pulp::view

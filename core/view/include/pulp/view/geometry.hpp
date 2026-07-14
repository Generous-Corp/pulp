#pragma once

#include <pulp/canvas/affine_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

/// The 2D affine transform value type. Defined in `pulp::canvas` — which is
/// the layer BELOW this one (core/view links core/canvas, not the reverse) and
/// where `Path` needs it — and re-exported here so view code can spell it
/// `pulp::view::AffineTransform`. One type, two names; never two types.
using AffineTransform = pulp::canvas::AffineTransform;

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

struct IntRect;  // below

/// Axis-aligned rectangle in continuous (float) coordinates.
///
/// ── WHY THERE IS ALSO AN `IntRect` ──────────────────────────────────────
/// A float rect's centre is `x + width / 2.0f`. An INTEGER rect's centre is
/// `x + width / 2` — integer division, which TRUNCATES. For an even extent the
/// two agree; for an odd one they differ by half a pixel. That half pixel is
/// not academic: it is the difference between a 7px row's content landing on
/// the pixel grid and landing between two pixels, where it renders as a blurry
/// double-width line. Code that computes an integer layout with float maths and
/// rounds at the end does NOT reproduce integer-rect behaviour, because the
/// truncation has to happen at each step, not once at the end.
///
/// So the two types deliberately do NOT share an implementation. Pick the one
/// that matches the coordinate space you are actually in, and convert
/// explicitly with `to_int()` / `to_float()`.
struct Rect {
    float x = 0, y = 0, width = 0, height = 0;

    constexpr float right() const { return x + width; }
    constexpr float bottom() const { return y + height; }
    Point origin() const { return {x, y}; }
    Size size() const { return {width, height}; }
    Point center() const { return {x + width * 0.5f, y + height * 0.5f}; }

    /// True centre in continuous coordinates — exact halves, no truncation.
    constexpr float centre_x() const { return x + width * 0.5f; }
    constexpr float centre_y() const { return y + height * 0.5f; }

    bool contains(Point p) const {
        return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
    }
    constexpr bool contains(float px, float py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }
    /// Whole-rect containment: does this rect fully enclose `r`?
    ///
    /// NOT an overload of `contains`, on purpose. `Rect` has default member
    /// initializers, so a braced `{50, 30}` is a valid two-field `Rect` as well
    /// as a valid `Point` — which makes the natural, idiomatic
    /// `r.contains({50, 30})` ambiguous at EVERY call site the moment a
    /// `contains(const Rect&)` overload exists. A distinct name is the fix;
    /// `encloses` also says what it does more plainly than a third `contains`.
    constexpr bool encloses(const Rect& r) const {
        return r.x >= x && r.y >= y && r.right() <= right() && r.bottom() <= bottom();
    }

    constexpr bool is_empty() const { return width <= 0 || height <= 0; }

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

    // ── Non-mutating derivations ─────────────────────────────────────────
    /// Shrink by `d` on every side (the common "padding" idiom).
    Rect reduced(float d) const { return inset(d); }
    Rect reduced(float dx, float dy) const { return inset(dx, dy); }
    /// Grow by `d` on every side.
    Rect expanded(float d) const { return inset(-d); }
    Rect expanded(float dx, float dy) const { return inset(-dx, -dy); }

    constexpr Rect translated(float dx, float dy) const {
        return {x + dx, y + dy, width, height};
    }

    constexpr Rect with_x(float v) const      { return {v, y, width, height}; }
    constexpr Rect with_y(float v) const      { return {x, v, width, height}; }
    constexpr Rect with_width(float v) const  { return {x, y, v, height}; }
    constexpr Rect with_height(float v) const { return {x, y, width, v}; }
    constexpr Rect with_size(float w, float h) const { return {x, y, w, h}; }
    constexpr Rect with_position(float nx, float ny) const {
        return {nx, ny, width, height};
    }

    /// Resize about the centre — the centre stays put and the edges move.
    constexpr Rect with_size_keeping_centre(float w, float h) const {
        return {x + (width - w) * 0.5f, y + (height - h) * 0.5f, w, h};
    }

    // ── The slicing idiom ────────────────────────────────────────────────
    // `remove_from_*` MUTATES this rect and returns the slice taken off. It is
    // the standard way to lay a row of controls out of a bounds rect:
    //
    //     auto header = bounds.remove_from_top(24);
    //     auto footer = bounds.remove_from_bottom(24);
    //     // `bounds` is now just the middle.
    //
    // Asking for more than exists takes everything and leaves an empty rect.
    // Asking for a NEGATIVE amount takes nothing and changes nothing — a
    // deliberate choice: the alternative (a negative-height slice, which is
    // what naive arithmetic produces) is never what a caller wants and turns a
    // simple sign bug into an invisible, badly-clipped widget.
    Rect remove_from_top(float amount) {
        if (amount <= 0) return {x, y, width, 0};
        const float take = std::min(amount, height);
        const Rect taken{x, y, width, take};
        y += take;
        height -= take;
        return taken;
    }
    Rect remove_from_bottom(float amount) {
        if (amount <= 0) return {x, bottom(), width, 0};
        const float take = std::min(amount, height);
        height -= take;
        return {x, y + height, width, take};
    }
    Rect remove_from_left(float amount) {
        if (amount <= 0) return {x, y, 0, height};
        const float take = std::min(amount, width);
        const Rect taken{x, y, take, height};
        x += take;
        width -= take;
        return taken;
    }
    Rect remove_from_right(float amount) {
        if (amount <= 0) return {right(), y, 0, height};
        const float take = std::min(amount, width);
        width -= take;
        return {x + width, y, take, height};
    }

    // ── Set operations ───────────────────────────────────────────────────
    constexpr bool intersects(const Rect& o) const {
        if (is_empty() || o.is_empty()) return false;
        return x < o.right() && right() > o.x && y < o.bottom() && bottom() > o.y;
    }

    /// The overlap, or an empty rect when they do not overlap.
    Rect intersection(const Rect& o) const {
        const float ix = std::max(x, o.x);
        const float iy = std::max(y, o.y);
        const float ir = std::min(right(), o.right());
        const float ib = std::min(bottom(), o.bottom());
        if (ir <= ix || ib <= iy) return {};
        return {ix, iy, ir - ix, ib - iy};
    }

    /// Smallest rect enclosing both. An empty operand contributes nothing —
    /// without that rule, a default-constructed accumulator at (0,0) would drag
    /// every union back to the origin.
    Rect union_with(const Rect& o) const {
        if (is_empty()) return o;
        if (o.is_empty()) return *this;
        const float ux = std::min(x, o.x);
        const float uy = std::min(y, o.y);
        const float ur = std::max(right(), o.right());
        const float ub = std::max(bottom(), o.bottom());
        return {ux, uy, ur - ux, ub - uy};
    }

    // ── Conversion ───────────────────────────────────────────────────────
    /// Round each edge to the nearest integer. Note this rounds the EDGES, so
    /// the resulting width can differ from `round(width)` by one — which is the
    /// correct behaviour for a rect being snapped to the pixel grid.
    IntRect to_nearest_int() const;

    /// Smallest integer rect that fully CONTAINS this one.
    IntRect to_enclosing_int() const;

    bool operator==(const Rect& r) const {
        return x == r.x && y == r.y && width == r.width && height == r.height;
    }
    bool operator!=(const Rect& r) const { return !(*this == r); }
};

/// Axis-aligned rectangle in DISCRETE (integer) coordinates.
///
/// Every operation here uses integer arithmetic — most importantly, the centre
/// is an integer division and therefore truncates. See the note on `Rect`.
struct IntRect {
    int x = 0, y = 0, width = 0, height = 0;

    constexpr int right() const { return x + width; }
    constexpr int bottom() const { return y + height; }

    /// Integer centre. `IntRect{0,0,7,7}.centre_x() == 3`, NOT 3.5 — the half
    /// pixel is truncated away, and that is the whole point of this type.
    constexpr int centre_x() const { return x + width / 2; }
    constexpr int centre_y() const { return y + height / 2; }

    constexpr bool is_empty() const { return width <= 0 || height <= 0; }

    constexpr bool contains(int px, int py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }
    constexpr bool contains(const IntRect& r) const {
        return r.x >= x && r.y >= y && r.right() <= right() && r.bottom() <= bottom();
    }

    IntRect reduced(int d) const {
        return {x + d, y + d, std::max(0, width - 2 * d), std::max(0, height - 2 * d)};
    }
    IntRect reduced(int dx, int dy) const {
        return {x + dx, y + dy, std::max(0, width - 2 * dx), std::max(0, height - 2 * dy)};
    }
    IntRect expanded(int d) const { return reduced(-d); }
    IntRect expanded(int dx, int dy) const { return reduced(-dx, -dy); }

    constexpr IntRect translated(int dx, int dy) const {
        return {x + dx, y + dy, width, height};
    }

    constexpr IntRect with_x(int v) const      { return {v, y, width, height}; }
    constexpr IntRect with_y(int v) const      { return {x, v, width, height}; }
    constexpr IntRect with_width(int v) const  { return {x, y, v, height}; }
    constexpr IntRect with_height(int v) const { return {x, y, width, v}; }
    constexpr IntRect with_size(int w, int h) const { return {x, y, w, h}; }
    constexpr IntRect with_position(int nx, int ny) const {
        return {nx, ny, width, height};
    }

    /// Resize about the centre, in integer arithmetic. `(width - w) / 2`
    /// truncates: IntRect{0,0,7,7}.with_size_keeping_centre(4,4) is at x=1
    /// (because 3/2 == 1), where the float rect would be at x=1.5.
    constexpr IntRect with_size_keeping_centre(int w, int h) const {
        return {x + (width - w) / 2, y + (height - h) / 2, w, h};
    }

    // ── The slicing idiom (integer) ──────────────────────────────────────
    // Same contract as the float version, including the negative-amount guard.
    IntRect remove_from_top(int amount) {
        if (amount <= 0) return {x, y, width, 0};
        const int take = std::min(amount, height);
        const IntRect taken{x, y, width, take};
        y += take;
        height -= take;
        return taken;
    }
    IntRect remove_from_bottom(int amount) {
        if (amount <= 0) return {x, bottom(), width, 0};
        const int take = std::min(amount, height);
        height -= take;
        return {x, y + height, width, take};
    }
    IntRect remove_from_left(int amount) {
        if (amount <= 0) return {x, y, 0, height};
        const int take = std::min(amount, width);
        const IntRect taken{x, y, take, height};
        x += take;
        width -= take;
        return taken;
    }
    IntRect remove_from_right(int amount) {
        if (amount <= 0) return {right(), y, 0, height};
        const int take = std::min(amount, width);
        width -= take;
        return {x + width, y, take, height};
    }

    constexpr bool intersects(const IntRect& o) const {
        if (is_empty() || o.is_empty()) return false;
        return x < o.right() && right() > o.x && y < o.bottom() && bottom() > o.y;
    }

    IntRect intersection(const IntRect& o) const {
        const int ix = std::max(x, o.x);
        const int iy = std::max(y, o.y);
        const int ir = std::min(right(), o.right());
        const int ib = std::min(bottom(), o.bottom());
        if (ir <= ix || ib <= iy) return {};
        return {ix, iy, ir - ix, ib - iy};
    }

    IntRect union_with(const IntRect& o) const {
        if (is_empty()) return o;
        if (o.is_empty()) return *this;
        const int ux = std::min(x, o.x);
        const int uy = std::min(y, o.y);
        const int ur = std::max(right(), o.right());
        const int ub = std::max(bottom(), o.bottom());
        return {ux, uy, ur - ux, ub - uy};
    }

    /// Widening conversion — always exact.
    constexpr Rect to_float() const {
        return {static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(width), static_cast<float>(height)};
    }

    constexpr bool operator==(const IntRect& r) const {
        return x == r.x && y == r.y && width == r.width && height == r.height;
    }
    constexpr bool operator!=(const IntRect& r) const { return !(*this == r); }
};

inline IntRect Rect::to_nearest_int() const {
    // Round the EDGES, then derive the extent — so a rect spanning [0.6, 2.4]
    // becomes [1, 2] (width 1), not "width round(1.8) = 2" starting at 1.
    const int l = static_cast<int>(std::lround(x));
    const int t = static_cast<int>(std::lround(y));
    const int r = static_cast<int>(std::lround(right()));
    const int b = static_cast<int>(std::lround(bottom()));
    return {l, t, r - l, b - t};
}

inline IntRect Rect::to_enclosing_int() const {
    const int l = static_cast<int>(std::floor(x));
    const int t = static_cast<int>(std::floor(y));
    const int r = static_cast<int>(std::ceil(right()));
    const int b = static_cast<int>(std::ceil(bottom()));
    return {l, t, r - l, b - t};
}

// Layout mode
enum class LayoutMode { flex, grid };

// Flex layout direction. row_reverse / column_reverse route
// `flexDirection: 'row-reverse' | 'column-reverse'` to
// YGFlexDirectionRowReverse / ColumnReverse.
enum class FlexDirection { row, column, row_reverse, column_reverse };

/// Flex-wrap tri-state. Yoga has YGWrapNoWrap / YGWrapWrap /
/// YGWrapWrapReverse natively, so `flex-wrap: wrap-reverse` is distinct
/// from plain wrap.
enum class FlexWrap { no_wrap, wrap, wrap_reverse };

/// CSS `box-sizing`. Yoga 3.x has `YGNodeStyleSetBoxSizing` which natively
/// honors the spec: with `border-box`, the declared width/height includes
/// padding + border (the inner content area shrinks); with `content-box`
/// (CSS default), padding + border are outside the declared dimensions.
/// Many imports only lay out correctly under border-box because web designs
/// commonly reset `* { box-sizing: border-box }`.
enum class BoxSizing { content_box, border_box };

// Flex alignment (auto_ = inherit from parent's align_items).
// `baseline` maps to YGAlignBaseline.
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
        auto trim = [](std::string s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        };

        const auto input = trim(str);
        if (input == "auto") return {0, DimensionUnit::auto_};
        Dimension d;
        size_t pos = 0;
        try { d.value = std::stof(input, &pos); } catch (...) { return d; }
        auto suffix = trim(input.substr(pos));
        if (suffix == "vw") d.unit = DimensionUnit::vw;
        else if (suffix == "vh") d.unit = DimensionUnit::vh;
        else if (suffix == "vmin") d.unit = DimensionUnit::vmin;
        else if (suffix == "vmax") d.unit = DimensionUnit::vmax;
        else if (suffix == "%") d.unit = DimensionUnit::percent;
        else if (suffix.empty() || suffix == "px") d.unit = DimensionUnit::px;
        else return {};
        return d;
    }
};

// Flex layout properties for a view
struct FlexStyle {
    FlexDirection direction = FlexDirection::column;
    FlexAlign align_items = FlexAlign::stretch;
    FlexAlign align_self = FlexAlign::auto_;  ///< Override parent's align_items for this child

    /// align-content is the CSS / Yoga multi-line flex cross-axis
    /// distribution control. Yoga supports it natively via
    /// YGNodeStyleSetAlignContent. Default matches Yoga's default
    /// (FlexStart) — note this differs from CSS's `normal`/`stretch`
    /// defaults but matches Yoga / RN. The usual
    /// `space-between`/`space-around`/`space-evenly` /
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

    /// Viewport-relative dimension overrides. Non-default units or non-zero
    /// values are resolved before layout and override the corresponding float
    /// fields.
    /// Yoga's native percent / auto APIs are dispatched on these in
    /// `yoga_layout.cpp::apply_flex_style` when `unit != px`.
    /// `resolve_dimensions()` resolves preferred/min dimensions for the local
    /// scalar fields; max-* and flex-basis stay as Dimension values so they can
    /// reach Yoga's `YGNodeStyleSet{MaxWidth,MaxHeight,FlexBasis}Percent` and
    /// `YGNodeStyleSetFlexBasisAuto` instead of being truncated to plain floats.
    Dimension dim_width;
    Dimension dim_height;
    Dimension dim_min_width;
    Dimension dim_min_height;
    Dimension dim_max_width;
    Dimension dim_max_height;
    Dimension dim_flex_basis;

    /// Per-edge margin / padding accept percent strings (and `auto` for
    /// margin only). yoga_layout.cpp dispatches on `dim_*.unit` to
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

    /// Yoga logical-edge fan-out. CSS / RN logical edges (`marginStart` /
    /// `marginEnd` / `paddingStart` / `paddingEnd` / `start` / `end`) flip
    /// with the writing direction: in LTR, `start` is the left edge; in RTL,
    /// `start` is the right edge. Yoga resolves this natively via
    /// `YGEdgeStart` / `YGEdgeEnd` once the node's writing direction is set
    /// (see `writing_direction` below).
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

    /// Node writing direction. Controls how Yoga resolves `YGEdgeStart` /
    /// `YGEdgeEnd` (and how it lays out row-axis
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

    /// Tri-state wrap. CSS / RN allow `wrap-reverse` (overflows wrap UP
    /// instead of DOWN, or RIGHT instead of LEFT depending on flex-direction).
    /// Yoga has YGWrapWrapReverse for this. Bridge accepts numeric (0/1 for
    /// backward compat) and the CSS keyword strings ("wrap" / "wrap-reverse" /
    /// "nowrap" / "no-wrap").
    FlexWrap flex_wrap = FlexWrap::no_wrap;
    /// CSS `box-sizing`. Default is `border_box` to match Yoga 3.x's own
    /// default and the web-import convention that most pasted JSX already
    /// assumes via `* { box-sizing: border-box }`.
    /// Setting `content-box` opts in to CSS-spec behavior:
    /// `width = 100; padding = 10` → outer width = 120 instead of 100.
    BoxSizing box_sizing = BoxSizing::border_box;
    int order = 0;              ///< Layout order (lower values first, default 0)

    /// Aspect ratio (width / height). When set, Yoga sizes the cross axis
    /// to match `main_axis / aspect_ratio` (or the inverse, depending on
    /// which dimension is constrained). Imports use `aspectRatio` for image
    /// cards, video tiles, fixed-ratio frames, and hero images.
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

    // Helper: resolve directional gap. row_reverse counts as a row-axis
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
/// Supports:
///   • grid-auto-columns / grid-auto-rows  — implicit-track sizing
///   • grid-auto-flow                       — row / column / dense
///   • grid-template-areas                  — named-area string parser
///   • grid-area shorthand                  — single token reference
///     into the named-area map (resolves to the matching cell range)
struct GridStyle {
    std::vector<GridTrack> template_columns;  ///< grid-template-columns
    std::vector<GridTrack> template_rows;     ///< grid-template-rows

    /// Implicit-track sizing for auto-placed items that overflow the explicit
    /// grid. CSS spec: a single track template that's repeated for every
    /// implicit row/column.
    GridTrack auto_columns = GridTrack::auto_size();
    GridTrack auto_rows    = GridTrack::auto_size();

    /// Auto-flow direction.
    enum class AutoFlow {
        row,           ///< default; fill rows left-to-right then wrap
        column,        ///< fill columns top-to-bottom then wrap
        row_dense,     ///< row + dense-packing (fill earlier holes)
        column_dense,  ///< column + dense-packing
    };
    AutoFlow auto_flow = AutoFlow::row;

    float column_gap = 0;                     ///< grid-column-gap
    float row_gap = 0;                        ///< grid-row-gap

    /// Named-area grid. Each entry is `{name, col_start, col_end,
    /// row_start, row_end}` (1-based,
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
    /// `grid-area: header` references the parent's NamedArea by name.
    /// Empty string means "no named-area reference; use the explicit
    /// start/end fields above."
    std::string grid_area_name;

    /// Parse "1fr 2fr auto 100px" into track list. `depth` bounds recursion
    /// into nested `repeat(...)` bodies from untrusted design-tool input.
    static std::vector<GridTrack> parse_template(const std::string& tmpl, int depth = 0);

    /// Parse the CSS named-area grid string (e.g.
    /// `"'h h h' 'm c c' 'f f f'"`) into the NamedArea list.
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

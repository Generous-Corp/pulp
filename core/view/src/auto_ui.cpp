#include <pulp/view/auto_ui.hpp>
#include <pulp/view/ui_components.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <vector>

namespace pulp::view {

namespace {

constexpr float kTileWidth = 82.0f;
constexpr float kTileHeight = 112.0f;
constexpr float kTileGap = 14.0f;
constexpr float kMaxGridWidth = 760.0f;

// Shared chrome metrics — kept here so build()'s layout, the scroll body's
// content-extent math, and preferred_size()'s fit math never drift apart.
constexpr float kRootPadding = 14.0f;   ///< root column padding on every edge
constexpr float kTitleHeight = 24.0f;   ///< "Parameters" title row
constexpr float kRootGap = 12.0f;       ///< gap between title and body
constexpr float kBodyContentInset = 10.0f;  ///< body viewport → grid width slack
// Grouped-layout metrics (mirror update_content_extent()).
constexpr float kGroupOuterPadding = 6.0f;
constexpr float kGroupGap = 10.0f;
constexpr float kGroupSidePadding = 8.0f;
constexpr float kGroupBottomPadding = 8.0f;
constexpr float kGroupHeaderExtra = 6.0f;  ///< gap below the header before the grid

// Widest column count AutoUi will wrap to before it starts a new row, derived
// from the same tile+gap math the grid uses so the fit stays under kMaxGridWidth.
constexpr std::size_t kMaxColumns = static_cast<std::size_t>(
    (kMaxGridWidth + kTileGap) / (kTileWidth + kTileGap));

// Sensible floor so a one- or two-knob plugin doesn't open as a tiny sliver.
constexpr std::uint32_t kMinPreferredWidth = 320;
constexpr std::uint32_t kMinPreferredHeight = 240;

/// Columns AutoUi wraps @p count tiles into: roughly square, clamped to a
/// single row's worth and to kMaxColumns. 7 params → 3 columns (a 3x3-ish
/// block), 4 → 2, 16 → 4.
std::size_t columns_for(std::size_t count) {
    if (count <= 1) return count;  // 0 → 0, 1 → 1
    const auto square = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(count))));
    return std::clamp<std::size_t>(square, 1, kMaxColumns);
}

/// Pixel size of a grid holding @p count tiles at @p columns wide.
struct GridExtent {
    float width = 0.0f;
    float height = 0.0f;
};
GridExtent grid_extent(std::size_t count, std::size_t columns) {
    if (count == 0 || columns == 0) return {};
    const auto rows = (count + columns - 1) / columns;
    return {
        static_cast<float>(columns) * kTileWidth +
            static_cast<float>(columns - 1) * kTileGap,
        static_cast<float>(rows) * kTileHeight +
            static_cast<float>(rows - 1) * kTileGap,
    };
}

std::string format_parameter_value(const state::ParamInfo& param, float norm) {
    float val = param.range.denormalize(norm);
    // Normalization round-off can turn an exact plain zero into a tiny negative
    // value. Do not expose that implementation detail as "-0.00" in the UI.
    if (std::abs(val) < 0.0005f) val = 0.0f;
    std::ostringstream ss;
    if (std::abs(val) >= 100) ss << std::fixed << std::setprecision(0);
    else if (std::abs(val) >= 10) ss << std::fixed << std::setprecision(1);
    else                         ss << std::fixed << std::setprecision(2);
    ss << val;
    if (!param.unit.empty()) ss << " " << param.unit;
    return ss.str();
}

std::string value_label_id(state::ParamID id) {
    return "__auto_ui_value_" + std::to_string(id);
}

class AutoUiBody final : public ScrollView {
public:
    struct GroupLayout {
        GroupBox* box = nullptr;
        View* grid = nullptr;
        std::size_t parameter_count = 0;
    };

    void set_ungrouped_grid(View* grid, std::size_t parameter_count) {
        grid_ = grid;
        parameter_count_ = parameter_count;
    }

    void set_grouped_content(View* content, std::vector<GroupLayout> groups) {
        grouped_content_ = content;
        groups_ = std::move(groups);
    }

    void on_resized() override { update_content_extent(); }

    void layout_children() override {
        update_content_extent();
        ScrollView::layout_children();
    }

private:
    void update_content_extent() {
        const auto viewport = local_bounds();
        const float viewport_width = std::max(1.0f, viewport.width);
        const float content_width = std::min(
            kMaxGridWidth, std::max(kTileWidth, viewport_width - kBodyContentInset));

        if (grouped_content_) {
            float content_height = 2.0f * kGroupOuterPadding;
            for (std::size_t i = 0; i < groups_.size(); ++i) {
                auto& group = groups_[i];
                const float grid_width = std::max(
                    kTileWidth, content_width - 2.0f * kGroupSidePadding);
                const float grid_height = wrapped_height(group.parameter_count,
                                                         grid_width);
                group.grid->flex().preferred_width = grid_width;
                group.grid->flex().preferred_height = grid_height;
                group.box->flex().preferred_width = content_width;
                group.box->flex().preferred_height =
                    GroupBox::header_height + kGroupHeaderExtra + grid_height +
                    kGroupBottomPadding;
                content_height += group.box->flex().preferred_height;
                if (i + 1 < groups_.size()) content_height += kGroupGap;
            }

            grouped_content_->flex().preferred_width = viewport_width;
            grouped_content_->flex().preferred_height = content_height;
            set_content_size({viewport_width,
                              std::max(viewport.height, content_height)});
        } else if (grid_) {
            const float grid_height = wrapped_height(parameter_count_, content_width);
            grid_->flex().preferred_width = content_width;
            grid_->flex().preferred_height = grid_height;
            // The single wrapped grid IS the scroll content. Yoga lays it out at
            // the viewport height (not its taller content height), so its
            // wrapped rows fill a box shorter than they need. `align_content:
            // center` then centers those rows, pushing the first row ABOVE the
            // box top — and scroll_y is clamped to [0, content-viewport], so the
            // top row becomes unreachable (the clip-and-can't-scroll-up bug).
            // Top-align the rows once they overflow so the first row sits at the
            // scroll origin; keep centering the small clusters that fit.
            grid_->flex().align_content = grid_height > viewport.height
                                              ? FlexAlign::start
                                              : FlexAlign::center;
            set_content_size({viewport_width, std::max(viewport.height, grid_height)});
        }
    }

    static float wrapped_height(std::size_t count, float width) {
        if (count == 0) return 0.0f;
        const auto columns = std::max<std::size_t>(
            1, static_cast<std::size_t>(
                   std::floor((width + kTileGap) / (kTileWidth + kTileGap))));
        const auto rows = (count + columns - 1) / columns;
        return static_cast<float>(rows) * kTileHeight +
               static_cast<float>(rows - 1) * kTileGap;
    }

    View* grid_ = nullptr;
    std::size_t parameter_count_ = 0;
    View* grouped_content_ = nullptr;
    std::vector<GroupLayout> groups_;
};

/// AutoUi's scroll body derives its wrapped content extent from the viewport
/// width assigned by the parent Yoga pass. That information does not exist
/// until the first pass has applied the body's bounds, so the initial layout
/// needs one bounded convergence pass. Remembering the settled root size keeps
/// ordinary parameter/value repaints at one pass while still reconverging after
/// a real host resize.
class AutoUiRoot final : public View {
public:
    void layout_children() override {
        const auto size = local_bounds();
        const bool needs_convergence =
            !has_settled_layout_ || size.width != settled_width_ ||
            size.height != settled_height_;

        View::layout_children();
        if (needs_convergence) View::layout_children();

        has_settled_layout_ = true;
        settled_width_ = size.width;
        settled_height_ = size.height;
    }

private:
    bool has_settled_layout_ = false;
    float settled_width_ = 0.0f;
    float settled_height_ = 0.0f;
};

} // namespace

// AutoUi is the default editor for Processors that don't supply a
// custom create_view() (and aren't using a scripted UI). That's the
// majority of in-tree examples and the natural starting point for
// `pulp create my-plugin`. Previously, the layout was a single
// `flex-start`-aligned non-wrapping row, so a 4-knob plugin in the
// stock 400x300 editor window rendered the cluster in the top-left
// with ~70% of the window empty — looked like a layout bug.
//
// Layout:
//
//   root: column, padding 14, gap 12
//   ├── title: "Parameters" 16pt
//   └── body: vertical ScrollView, flex_grow=1
//        └── grid: row, flex_wrap=wrap, justify=center, align_items=center,
//                  align_content=center, max_width=760
//             └── tile per param (column, fixed 82x112, no shrink)
//                  ├── Knob 64x64 + parameter/value rows below it
//                  └── OR Toggle 54x30 (with its own label)
//
// Properties of the new shape:
//   - 1 param → centered single tile
//   - 4 params → centered cluster, 4 across in a stock 400x300 window
//   - 16 params → wraps to multiple rows, still centered
//   - 32+ params → wraps and scrolls vertically instead of truncating
//   - named StateStore groups → titled, wrapping sections in store order
//
// Size: preferred_size() computes a design size that fits the generated
// grid, and the format adapters adopt it (via view_size_from_design()) when
// the processor declares no explicit size — so the default editor OPENS large
// enough to show every knob and then scales proportionally on resize. A
// processor that overrides view_size()/editor_size() or sets DESIGN_WIDTH/
// HEIGHT still wins; AutoUi only fills the otherwise-unset default.

std::unique_ptr<View> AutoUi::build(state::StateStore& store) {
    auto root = std::make_unique<AutoUiRoot>();
    root->flex().direction = FlexDirection::column;
    root->flex().padding = 14;
    root->flex().gap = 12;

    // Title — kept as a stable anchor; small enough to leave room for
    // params, large enough to not feel buried.
    auto title = std::make_unique<Label>("Parameters");
    title->set_font_size(16.0f);
    title->flex().preferred_height = 24;
    root->add_child(std::move(title));

    // Body fills the remaining space. It keeps small sets centered, but owns
    // the content extent for large/grouped sets so the host can wheel-scroll
    // to every parameter instead of silently clipping the last rows.
    auto body = std::make_unique<AutoUiBody>();
    auto* body_raw = body.get();
    body->flex().direction = FlexDirection::column;
    body->flex().flex_grow = 1;
    body->flex().justify_content = FlexJustify::center;
    body->flex().align_items = FlexAlign::center;

    auto make_tile = [&](const state::ParamInfo& param) {
        auto tile = std::make_unique<View>();
        tile->flex().direction = FlexDirection::column;
        tile->flex().align_items = FlexAlign::center;
        tile->flex().justify_content = FlexJustify::center;
        tile->flex().gap = 3;
        tile->flex().preferred_width = kTileWidth;
        tile->flex().preferred_height = kTileHeight;
        tile->flex().flex_shrink = 0;

        // Determine widget type based on parameter range
        bool is_toggle = (param.range.min == 0.0f && param.range.max == 1.0f &&
                         param.range.step >= 0.9f);

        if (is_toggle) {
            auto toggle = std::make_unique<Toggle>();
            toggle->set_id(param.name);
            toggle->set_label(param.name);
            toggle->set_on(store.get_normalized(param.id) > 0.5f);
            toggle->on_toggle = [&store, id = param.id](bool on) {
                store.set_normalized(id, on ? 1.0f : 0.0f);
            };
            toggle->flex().preferred_height = 30;
            toggle->flex().preferred_width = 54;
            tile->add_child(std::move(toggle));
        } else {
            auto knob = std::make_unique<Knob>();
            knob->set_id(param.name);
            knob->set_label(param.name);
            // AutoUi owns separate label/value rows below the dial. Keep the
            // knob's visible internal text off so neither string rides over the
            // control artwork; set_label above still supplies its accessible name.
            knob->set_show_label(false);
            knob->set_show_value(false);
            knob->set_value(store.get_normalized(param.id));
            knob->on_change = [&store, id = param.id](float norm) {
                store.set_normalized(id, norm);
            };
            // Keep the physical-unit formatter on the Knob even though AutoUi
            // paints that value in its own row. AccessibilityValueInterface uses
            // the formatter for the value announced by platform assistive tech.
            knob->set_format([param](float norm) {
                return format_parameter_value(param, norm);
            });
            knob->flex().preferred_height = 64;
            knob->flex().preferred_width = 64;
            tile->add_child(std::move(knob));

            auto label = std::make_unique<Label>(param.name);
            label->set_font_size(10.0f);
            label->flex().preferred_height = 13.0f;
            tile->add_child(std::move(label));

            auto value = std::make_unique<Label>(format_parameter_value(
                param, store.get_normalized(param.id)));
            value->set_id(value_label_id(param.id));
            value->set_font_size(10.0f);
            value->flex().preferred_height = 13.0f;
            tile->add_child(std::move(value));
        }

        return tile;
    };

    auto make_grid = [&](const auto& selected_params) {
        auto grid = std::make_unique<View>();
        grid->flex().direction = FlexDirection::row;
        grid->flex().flex_wrap = FlexWrap::wrap;
        grid->flex().gap = kTileGap;
        grid->flex().justify_content = FlexJustify::center;
        grid->flex().align_items = FlexAlign::center;
        grid->flex().align_content = FlexAlign::center;
        grid->flex().max_width = kMaxGridWidth;
        for (const auto& param : selected_params)
            grid->add_child(make_tile(param));
        return grid;
    };

    const auto params = store.all_params();
    const auto groups = store.all_groups();
    std::vector<AutoUiBody::GroupLayout> group_layouts;

    if (!groups.empty()) {
        auto content = std::make_unique<View>();
        auto* content_raw = content.get();
        content->flex().direction = FlexDirection::column;
        content->flex().align_items = FlexAlign::center;
        content->flex().gap = 10.0f;
        content->flex().padding = 6.0f;

        auto add_group = [&](std::string title,
                             const std::vector<state::ParamInfo>& selected) {
            if (selected.empty()) return;
            auto box = std::make_unique<GroupBox>();
            auto* box_raw = box.get();
            box->set_title(std::move(title));
            box->flex().direction = FlexDirection::column;
            box->flex().align_items = FlexAlign::center;
            box->flex().padding_left = 8.0f;
            box->flex().padding_right = 8.0f;
            box->flex().padding_top = GroupBox::header_height + 6.0f;
            box->flex().padding_bottom = 8.0f;

            auto grid = make_grid(selected);
            auto* grid_raw = grid.get();
            box->add_child(std::move(grid));
            group_layouts.push_back({box_raw, grid_raw, selected.size()});
            content->add_child(std::move(box));
        };

        for (const auto& group : groups) {
            std::vector<state::ParamInfo> selected;
            for (const auto& param : params)
                if (param.group_id == group.id) selected.push_back(param);
            add_group(group.name, selected);
        }

        std::vector<state::ParamInfo> other;
        for (const auto& param : params) {
            const bool has_registered_group = std::any_of(
                groups.begin(), groups.end(), [&](const auto& group) {
                    return param.group_id == group.id;
                });
            if (!has_registered_group) other.push_back(param);
        }
        add_group("Other", other);

        if (!group_layouts.empty()) {
            body_raw->set_grouped_content(content_raw, std::move(group_layouts));
            body->add_child(std::move(content));
        }
    }

    // Preserve the historic root → body → grid shape when no registered
    // group actually owns parameters. Existing small plug-ins remain visually
    // identical; only their body gains latent scroll capability.
    if (body->child_count() == 0) {
        auto grid = make_grid(params);
        auto* grid_raw = grid.get();
        body_raw->set_ungrouped_grid(grid_raw, params.size());
        body->add_child(std::move(grid));
    }

    root->add_child(std::move(body));
    return root;
}

AutoUi::SizeHint AutoUi::preferred_size(state::StateStore& store) {
    const auto params = store.all_params();
    const auto groups = store.all_groups();

    // Mirror build()'s group selection: count params per registered group, plus
    // an "Other" bucket for params that belong to no registered group. Only
    // groups that actually own params contribute a box (empty groups are
    // dropped, exactly as add_group() skips an empty selection).
    std::vector<std::size_t> group_counts;
    if (!groups.empty()) {
        for (const auto& group : groups) {
            std::size_t count = 0;
            for (const auto& param : params)
                if (param.group_id == group.id) ++count;
            if (count > 0) group_counts.push_back(count);
        }
        std::size_t other = 0;
        for (const auto& param : params) {
            const bool registered = std::any_of(
                groups.begin(), groups.end(),
                [&](const auto& g) { return param.group_id == g.id; });
            if (!registered) ++other;
        }
        if (other > 0) group_counts.push_back(other);
    }

    float width = 0.0f;
    float height = 0.0f;
    if (!group_counts.empty()) {
        // Grouped: stack the group boxes and take the widest grid, matching
        // update_content_extent()'s grouped branch.
        float widest_grid = 0.0f;
        float content_height = 2.0f * kGroupOuterPadding;
        for (std::size_t i = 0; i < group_counts.size(); ++i) {
            const auto ext =
                grid_extent(group_counts[i], columns_for(group_counts[i]));
            widest_grid = std::max(widest_grid, ext.width);
            content_height += GroupBox::header_height + kGroupHeaderExtra +
                              ext.height + kGroupBottomPadding;
            if (i + 1 < group_counts.size()) content_height += kGroupGap;
        }
        const float content_w = widest_grid + 2.0f * kGroupSidePadding;
        width = content_w + kBodyContentInset + 2.0f * kRootPadding;
        height = content_height + kTitleHeight + kRootGap + 2.0f * kRootPadding;
    } else {
        // Ungrouped: one wrapped grid. Sizing the body content width to exactly
        // the grid width makes wrapped_height() settle on the intended column
        // count, so the reported height matches the rendered row count.
        const std::size_t n = params.size();
        const auto ext = grid_extent(n, columns_for(n));
        width = ext.width + kBodyContentInset + 2.0f * kRootPadding;
        height = ext.height + kTitleHeight + kRootGap + 2.0f * kRootPadding;
    }

    return {
        std::max(kMinPreferredWidth,
                 static_cast<std::uint32_t>(std::ceil(width))),
        std::max(kMinPreferredHeight,
                 static_cast<std::uint32_t>(std::ceil(height))),
    };
}

void AutoUi::sync(View& root, state::StateStore& store) {
    auto params = store.all_params();

    for (auto& param : params) {
        // Walk the view tree to find widget matching this param name
        std::function<void(View&)> visit = [&](View& view) {
            if (view.id() == param.name) {
                float norm = store.get_normalized(param.id);
                if (auto* knob = dynamic_cast<Knob*>(&view))
                    knob->set_value(norm);
                else if (auto* toggle = dynamic_cast<Toggle*>(&view))
                    toggle->set_on(norm > 0.5f);
                else if (auto* fader = dynamic_cast<Fader*>(&view))
                    fader->set_value(norm);
            } else if (view.id() == value_label_id(param.id)) {
                if (auto* label = dynamic_cast<Label*>(&view))
                    label->set_text(format_parameter_value(
                        param, store.get_normalized(param.id)));
            }
            for (size_t i = 0; i < view.child_count(); ++i)
                visit(*view.child_at(i));
        };
        visit(root);
    }
}

} // namespace pulp::view

#include <pulp/view/auto_ui.hpp>
#include <pulp/view/ui_components.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <vector>

namespace pulp::view {

namespace {

constexpr float kTileWidth = 82.0f;
constexpr float kTileHeight = 112.0f;
constexpr float kTileGap = 14.0f;
constexpr float kMaxGridWidth = 760.0f;

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
            kMaxGridWidth, std::max(kTileWidth, viewport_width - 10.0f));

        if (grouped_content_) {
            constexpr float outer_padding = 6.0f;
            constexpr float group_gap = 10.0f;
            constexpr float group_side_padding = 8.0f;
            constexpr float group_bottom_padding = 8.0f;

            float content_height = 2.0f * outer_padding;
            for (std::size_t i = 0; i < groups_.size(); ++i) {
                auto& group = groups_[i];
                const float grid_width = std::max(
                    kTileWidth, content_width - 2.0f * group_side_padding);
                const float grid_height = wrapped_height(group.parameter_count,
                                                         grid_width);
                group.grid->flex().preferred_width = grid_width;
                group.grid->flex().preferred_height = grid_height;
                group.box->flex().preferred_width = content_width;
                group.box->flex().preferred_height =
                    GroupBox::header_height + 6.0f + grid_height +
                    group_bottom_padding;
                content_height += group.box->flex().preferred_height;
                if (i + 1 < groups_.size()) content_height += group_gap;
            }

            grouped_content_->flex().preferred_width = viewport_width;
            grouped_content_->flex().preferred_height = content_height;
            set_content_size({viewport_width,
                              std::max(viewport.height, content_height)});
        } else if (grid_) {
            const float grid_height = wrapped_height(parameter_count_, content_width);
            grid_->flex().preferred_width = content_width;
            grid_->flex().preferred_height = grid_height;
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
// Caveat: editor_size() is intentionally NOT auto-overridden by AutoUi.
// That virtual is processor-owned and has no StateStore input. Layout
// must look correct at the host's chosen size; an AutoUi-aware size
// hint is a separate feature if Pulp wants smarter defaults later.

std::unique_ptr<View> AutoUi::build(state::StateStore& store) {
    auto root = std::make_unique<View>();
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

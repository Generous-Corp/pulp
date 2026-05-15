#include <pulp/view/auto_ui.hpp>
#include <sstream>
#include <iomanip>

namespace pulp::view {

// pulp #97 — make AutoUi look intentional at any param count.
//
// AutoUi is the default editor for Processors that don't supply a
// custom create_view() (and aren't using a scripted UI). That's the
// majority of in-tree examples and the natural starting point for
// `pulp create my-plugin`. Previously, the layout was a single
// `flex-start`-aligned non-wrapping row, so a 4-knob plugin in the
// stock 400x300 editor window rendered the cluster in the top-left
// with ~70% of the window empty — looked like a layout bug.
//
// New layout (Codex-consult on 2026-05-14):
//
//   root: column, padding 14, gap 12
//   ├── title: "Parameters" 16pt
//   └── body: column, flex_grow=1, justify=center, align_items=center
//        └── grid: row, flex_wrap=wrap, justify=center, align_items=center,
//                  align_content=center, max_width=760
//             └── tile per param (column, fixed 82x96, no shrink)
//                  └── Knob 64x64  OR  Toggle 54x30
//
// Properties of the new shape:
//   - 1 param → centered single tile
//   - 4 params → centered cluster, 4 across in a stock 400x300 window
//   - 16 params → wraps to multiple rows, still centered
//   - 32+ params → wraps but exceeds visible area in stock window;
//                  proper fix is scroll/paging via a future SDK
//                  capability, NOT a static-layout trick
//
// Caveat: editor_size() is intentionally NOT auto-overridden by AutoUi.
// That virtual is processor-owned and has no StateStore input. Layout
// must look correct at the host's chosen size; an AutoUi-aware size
// hint is a separate feature if Pulp wants smarter defaults later.

std::unique_ptr<View> AutoUi::build(state::StateStore& store) {
    auto format_value = [](const state::ParamInfo& param, float norm) {
        float val = param.range.denormalize(norm);
        std::ostringstream ss;
        if (std::abs(val) >= 100) ss << std::fixed << std::setprecision(0);
        else if (std::abs(val) >= 10) ss << std::fixed << std::setprecision(1);
        else                          ss << std::fixed << std::setprecision(2);
        ss << val;
        if (!param.unit.empty()) ss << " " << param.unit;
        return ss.str();
    };

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

    // Body fills remaining vertical space and centers its single child
    // (the param grid) in both axes — that's what gives "1 knob in a
    // 400x300 window" the same intentional look as "16 knobs in a
    // 800x600 window."
    auto body = std::make_unique<View>();
    body->flex().direction = FlexDirection::column;
    body->flex().flex_grow = 1;
    body->flex().justify_content = FlexJustify::center;
    body->flex().align_items = FlexAlign::center;

    // Grid: wrapping flex row of fixed-size tiles, centered along all
    // three axes (main, cross, and cross-when-wrapped). max_width caps
    // the row so the grid stays readable on very wide editor windows
    // and pushes wrapping into a denser block.
    auto grid = std::make_unique<View>();
    grid->flex().direction = FlexDirection::row;
    grid->flex().flex_wrap = FlexWrap::wrap;
    grid->flex().gap = 14;
    grid->flex().justify_content = FlexJustify::center;
    grid->flex().align_items = FlexAlign::center;
    grid->flex().align_content = FlexAlign::center;
    grid->flex().max_width = 760;

    auto params = store.all_params();
    for (auto& param : params) {
        auto tile = std::make_unique<View>();
        tile->flex().direction = FlexDirection::column;
        tile->flex().align_items = FlexAlign::center;
        tile->flex().justify_content = FlexJustify::center;
        tile->flex().gap = 6;
        tile->flex().preferred_width = 82;
        tile->flex().preferred_height = 96;
        tile->flex().flex_shrink = 0;

        // Determine widget type based on parameter range
        bool is_toggle = (param.range.min == 0.0f && param.range.max == 1.0f &&
                         param.range.step >= 0.9f);

        if (is_toggle) {
            auto toggle = std::make_unique<Toggle>();
            toggle->set_id(param.name);
            toggle->set_label(param.name);
            toggle->set_on(store.get_normalized(param.id) > 0.5f);
            toggle->flex().preferred_height = 30;
            toggle->flex().preferred_width = 54;
            tile->add_child(std::move(toggle));
        } else {
            auto knob = std::make_unique<Knob>();
            knob->set_id(param.name);
            knob->set_label(param.name);
            knob->set_value(store.get_normalized(param.id));
            // Capture the per-param formatter by value so each knob owns
            // its own closure (params is a temporary).
            knob->set_format([param, format_value](float norm) {
                return format_value(param, norm);
            });
            knob->flex().preferred_height = 64;
            knob->flex().preferred_width = 64;
            tile->add_child(std::move(knob));
        }

        grid->add_child(std::move(tile));
    }

    body->add_child(std::move(grid));
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
            }
            for (size_t i = 0; i < view.child_count(); ++i)
                visit(*view.child_at(i));
        };
        visit(root);
    }
}

} // namespace pulp::view

#include <pulp/view/auto_ui.hpp>
#include <sstream>
#include <iomanip>

namespace pulp::view {

std::unique_ptr<View> AutoUi::build(state::StateStore& store) {
    auto root = std::make_unique<View>();
    root->flex().direction = FlexDirection::column;
    root->flex().padding = 12;
    root->flex().gap = 8;

    // Title
    auto title = std::make_unique<Label>("Parameters");
    title->set_font_size(18.0f);
    title->flex().preferred_height = 24;
    root->add_child(std::move(title));

    // Create a row container for controls
    auto row = std::make_unique<View>();
    row->flex().direction = FlexDirection::row;
    row->flex().gap = 16;
    row->flex().flex_grow = 1;

    auto params = store.all_params();
    for (auto& param : params) {
        auto col = std::make_unique<View>();
        col->flex().direction = FlexDirection::column;
        col->flex().gap = 4;
        col->flex().preferred_width = 64;

        // Determine widget type based on parameter range
        bool is_toggle = (param.range.min == 0.0f && param.range.max == 1.0f &&
                         param.range.step >= 0.9f);

        if (is_toggle) {
            auto toggle = std::make_unique<Toggle>();
            toggle->set_id(param.name);
            toggle->set_label(param.name);
            toggle->set_on(store.get_normalized(param.id) > 0.5f);
            toggle->flex().preferred_height = 28;
            toggle->flex().preferred_width = 50;
            col->add_child(std::move(toggle));
        } else {
            auto knob = std::make_unique<Knob>();
            knob->set_id(param.name);
            knob->set_label(param.name);
            knob->set_value(store.get_normalized(param.id));

            // Format function showing value with unit
            auto unit = param.unit;
            auto range = param.range;
            knob->set_format([unit, range](float norm_val) {
                float val = range.denormalize(norm_val);
                std::ostringstream ss;
                if (std::abs(val) >= 100) ss << std::fixed << std::setprecision(0) << val;
                else if (std::abs(val) >= 10) ss << std::fixed << std::setprecision(1) << val;
                else ss << std::fixed << std::setprecision(2) << val;
                if (!unit.empty()) ss << " " << unit;
                return ss.str();
            });

            knob->flex().preferred_height = 64;
            knob->flex().preferred_width = 64;
            col->add_child(std::move(knob));
        }

        row->add_child(std::move(col));
    }

    root->add_child(std::move(row));
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

// widget_bridge/widget_value_basic_api.cpp - basic widget value registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <pulp/view/ui_components.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void BridgeRegistrars::register_widget_value_label_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // Property setters
    register_bridge_function(api, "setLabel", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto text = args.get<std::string>(1, "");
        auto* v = self.widget(id);
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_label(text);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_label(text);
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_label(text);
        else if (auto* tb = dynamic_cast<ToggleButton*>(v)) tb->set_label(text);
        else if (auto* l = dynamic_cast<Label*>(v)) l->set_text(text);
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_widget_value_basic_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setStyle — placeholder until KnobStyle/ToggleStyle enums added to main widgets
    register_bridge_function(api, "setStyle", [](choc::javascript::ArgumentList) {
        return choc::value::Value();
    });

    // setWidgetStyle(id, "standard"|"minimal"|"silver") — switch rendering mode
    // "minimal" draws simple shapes matching design tools (circles, thin tracks)
    // "silver" draws a skeuomorphic chrome-body knob (figma-import alt path)
    register_bridge_function(api, "setWidgetStyle", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto style_str = args.get<std::string>(1, "standard");
        WidgetRenderStyle style;
        if (style_str == "minimal") style = WidgetRenderStyle::minimal;
        else if (style_str == "silver") style = WidgetRenderStyle::silver;
        else                          style = WidgetRenderStyle::standard;
        auto* v = self.widget(id);
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_render_style(style);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_render_style(style);
        else if (auto* m = dynamic_cast<Meter*>(v)) m->set_render_style(style);
        return choc::value::Value();
    });

    register_bridge_function(api, "setItems", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        if (auto* c = dynamic_cast<ComboBox*>(self.widget(id))) {
            std::vector<std::string> items;
            if (args.numArgs > 1 && args[1]) {
                auto& arr = *args[1];
                for (uint32_t i = 0; i < arr.size(); ++i)
                    items.push_back(std::string(arr[i].toString()));
            }
            c->set_items(std::move(items));
        }
        return choc::value::Value();
    });

    // setSelected(id, index) — set ComboBox selected index without firing on_change
    register_bridge_function(api, "setSelected", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto idx = args.get<int>(1, 0);
        if (auto* c = dynamic_cast<ComboBox*>(self.widget(id))) {
            c->set_selected_silent(idx);
        }
        return choc::value::Value();
    });
}

} // namespace pulp::view

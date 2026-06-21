// widget_bridge/widget_value_controls_api.cpp - scalar control value registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/gap_widgets.hpp>
#include "api_registry.hpp"

#include <string>

namespace pulp::view {

void WidgetBridge::register_widget_value_controls_api() {
    BridgeApiContext api{engine_};

    // setValue(id, value) -> set widget value
    // For Knob / Fader / Toggle this is normalised 0..1.
    // For RangeSlider it's the raw value in [min,max]; the widget clamps
    // and quantises against its own configured range.
    register_bridge_function(api, "setValue", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto value = args.get<double>(1, 0);

        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();

        if (auto* knob = dynamic_cast<Knob*>(it->second))
            knob->set_value(static_cast<float>(value));
        else if (auto* fader = dynamic_cast<Fader*>(it->second))
            fader->set_value(static_cast<float>(value));
        else if (auto* range = dynamic_cast<RangeSlider*>(it->second))
            range->set_value(static_cast<float>(value));
        else if (auto* toggle = dynamic_cast<Toggle*>(it->second))
            toggle->set_on(value > 0.5);
        else if (auto* cb = dynamic_cast<Checkbox*>(it->second))
            cb->set_checked(value > 0.5);
        else if (auto* tb = dynamic_cast<ToggleButton*>(it->second))
            tb->set_on(value > 0.5);
        else if (auto* stepper = dynamic_cast<Stepper*>(it->second))
            stepper->set_value(value);
        else if (auto* pan = dynamic_cast<PanControl*>(it->second))
            pan->set_value(static_cast<float>(value));

        return choc::value::Value();
    });

    // getValue(id) -> get widget value (normalised for Knob/Fader/Toggle,
    // raw for RangeSlider).
    register_bridge_function(api, "getValue", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");

        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::createFloat64(0);

        if (auto* knob = dynamic_cast<Knob*>(it->second))
            return choc::value::createFloat64(knob->value());
        if (auto* fader = dynamic_cast<Fader*>(it->second))
            return choc::value::createFloat64(fader->value());
        if (auto* range = dynamic_cast<RangeSlider*>(it->second))
            return choc::value::createFloat64(range->value());
        if (auto* toggle = dynamic_cast<Toggle*>(it->second))
            return choc::value::createFloat64(toggle->is_on() ? 1.0 : 0.0);

        return choc::value::createFloat64(0);
    });

    // RangeSlider configuration setters.
    // setMin/setMax/setStep mirror the HTMLInputElement attributes
    // `min`, `max`, `step`. Each one re-applies the widget's clamp +
    // quantisation pipeline so out-of-range values stay consistent.
    register_bridge_function(api, "setMin", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto v = args.get<double>(1, 0);
        if (auto* range = dynamic_cast<RangeSlider*>(widget(id)))
            range->set_min(static_cast<float>(v));
        return choc::value::Value();
    });

    register_bridge_function(api, "setMax", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto v = args.get<double>(1, 1);
        if (auto* range = dynamic_cast<RangeSlider*>(widget(id)))
            range->set_max(static_cast<float>(v));
        return choc::value::Value();
    });

    register_bridge_function(api, "setStep", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto v = args.get<double>(1, 0);
        if (auto* range = dynamic_cast<RangeSlider*>(widget(id)))
            range->set_step(static_cast<float>(v));
        return choc::value::Value();
    });

    // setOrientation(id, "horizontal" | "vertical")
    // RangeSlider and Fader consume this today; future widgets that need an
    // orientation can extend this dynamic_cast chain.
    register_bridge_function(api, "setOrientation", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto orient = args.get<std::string>(1, "horizontal");
        auto* w = widget(id);
        if (auto* range = dynamic_cast<RangeSlider*>(w)) {
            range->set_orientation(orient == "vertical"
                ? RangeSlider::Orientation::vertical
                : RangeSlider::Orientation::horizontal);
        } else if (auto* fader = dynamic_cast<Fader*>(w)) {
            fader->set_orientation(orient == "horizontal"
                ? Fader::Orientation::horizontal
                : Fader::Orientation::vertical);
        }
        return choc::value::Value();
    });

    // setAccentColor(id, "#hex" | "rgb(...)" | "")
    // Empty string clears the override and returns to the active theme's
    // `control.fill` / `control.thumb` tokens.
    register_bridge_function(api, "setAccentColor", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        if (auto* range = dynamic_cast<RangeSlider*>(widget(id))) {
            if (hex.empty()) {
                range->clear_accent_color();
            } else {
                // parseColor is defined later in register_api; rebuild the
                // same parser inline so registration order does not matter.
                canvas::Color c = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
                if (!hex.empty() && hex[0] == '#') {
                    if (hex.size() == 4) {
                        c.r = static_cast<float>(std::stoul(std::string(2, hex[1]), nullptr, 16)) / 255.0f;
                        c.g = static_cast<float>(std::stoul(std::string(2, hex[2]), nullptr, 16)) / 255.0f;
                        c.b = static_cast<float>(std::stoul(std::string(2, hex[3]), nullptr, 16)) / 255.0f;
                    } else if (hex.size() >= 7) {
                        c.r = static_cast<float>(std::stoul(hex.substr(1,2), nullptr, 16)) / 255.0f;
                        c.g = static_cast<float>(std::stoul(hex.substr(3,2), nullptr, 16)) / 255.0f;
                        c.b = static_cast<float>(std::stoul(hex.substr(5,2), nullptr, 16)) / 255.0f;
                        if (hex.size() >= 9)
                            c.a = static_cast<float>(std::stoul(hex.substr(7,2), nullptr, 16)) / 255.0f;
                    }
                }
                range->set_accent_color(c);
            }
        }
        return choc::value::Value();
    });
}

} // namespace pulp::view

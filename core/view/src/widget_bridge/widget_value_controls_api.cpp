// widget_bridge/widget_value_controls_api.cpp - scalar control value registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/gap_widgets.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <string>

namespace pulp::view {

void BridgeRegistrars::register_widget_value_controls_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setValue(id, value) -> set widget value
    // For Knob / Fader / Toggle this is normalized 0..1.
    // For RangeSlider it's the raw value in [min,max]; the widget clamps
    // and quantises against its own configured range.
    register_bridge_function(api, "setValue", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto value = args.get<double>(1, 0);

        auto it = self.widgets_.find(id);
        if (it == self.widgets_.end()) return choc::value::Value();

        if (auto* knob = dynamic_cast<Knob*>(it->second.view))
            knob->set_value(static_cast<float>(value));
        else if (auto* fader = dynamic_cast<Fader*>(it->second.view))
            fader->set_value(static_cast<float>(value));
        else if (auto* range = dynamic_cast<RangeSlider*>(it->second.view))
            range->set_value(static_cast<float>(value));
        else if (auto* toggle = dynamic_cast<Toggle*>(it->second.view))
            toggle->set_on(toggle_on_from_normalized(value));
        else if (auto* cb = dynamic_cast<Checkbox*>(it->second.view))
            cb->set_checked(toggle_on_from_normalized(value));
        else if (auto* tb = dynamic_cast<ToggleButton*>(it->second.view))
            tb->set_on(toggle_on_from_normalized(value));
        else if (auto* stepper = dynamic_cast<Stepper*>(it->second.view))
            stepper->set_value(value);
        else if (auto* pan = dynamic_cast<PanControl*>(it->second.view))
            pan->set_value(static_cast<float>(value));
        // A selector has segments, not a position: the normalized value picks
        // which one is lit. Silent, because this is the host pushing state in
        // — echoing it back as a user edit would fight the binding every frame.
        else if (auto* seg = dynamic_cast<SegmentedControl*>(it->second.view))
            seg->set_selected_silent(selector_segment_index(
                static_cast<float>(value),
                static_cast<int>(seg->segments().size())));

        return choc::value::Value();
    });

    // getValue(id) -> get widget value (normalized for Knob/Fader/Toggle,
    // raw for RangeSlider).
    register_bridge_function(api, "getValue", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");

        auto it = self.widgets_.find(id);
        if (it == self.widgets_.end()) return choc::value::createFloat64(0);

        if (auto* knob = dynamic_cast<Knob*>(it->second.view))
            return choc::value::createFloat64(knob->value());
        if (auto* seg = dynamic_cast<SegmentedControl*>(it->second.view))
            return choc::value::createFloat64(selector_segment_value(
                seg->selected(), static_cast<int>(seg->segments().size())));
        if (auto* fader = dynamic_cast<Fader*>(it->second.view))
            return choc::value::createFloat64(fader->value());
        if (auto* range = dynamic_cast<RangeSlider*>(it->second.view))
            return choc::value::createFloat64(range->value());
        if (auto* toggle = dynamic_cast<Toggle*>(it->second.view))
            return choc::value::createFloat64(toggle->is_on() ? 1.0 : 0.0);
        if (auto* stepper = dynamic_cast<Stepper*>(it->second.view))
            return choc::value::createFloat64(stepper->value());

        return choc::value::createFloat64(0);
    });

    // RangeSlider configuration setters.
    // setMin/setMax/setStep mirror the HTMLInputElement attributes
    // `min`, `max`, `step`. Each one re-applies the widget's clamp +
    // quantisation pipeline so out-of-range values stay consistent.
    register_bridge_function(api, "setMin", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto v = args.get<double>(1, 0);
        if (auto* range = dynamic_cast<RangeSlider*>(self.widget(id)))
            range->set_min(static_cast<float>(v));
        // A stepper's range is the grid its plain value lives on, and both
        // emitters already declare it through setMin/setMax; without this the
        // widget keeps its -24..24 default and a voice count reads as an
        // octave offset.
        else if (auto* st = dynamic_cast<Stepper*>(self.widget(id)))
            st->set_minimum(v);
        return choc::value::Value();
    });

    register_bridge_function(api, "setMax", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto v = args.get<double>(1, 1);
        if (auto* range = dynamic_cast<RangeSlider*>(self.widget(id)))
            range->set_max(static_cast<float>(v));
        else if (auto* st = dynamic_cast<Stepper*>(self.widget(id)))
            st->set_maximum(v);
        return choc::value::Value();
    });

    register_bridge_function(api, "setStep", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto v = args.get<double>(1, 0);
        if (auto* range = dynamic_cast<RangeSlider*>(self.widget(id)))
            range->set_step(static_cast<float>(v));
        else if (auto* st = dynamic_cast<Stepper*>(self.widget(id)))
            st->set_step(v);
        return choc::value::Value();
    });

    // setOrientation(id, "horizontal" | "vertical")
    // RangeSlider and Fader consume this today; future widgets that need an
    // orientation can extend this dynamic_cast chain.
    register_bridge_function(api, "setOrientation", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto orient = args.get<std::string>(1, "horizontal");
        auto* w = self.widget(id);
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

    // setAccentColor(id, CSS color | "")
    // Empty string clears the override and returns the active fill to the
    // theme. The thumb remains an independent opaque platform color.
    register_bridge_function(api, "setAccentColor", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        if (auto* range = dynamic_cast<RangeSlider*>(self.widget(id))) {
            if (hex.empty()) {
                range->clear_accent_color();
            } else {
                range->set_accent_color(parse_bridge_css_color(hex));
            }
        }
        return choc::value::Value();
    });

    auto toggle_of = [&self](const std::string& id) -> ToggleButton* {
        return dynamic_cast<ToggleButton*>(id.empty() ? &self.root_ : self.widget(id));
    };

    // A momentary action on a ToggleButton latches: press once and it acts,
    // press again and it only un-latches, so every second press appeared to do
    // nothing. Scripts need to be able to release it.
    register_bridge_function(api, "setToggleOn",
                             [toggle_of](choc::javascript::ArgumentList args) {
        auto* t = toggle_of(args.get<std::string>(0, ""));
        if (t) t->set_on(args.get<bool>(1, false));
        return choc::value::Value();
    });

    // Two toggles that look mutually exclusive must BE mutually exclusive.
    // Without a shared group both tabs could read as selected at once, and the
    // mode became whichever was clicked last rather than what was shown.
    register_bridge_function(api, "setRadioGroup",
                             [toggle_of](choc::javascript::ArgumentList args) {
        auto* t = toggle_of(args.get<std::string>(0, ""));
        if (t) t->set_radio_group(static_cast<int>(args.get<double>(1, 0)));
        return choc::value::Value();
    });
}

} // namespace pulp::view

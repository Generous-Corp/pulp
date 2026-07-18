// widget_bridge/widget_value_content_api.cpp - content value registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>

#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void BridgeRegistrars::register_widget_value_content_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "setProgress", [&self](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<ProgressBar*>(self.widget(args.get<std::string>(0, ""))))
            p->set_progress(static_cast<float>(args.get<double>(1, 0)));
        return choc::value::Value();
    });

    register_bridge_function(api, "setMeterLevel", [&self](choc::javascript::ArgumentList args) {
        if (auto* m = dynamic_cast<Meter*>(self.widget(args.get<std::string>(0, ""))))
            m->set_level(static_cast<float>(args.get<double>(1, 0)),
                        static_cast<float>(args.get<double>(2, 0)));
        return choc::value::Value();
    });

    register_bridge_function(api, "setXY", [&self](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<XYPad*>(self.widget(args.get<std::string>(0, "")))) {
            p->set_x(static_cast<float>(args.get<double>(1, .5)));
            p->set_y(static_cast<float>(args.get<double>(2, .5)));
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setWaveformData", [&self](choc::javascript::ArgumentList args) {
        if (auto* w = dynamic_cast<WaveformView*>(self.widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1 && args[1]) {
                auto& a = *args[1]; std::vector<float> d(a.size());
                for (uint32_t i = 0; i < a.size(); ++i) d[i] = static_cast<float>(a[i].getWithDefault<double>(0));
                w->set_data(std::move(d));
            }
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setSpectrumData", [&self](choc::javascript::ArgumentList args) {
        if (auto* s = dynamic_cast<SpectrumView*>(self.widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1 && args[1]) {
                auto& a = *args[1]; std::vector<float> d(a.size());
                for (uint32_t i = 0; i < a.size(); ++i) d[i] = static_cast<float>(a[i].getWithDefault<double>(0));
                s->set_spectrum(std::move(d));
            }
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setPlaceholder", [&self](choc::javascript::ArgumentList args) {
        if (auto* e = dynamic_cast<TextEditor*>(self.widget(args.get<std::string>(0, ""))))
            e->placeholder = args.get<std::string>(1, "");
        return choc::value::Value();
    });

    register_bridge_function(api, "setText", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto t = args.get<std::string>(1, "");
        auto* v = self.widget(id);
        if (auto* e = dynamic_cast<TextEditor*>(v)) e->set_text(t);
        else if (auto* l = dynamic_cast<Label*>(v)) l->set_text(t);
        else if (auto* b = dynamic_cast<Badge*>(v)) b->set_text(t);
        return choc::value::Value();
    });

    register_bridge_function(api, "getText", [&self](choc::javascript::ArgumentList args) {
        auto* v = self.widget(args.get<std::string>(0, ""));
        if (auto* e = dynamic_cast<TextEditor*>(v)) return choc::value::createString(e->text());
        if (auto* l = dynamic_cast<Label*>(v)) return choc::value::createString(l->text());
        return choc::value::createString("");
    });

    register_bridge_function(api, "setPanelStyle", [&self](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<Panel*>(self.widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1) p->set_background_token(args.get<std::string>(1, "bg.surface"));
            if (args.numArgs > 2) p->set_border_token(args.get<std::string>(2, "control.border"));
            if (args.numArgs > 3) p->set_corner_radius(static_cast<float>(args.get<double>(3, 8)));
            if (args.numArgs > 4) p->set_border_width(static_cast<float>(args.get<double>(4, 1)));
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setScrollContentSize", [&self](choc::javascript::ArgumentList args) {
        if (auto* s = dynamic_cast<ScrollView*>(self.widget(args.get<std::string>(0, ""))))
            s->set_content_size({static_cast<float>(args.get<double>(1,0)),
                                static_cast<float>(args.get<double>(2,0))});
        return choc::value::Value();
    });
}

} // namespace pulp::view

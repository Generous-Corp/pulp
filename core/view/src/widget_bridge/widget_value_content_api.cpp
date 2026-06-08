// widget_bridge/widget_value_content_api.cpp - content value registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>

#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>

#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void WidgetBridge::register_widget_value_content_api() {
    engine_.register_function("setProgress", [this](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<ProgressBar*>(widget(args.get<std::string>(0, ""))))
            p->set_progress(static_cast<float>(args.get<double>(1, 0)));
        return choc::value::Value();
    });

    engine_.register_function("setMeterLevel", [this](choc::javascript::ArgumentList args) {
        if (auto* m = dynamic_cast<Meter*>(widget(args.get<std::string>(0, ""))))
            m->set_level(static_cast<float>(args.get<double>(1, 0)),
                        static_cast<float>(args.get<double>(2, 0)));
        return choc::value::Value();
    });

    engine_.register_function("setXY", [this](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<XYPad*>(widget(args.get<std::string>(0, "")))) {
            p->set_x(static_cast<float>(args.get<double>(1, .5)));
            p->set_y(static_cast<float>(args.get<double>(2, .5)));
        }
        return choc::value::Value();
    });

    engine_.register_function("setWaveformData", [this](choc::javascript::ArgumentList args) {
        if (auto* w = dynamic_cast<WaveformView*>(widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1 && args[1]) {
                auto& a = *args[1]; std::vector<float> d(a.size());
                for (uint32_t i = 0; i < a.size(); ++i) d[i] = static_cast<float>(a[i].getWithDefault<double>(0));
                w->set_data(std::move(d));
            }
        }
        return choc::value::Value();
    });

    engine_.register_function("setSpectrumData", [this](choc::javascript::ArgumentList args) {
        if (auto* s = dynamic_cast<SpectrumView*>(widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1 && args[1]) {
                auto& a = *args[1]; std::vector<float> d(a.size());
                for (uint32_t i = 0; i < a.size(); ++i) d[i] = static_cast<float>(a[i].getWithDefault<double>(0));
                s->set_spectrum(std::move(d));
            }
        }
        return choc::value::Value();
    });

    engine_.register_function("setPlaceholder", [this](choc::javascript::ArgumentList args) {
        if (auto* e = dynamic_cast<TextEditor*>(widget(args.get<std::string>(0, ""))))
            e->placeholder = args.get<std::string>(1, "");
        return choc::value::Value();
    });

    engine_.register_function("setText", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto t = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (auto* e = dynamic_cast<TextEditor*>(v)) e->set_text(t);
        else if (auto* l = dynamic_cast<Label*>(v)) l->set_text(t);
        return choc::value::Value();
    });

    engine_.register_function("getText", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (auto* e = dynamic_cast<TextEditor*>(v)) return choc::value::createString(e->text());
        if (auto* l = dynamic_cast<Label*>(v)) return choc::value::createString(l->text());
        return choc::value::createString("");
    });

    engine_.register_function("setPanelStyle", [this](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<Panel*>(widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1) p->set_background_token(args.get<std::string>(1, "bg.surface"));
            if (args.numArgs > 2) p->set_border_token(args.get<std::string>(2, "control.border"));
            if (args.numArgs > 3) p->set_corner_radius(static_cast<float>(args.get<double>(3, 8)));
            if (args.numArgs > 4) p->set_border_width(static_cast<float>(args.get<double>(4, 1)));
        }
        return choc::value::Value();
    });

    engine_.register_function("setScrollContentSize", [this](choc::javascript::ArgumentList args) {
        if (auto* s = dynamic_cast<ScrollView*>(widget(args.get<std::string>(0, ""))))
            s->set_content_size({static_cast<float>(args.get<double>(1,0)),
                                static_cast<float>(args.get<double>(2,0))});
        return choc::value::Value();
    });
}

} // namespace pulp::view

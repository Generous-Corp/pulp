// widget_bridge/widget_schema_api.cpp - widget schema and style preset registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <choc/text/choc_JSON.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace pulp::view {

void WidgetBridge::register_widget_schema_api() {
    BridgeApiContext api{engine_};

    // setWidgetSchema(id, schemaJSON) -> apply declarative widget schema
    register_bridge_function(api, "setWidgetSchema", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto json = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_widget_schema(json);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_widget_schema(json);
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_widget_schema(json);
        request_repaint();
        return choc::value::Value();
    });

    // setWidgetLottie(id, lottieJSON) -> store Lottie animation JSON on widget
    register_bridge_function(api, "setWidgetLottie", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto json = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        // Store Lottie JSON for JS-side rendering or future Skottie integration
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_lottie_json(json);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_lottie_json(json);
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_lottie_json(json);
        request_repaint();
        return choc::value::Value();
    });

    // seekWidgetLottie(id, normalizedTime) -> scrub Lottie to position (0-1)
    register_bridge_function(api, "seekWidgetLottie", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto t = static_cast<float>(args.get<double>(1, 0));
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_lottie_time(t);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_lottie_time(t);
        else if (auto* tg = dynamic_cast<Toggle*>(v)) tg->set_lottie_time(t);
        request_repaint();
        return choc::value::Value();
    });

    // clearWidgetSchema(id) -> remove schema, restore default paint
    register_bridge_function(api, "clearWidgetSchema", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_widget_schema("");
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_widget_schema("");
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_widget_schema("");
        request_repaint();
        return choc::value::Value();
    });

    // saveStylePreset(name, object) -> persist style payload as JSON under temp storage
    register_bridge_function(api, "saveStylePreset", [](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        if (name.empty() || args.numArgs < 2 || !args[1]) return choc::value::createBool(false);

        std::string safe_name;
        safe_name.reserve(name.size());
        for (char c : name) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_') {
                safe_name.push_back(c);
            } else if (c == ' ') {
                safe_name.push_back('_');
            }
        }
        if (safe_name.empty()) return choc::value::createBool(false);

        auto dir = std::filesystem::temp_directory_path() / "pulp-style-presets";
        std::filesystem::create_directories(dir);

        std::ofstream out(dir / (safe_name + ".json"));
        if (!out.is_open()) return choc::value::createBool(false);
        out << choc::json::toString(*args[1], true);
        return choc::value::createBool(true);
    });

    // loadStylePreset(name) -> load persisted JSON object or null
    register_bridge_function(api, "loadStylePreset", [](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        if (name.empty()) return choc::value::Value();

        std::string safe_name;
        safe_name.reserve(name.size());
        for (char c : name) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_') {
                safe_name.push_back(c);
            } else if (c == ' ') {
                safe_name.push_back('_');
            }
        }
        if (safe_name.empty()) return choc::value::Value();

        auto path = std::filesystem::temp_directory_path() / "pulp-style-presets" / (safe_name + ".json");
        if (!std::filesystem::exists(path)) return choc::value::Value();

        std::ifstream in(path);
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (json.empty()) return choc::value::Value();
        try {
            return choc::json::parse(json);
        } catch (...) {
            return choc::value::Value();
        }
    });
}

} // namespace pulp::view

// widget_bridge/widget_assets_api.cpp - asset and skin registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/sprite_strip.hpp>
#include "api_registry.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

namespace {

std::pair<canvas::Color, bool> parse_skin_hex(const std::string& hex) {
    canvas::Color c = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
    if (hex.empty() || hex[0] != '#') return {c, false};
    try {
        if (hex.size() == 4) {
            c.r = static_cast<float>(std::stoul(std::string(2, hex[1]), nullptr, 16)) / 255.0f;
            c.g = static_cast<float>(std::stoul(std::string(2, hex[2]), nullptr, 16)) / 255.0f;
            c.b = static_cast<float>(std::stoul(std::string(2, hex[3]), nullptr, 16)) / 255.0f;
        } else if (hex.size() >= 7) {
            c.r = static_cast<float>(std::stoul(hex.substr(1, 2), nullptr, 16)) / 255.0f;
            c.g = static_cast<float>(std::stoul(hex.substr(3, 2), nullptr, 16)) / 255.0f;
            c.b = static_cast<float>(std::stoul(hex.substr(5, 2), nullptr, 16)) / 255.0f;
            if (hex.size() >= 9)
                c.a = static_cast<float>(std::stoul(hex.substr(7, 2), nullptr, 16)) / 255.0f;
        } else {
            return {c, false};
        }
    } catch (...) {
        return {c, false};
    }
    return {c, true};
}

} // namespace

void BridgeRegistrars::register_widget_assets_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setImageSource(id, path) - set image file path.
    register_bridge_function(api, "setImageSource", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto path = args.get<std::string>(1, "");
        if (auto* img = dynamic_cast<ImageView*>(self.widget(id)))
            img->set_image_path(path);
        return choc::value::Value();
    });

    // setKnobSpriteStrip(id, pngPath, frameCount, orientation?) - Track A1.
    register_bridge_function(api, "setKnobSpriteStrip", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto path = args.get<std::string>(1, "");
        int frame_count = static_cast<int>(args.get<double>(2, 1));
        std::string orientation_s = args.get<std::string>(3, "vertical");

        auto* k = dynamic_cast<Knob*>(self.widget(id));
        if (!k || path.empty() || frame_count <= 0) return choc::value::Value();

        if (path.rfind("file://", 0) == 0) path = path.substr(7);

        std::ifstream f(path, std::ios::binary);
        if (!f.good()) {
            std::cerr << "[setKnobSpriteStrip] could not open " << path << "\n";
            return choc::value::Value();
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
        auto img = AssetManager::instance().load_image_from_memory(bytes.data(), bytes.size());
        if (!img.valid()) {
            std::cerr << "[setKnobSpriteStrip] PNG decode failed for " << path << "\n";
            return choc::value::Value();
        }

        auto strip = std::make_shared<SpriteStrip>();
        auto orientation = (orientation_s == "horizontal")
                               ? SpriteStrip::Orientation::horizontal
                               : SpriteStrip::Orientation::vertical;
        strip->load_from_file(path,
                              static_cast<int>(img.width),
                              static_cast<int>(img.height),
                              frame_count, orientation);
        k->set_sprite_strip(std::move(strip));
        k->request_repaint();
        return choc::value::Value();
    });

    // setKnobSpriteCore(id, core_x, core_y, core_w, core_h) - opaque-core rect.
    register_bridge_function(api, "setKnobSpriteCore", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* k = dynamic_cast<Knob*>(self.widget(id));
        if (!k) return choc::value::Value();
        k->set_sprite_core(static_cast<float>(args.get<double>(1, 0.0)),
                           static_cast<float>(args.get<double>(2, 0.0)),
                           static_cast<float>(args.get<double>(3, 0.0)),
                           static_cast<float>(args.get<double>(4, 0.0)));
        k->request_repaint();
        return choc::value::Value();
    });

    // setFaderSkin(id, trackColor, fillColor, thumbColor, thumbBorderColor?,
    //              thumbW?, thumbH?, cornerRadius?)
    register_bridge_function(api, "setFaderSkin",
        [&self](choc::javascript::ArgumentList args) {
            auto* f = dynamic_cast<Fader*>(self.widget(args.get<std::string>(0, "")));
            if (!f) return choc::value::Value();
            if (auto [c, ok] = parse_skin_hex(args.get<std::string>(1, "")); ok) f->set_skin_track_color(c);
            if (auto [c, ok] = parse_skin_hex(args.get<std::string>(2, "")); ok) f->set_skin_fill_color(c);
            if (auto [c, ok] = parse_skin_hex(args.get<std::string>(3, "")); ok) f->set_skin_thumb_color(c);
            if (auto [c, ok] = parse_skin_hex(args.get<std::string>(4, "")); ok) f->set_skin_thumb_border_color(c);
            float tw = static_cast<float>(args.get<double>(5, 0));
            float th = static_cast<float>(args.get<double>(6, 0));
            if (tw > 0.0f && th > 0.0f) f->set_thumb_size(tw, th);
            float cr = static_cast<float>(args.get<double>(7, 0));
            if (cr > 0.0f) f->set_thumb_corner_radius(cr);
            f->set_thumb_shape(Fader::ThumbShape::rectangle);
            f->request_repaint();
            return choc::value::Value();
        });

    // setFaderTrackWidth(id, widthPx)
    register_bridge_function(api, "setFaderTrackWidth",
        [&self](choc::javascript::ArgumentList args) {
            auto* f = dynamic_cast<Fader*>(self.widget(args.get<std::string>(0, "")));
            if (!f) return choc::value::Value();
            float w = static_cast<float>(args.get<double>(1, 0));
            if (w > 0.0f) { f->set_skin_track_width(w); f->request_repaint(); }
            return choc::value::Value();
        });

    // setFaderTrackBorder(id, "#rrggbb")
    register_bridge_function(api, "setFaderTrackBorder",
        [&self](choc::javascript::ArgumentList args) {
            auto* f = dynamic_cast<Fader*>(self.widget(args.get<std::string>(0, "")));
            if (!f) return choc::value::Value();
            if (auto [c, ok] = parse_skin_hex(args.get<std::string>(1, "")); ok) {
                f->set_skin_track_border_color(c);
                f->request_repaint();
            }
            return choc::value::Value();
        });

    // setMeterColors(id, backgroundColor, "#stop0,#stop1,#stop2,...")
    register_bridge_function(api, "setMeterColors",
        [&self](choc::javascript::ArgumentList args) {
            auto* m = dynamic_cast<Meter*>(self.widget(args.get<std::string>(0, "")));
            if (!m) return choc::value::Value();
            if (auto [bg, ok] = parse_skin_hex(args.get<std::string>(1, "")); ok)
                m->set_skin_background_color(bg);
            auto stops_str = args.get<std::string>(2, "");
            std::vector<canvas::Color> stops;
            size_t start = 0;
            while (start <= stops_str.size()) {
                size_t comma = stops_str.find(',', start);
                std::string tok = stops_str.substr(start, comma == std::string::npos
                                                              ? std::string::npos
                                                              : comma - start);
                while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
                while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.pop_back();
                if (!tok.empty()) {
                    if (auto [c, ok] = parse_skin_hex(tok); ok) stops.push_back(c);
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            if (stops.size() >= 2) m->set_skin_gradient(std::move(stops));
            else m->clear_skin();
            m->request_repaint();
            return choc::value::Value();
        });

    // setMeterBarRatio(id, ratio)
    register_bridge_function(api, "setMeterBarRatio",
        [&self](choc::javascript::ArgumentList args) {
            auto* m = dynamic_cast<Meter*>(self.widget(args.get<std::string>(0, "")));
            if (!m) return choc::value::Value();
            float r = static_cast<float>(args.get<double>(1, 0));
            if (r > 0.0f) { m->set_bar_fill_ratio(r); m->request_repaint(); }
            return choc::value::Value();
        });
}

} // namespace pulp::view

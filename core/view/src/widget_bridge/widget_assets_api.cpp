// widget_bridge/widget_assets_api.cpp - asset and skin registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/css_gradient.hpp>
#include <pulp/view/sprite_strip.hpp>
#include "api_registry.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::view {

namespace {

/// A skin colour argument, and whether it was recognized at all.
///
/// The bool is the whole point of the pair: every caller applies the colour only
/// when it parsed, so an argument the bridge cannot read leaves the widget's
/// existing colour alone instead of repainting it the parser's white default.
///
/// Recognition is delegated to the shared `parse_css_color` rather than spelled
/// again here. That parser already covers `hsl()`, `oklab()` and `oklch()`, which
/// is what a browser-captured design actually arrives as — Chromium serializes
/// every modern colour syntax into Oklab, whatever it was authored in — so a
/// hex-only reader silently dropped those to the caller's fallback while the
/// native materializer resolved them. One value read by two parsers of different
/// tolerance is how a control comes out right on one path and wrong on the other.
std::pair<canvas::Color, bool> parse_skin_color(const std::string& value) {
    const canvas::Color unparsed = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
    if (value.empty()) return {unparsed, false};
    if (value[0] == '#') {
        // `parse_css_color` returns its white default for a malformed hex run
        // rather than reporting failure, so the accepted lengths stay checked
        // here — otherwise `#ab12` would report success and paint white.
        if (value.size() != 4 && value.size() < 7) return {unparsed, false};
        return {parse_css_color(value), true};
    }
    if (value == "transparent") return {parse_css_color(value), true};
    // The functional forms `parse_css_color` understands. Anything else — a
    // named colour, a var(), a gradient — is reported unrecognized so the caller
    // keeps its fallback rather than receiving that parser's white.
    static constexpr std::string_view kFunctionalForms[] = {
        "rgb(", "rgba(", "hsl(", "hsla(", "oklab(", "oklch("};
    for (const auto form : kFunctionalForms)
        if (value.compare(0, form.size(), form) == 0)
            return {parse_css_color(value), true};
    return {unparsed, false};
}

} // namespace

void BridgeRegistrars::register_widget_assets_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setImageSource(id, path) - set image file path. Relative paths resolve
    // against the bridge's script base dir when one is set (self-contained
    // import artifacts reference `assets/<file>` next to their ui.js).
    register_bridge_function(api, "setImageSource", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto path = self.resolve_script_relative(args.get<std::string>(1, ""));
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
        path = self.resolve_script_relative(path);

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

    // setKnobCapturedIndicator(id, rIn, rOut, width, color) — the design's OWN
    // pointer geometry, as fractions of the disc's half extent. Knob::paint
    // sweeps THIS pointer along the value arc instead of the synthetic notch.
    //
    // Without it a knob that carries an imported disc plus the design's pointer
    // metadata still renders the generic white notch on the scripted path,
    // because only the native materializer forwarded the geometry — the same
    // captured art looked right when materialized and wrong when scripted.
    register_bridge_function(api, "setKnobCapturedIndicator",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto* k = dynamic_cast<Knob*>(self.widget(id));
            if (!k) return choc::value::Value();
            // No captured colour means Pulp must not invent one: fall back to
            // the same `knob.thumb` token the synthetic notch resolves, so a
            // theme reaches the pointer instead of a fixed near-white.
            auto color = k->resolve_color("knob.thumb", canvas::Color::rgba8(235, 235, 235));
            if (auto [c, ok] = parse_skin_color(args.get<std::string>(4, "")); ok)
                color = c;
            k->set_captured_indicator(
                static_cast<float>(args.get<double>(1, 0.0)),
                static_cast<float>(args.get<double>(2, 0.0)),
                static_cast<float>(args.get<double>(3, 0.0)),
                color);
            k->request_repaint();
            return choc::value::Value();
        });

    // setFaderCapturedArt(id, bodyPath, bodyW, bodyH,
    //                     indicatorPath, indicatorW, indicatorH, cross,
    //                     bodyOriginX, bodyOriginY, controlW, controlH)
    // Hoists the browser-authored thumb into a value-driven overlay while the
    // cleaned body crop covers the frozen instance in the capture.
    register_bridge_function(api, "setFaderCapturedArt",
        [&self](choc::javascript::ArgumentList args) {
            auto* fader = dynamic_cast<Fader*>(
                self.widget(args.get<std::string>(0, "")));
            if (!fader) return choc::value::Value();

            auto body_path = args.get<std::string>(1, "");
            auto indicator_path = args.get<std::string>(4, "");
            const int body_w = static_cast<int>(args.get<double>(2, 0));
            const int body_h = static_cast<int>(args.get<double>(3, 0));
            const int indicator_w = static_cast<int>(args.get<double>(5, 0));
            const int indicator_h = static_cast<int>(args.get<double>(6, 0));
            if (body_path.empty() || indicator_path.empty() ||
                body_w <= 0 || body_h <= 0 ||
                indicator_w <= 0 || indicator_h <= 0)
                return choc::value::Value();
            if (body_path.rfind("file://", 0) == 0) body_path = body_path.substr(7);
            if (indicator_path.rfind("file://", 0) == 0)
                indicator_path = indicator_path.substr(7);
            body_path = self.resolve_script_relative(body_path);
            indicator_path = self.resolve_script_relative(indicator_path);

            auto body = std::make_shared<SpriteStrip>();
            body->load_from_file(body_path, body_w, body_h, 1,
                                 SpriteStrip::Orientation::vertical);
            auto indicator = std::make_shared<SpriteStrip>();
            indicator->load_from_file(indicator_path, indicator_w, indicator_h, 1,
                                      SpriteStrip::Orientation::vertical);
            if (!body->loaded() || !indicator->loaded())
                return choc::value::Value();
            fader->set_captured_art(
                std::move(body), std::move(indicator),
                static_cast<float>(args.get<double>(7, 0.5)),
                static_cast<float>(args.get<double>(8, 0.0)),
                static_cast<float>(args.get<double>(9, 0.0)),
                static_cast<float>(args.get<double>(10, body_w)),
                static_cast<float>(args.get<double>(11, body_h)));
            return choc::value::Value();
        });

    // setFaderSkin(id, trackColor, fillColor, thumbColor, thumbBorderColor?,
    //              thumbW?, thumbH?, cornerRadius?)
    register_bridge_function(api, "setFaderSkin",
        [&self](choc::javascript::ArgumentList args) {
            auto* f = dynamic_cast<Fader*>(self.widget(args.get<std::string>(0, "")));
            if (!f) return choc::value::Value();
            if (auto [c, ok] = parse_skin_color(args.get<std::string>(1, "")); ok) f->set_skin_track_color(c);
            if (auto [c, ok] = parse_skin_color(args.get<std::string>(2, "")); ok) f->set_skin_fill_color(c);
            if (auto [c, ok] = parse_skin_color(args.get<std::string>(3, "")); ok) f->set_skin_thumb_color(c);
            if (auto [c, ok] = parse_skin_color(args.get<std::string>(4, "")); ok) f->set_skin_thumb_border_color(c);
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
            if (auto [c, ok] = parse_skin_color(args.get<std::string>(1, "")); ok) {
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
            if (auto [bg, ok] = parse_skin_color(args.get<std::string>(1, "")); ok)
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
                    if (auto [c, ok] = parse_skin_color(tok); ok) stops.push_back(c);
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

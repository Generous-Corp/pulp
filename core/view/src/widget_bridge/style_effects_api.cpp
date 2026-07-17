// widget_bridge/style_effects_api.cpp - CSS filter, clip-path, backdrop, and blend style registrations.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <cctype>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void WidgetBridge::register_widget_style_filter_clip_api() {
    BridgeApiContext api{engine_};

    // setFilter(id, "blur(4px) brightness(0.8) saturate(1.2) drop-shadow(...)")
    // Walks the function sequence and builds View::FilterOp entries; the
    // View paint path passes the chain to canvas.save_layer_with_filters,
    // which composes via SkImageFilters on the Skia backend (CG falls
    // through to blur-only for now).
    register_bridge_function(api, "setFilter", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto filter_str = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();

        if (filter_str == "none" || filter_str.empty()) {
            v->clear_filter_chain();
            v->set_filter_blur(0.0f);
            return choc::value::Value();
        }

        // Walk function-call sequence: `name(args)` repeated.
        std::vector<View::FilterOp> chain;
        size_t i = 0;
        while (i < filter_str.size()) {
            // Skip whitespace
            while (i < filter_str.size() && std::isspace(static_cast<unsigned char>(filter_str[i]))) ++i;
            if (i >= filter_str.size()) break;
            // Parse name up to '('
            size_t name_start = i;
            while (i < filter_str.size() && filter_str[i] != '(') ++i;
            if (i >= filter_str.size()) break;
            std::string name = filter_str.substr(name_start, i - name_start);
            // Trim trailing whitespace from name
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
            ++i; // skip '('
            // Parse args up to ')'
            size_t args_start = i;
            int depth = 1;
            while (i < filter_str.size() && depth > 0) {
                if (filter_str[i] == '(') ++depth;
                else if (filter_str[i] == ')') --depth;
                if (depth > 0) ++i;
            }
            std::string args_str = filter_str.substr(args_start, i - args_start);
            if (i < filter_str.size()) ++i; // skip ')'

            View::FilterOp op{};
            // Strip 'px' / '%' suffix and parse numeric.
            auto parse_amount = [](const std::string& s) -> float {
                std::string t = s;
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
                if (t.size() >= 2 && t.substr(t.size() - 2) == "px") t.erase(t.size() - 2);
                bool pct = false;
                if (!t.empty() && t.back() == '%') { pct = true; t.pop_back(); }
                try {
                    float v = std::stof(t);
                    return pct ? v / 100.0f : v;
                } catch (...) { return 0.0f; }
            };
            auto parse_angle_deg = [](const std::string& s) -> float {
                std::string t = s;
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
                float scale = 1.0f;
                // Check 4-char suffixes first so "grad" doesn't get
                // matched as "rad" with a stray 'g' prefix.
                if (t.size() >= 4 && t.substr(t.size() - 4) == "grad") { t.erase(t.size() - 4); scale = 0.9f; }
                else if (t.size() >= 4 && t.substr(t.size() - 4) == "turn") { t.erase(t.size() - 4); scale = 360.0f; }
                else if (t.size() >= 3 && t.substr(t.size() - 3) == "deg") { t.erase(t.size() - 3); }
                else if (t.size() >= 3 && t.substr(t.size() - 3) == "rad") { t.erase(t.size() - 3); scale = 180.0f / 3.14159265358979323846f; }
                try { return std::stof(t) * scale; } catch (...) { return 0.0f; }
            };
            if      (name == "blur")       { op.kind = View::FilterOp::Kind::blur;       op.amount = parse_amount(args_str); }
            else if (name == "brightness") { op.kind = View::FilterOp::Kind::brightness; op.amount = parse_amount(args_str); }
            else if (name == "contrast")   { op.kind = View::FilterOp::Kind::contrast;   op.amount = parse_amount(args_str); }
            else if (name == "grayscale")  { op.kind = View::FilterOp::Kind::grayscale;  op.amount = parse_amount(args_str); }
            else if (name == "invert")     { op.kind = View::FilterOp::Kind::invert;     op.amount = parse_amount(args_str); }
            else if (name == "opacity")    { op.kind = View::FilterOp::Kind::opacity;    op.amount = parse_amount(args_str); }
            else if (name == "saturate")   { op.kind = View::FilterOp::Kind::saturate;   op.amount = parse_amount(args_str); }
            else if (name == "sepia")      { op.kind = View::FilterOp::Kind::sepia;      op.amount = parse_amount(args_str); }
            else if (name == "hue-rotate") { op.kind = View::FilterOp::Kind::hue_rotate; op.angle_deg = parse_angle_deg(args_str); }
            else if (name == "drop-shadow") {
                // drop-shadow(<dx> <dy> <blur> <color>) - space-separated
                op.kind = View::FilterOp::Kind::drop_shadow;
                std::vector<std::string> tokens;
                std::string tok;
                int paren = 0;
                for (char c : args_str) {
                    if (c == '(') { ++paren; tok += c; continue; }
                    if (c == ')') { --paren; tok += c; continue; }
                    if (paren == 0 && std::isspace(static_cast<unsigned char>(c))) {
                        if (!tok.empty()) { tokens.push_back(tok); tok.clear(); }
                        continue;
                    }
                    tok += c;
                }
                if (!tok.empty()) tokens.push_back(tok);
                if (tokens.size() >= 3) {
                    op.ds_offset_x = parse_amount(tokens[0]);
                    op.ds_offset_y = parse_amount(tokens[1]);
                    op.ds_blur     = parse_amount(tokens[2]);
                    if (tokens.size() >= 4) {
                        // tokens[3..] is the color (may be space-separated rgb()).
                        std::string color_str = tokens[3];
                        for (size_t k = 4; k < tokens.size(); ++k) color_str += " " + tokens[k];
                        // Lean on the existing Color::from_string parser.
                        op.ds_color = parse_bridge_css_color(color_str);
                    } else {
                        op.ds_color = canvas::Color::rgba(0.0f, 0.0f, 0.0f, 1.0f);
                    }
                }
            }
            else { continue; } // unknown filter function - silently drop
            chain.push_back(op);
        }

        // Maintain the legacy filter_blur_ slot for backward compat
        // with paths that haven't migrated to the chain API yet.
        float total_blur = 0.0f;
        for (const auto& op : chain) {
            if (op.kind == View::FilterOp::Kind::blur) total_blur += op.amount;
        }
        v->set_filter_blur(total_blur);
        v->set_filter_chain(std::move(chain));
        return choc::value::Value();
    });

    // setBackdropFilter(id, blur_px) - CSS `backdrop-filter: blur(Npx)` for
    // frosted-glass overlays / modal backgrounds. Numeric overload keeps
    // the bridge cheap; string-form CSS parsing stays in
    // setFilter.
    register_bridge_function(api, "setBackdropFilter",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto blur_px = args.get<double>(1, 0.0);
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_backdrop_blur(static_cast<float>(blur_px));
            return choc::value::Value();
        });

    // setClipPath(id, value) - CSS `clip-path`. The paint side feeds the
    // stored slot to `Canvas::clip_path_svg` which calls
    // `SkPath::FromSVGString`; that parser only accepts raw SVG path "d"
    // data. Unwrap `path("...")` here and explicitly skip shapes / URL
    // refs the paint side cannot honor.
    register_bridge_function(api, "setClipPath",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (!v) return choc::value::Value();
            auto trim = [](std::string s) {
                auto a = s.find_first_not_of(" \t\n\r");
                if (a == std::string::npos) return std::string{};
                auto b = s.find_last_not_of(" \t\n\r");
                return s.substr(a, b - a + 1);
            };
            std::string t = trim(value);
            // CSS keywords (`none`, `path(...)`, `circle(...)`, etc.) are
            // case-insensitive per spec. Build a lowercased copy of the
            // prefix-bearing portion for comparison; preserve the original
            // case for the SVG path "d" data inside path("...").
            std::string t_lower = t;
            for (auto& c : t_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (t_lower.empty() || t_lower == "none") {
                v->set_clip_path("");
            } else if (t_lower.size() > 6 && t_lower.rfind("path(", 0) == 0 && t.back() == ')') {
                std::string inner = trim(t.substr(5, t.size() - 6));
                if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'')
                    && inner.back() == inner.front()) {
                    inner = inner.substr(1, inner.size() - 2);
                }
                v->set_clip_path(inner);
            } else {
                bool deferred_shape = false;
                for (const char* p : {"circle(", "ellipse(", "inset(", "polygon(",
                                       "rect(", "xywh(", "url("}) {
                    if (t_lower.rfind(p, 0) == 0) { deferred_shape = true; break; }
                }
                if (deferred_shape) {
                    v->set_clip_path("");
                } else {
                    v->set_clip_path(t);
                }
            }
            return choc::value::Value();
        });
}

void WidgetBridge::register_widget_style_blend_api() {
    BridgeApiContext api{engine_};

    // setMixBlendMode(id, "multiply") for CSS / RN `mix-blend-mode`.
    // Maps the W3C blend-mode keyword set to the canvas BlendMode enum
    // so the View paint path can pass it straight into
    // `save_layer_with_blend()` at compositing time.
    // The keyword set mirrors the W3C separable + non-separable blend
    // modes (the same 16 values RN's New Architecture surface accepts;
    // see tools/harness/oracles/rn/rn-viewstyle.json::mixBlendMode).
    // Unknown keywords (including the empty string and "normal") leave
    // the View at default `BlendMode::normal` so the fast path stays
    // a paint-time no-op.
    register_bridge_function(api, "setMixBlendMode",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "normal");
            auto* v = id.empty() ? &root_ : widget(id);
            if (!v) return choc::value::Value();
            using BM = pulp::canvas::Canvas::BlendMode;
            BM mode = BM::normal;
            if      (kw == "normal")      mode = BM::normal;
            else if (kw == "multiply")    mode = BM::multiply;
            else if (kw == "screen")      mode = BM::screen;
            else if (kw == "overlay")     mode = BM::overlay;
            else if (kw == "darken")      mode = BM::darken;
            else if (kw == "lighten")     mode = BM::lighten;
            else if (kw == "color-dodge") mode = BM::color_dodge;
            else if (kw == "color-burn")  mode = BM::color_burn;
            else if (kw == "hard-light")  mode = BM::hard_light;
            else if (kw == "soft-light")  mode = BM::soft_light;
            else if (kw == "difference")  mode = BM::difference;
            else if (kw == "exclusion")   mode = BM::exclusion;
            else if (kw == "hue")         mode = BM::hue;
            else if (kw == "saturation")  mode = BM::saturation;
            else if (kw == "color")       mode = BM::color;
            else if (kw == "luminosity")  mode = BM::luminosity;
            // `plus-lighter` and `plus-darker` are CSS Compositing &
            // Blending Level 2 keywords. Both map to
            // `SkBlendMode::kPlus` (additive) at the Skia layer (see
            // canvas.hpp::BlendMode::lighter, index 26). `plus-darker` is
            // technically a multiplicative variant in the W3C draft but
            // Skia / Chromium ship the additive `kPlus` for both;
            // mirroring that is the closest we can do without a custom
            // SkBlender. Keeps consumers (Figma export, compositing demos)
            // from silently falling back to `normal`.
            else if (kw == "plus-lighter" || kw == "plus-darker")
                                          mode = BM::lighter;
            // Unknown keyword -> normal (paint-time no-op fallback).
            v->set_mix_blend_mode(mode);
            return choc::value::Value();
        });
}

} // namespace pulp::view

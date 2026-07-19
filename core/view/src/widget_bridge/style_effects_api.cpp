// widget_bridge/style_effects_api.cpp - CSS filter, clip-path, backdrop, and blend style registrations.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/canvas/view_effect.hpp>
#include <pulp/runtime/log.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {
namespace {

// Forward declarations (defined below, used by build_view_effect's sksl case).
canvas::Canvas::BlendMode parse_blend_keyword(const std::string& kw);
void parse_named_uniforms(const choc::value::ValueView& obj,
                          std::vector<canvas::Canvas::NamedUniform>& out);

// Numeric member with a fallback (non-numeric or missing → fallback).
float effect_num(const choc::value::ValueView& obj, const char* key, float fallback) {
    if (obj.isObject() && obj.hasObjectMember(key))
        return static_cast<float>(obj[key].getWithDefault<double>(fallback));
    return fallback;
}

// CSS-color member with a fallback.
canvas::Color effect_color(const choc::value::ValueView& obj, const char* key,
                           canvas::Color fallback) {
    if (obj.isObject() && obj.hasObjectMember(key)) {
        std::string s(obj[key].getWithDefault<std::string_view>(""));
        if (!s.empty()) return parse_bridge_css_color(s);
    }
    return fallback;
}

// Build one ViewEffect from a `{type, ...params}` spec. Returns nullptr and
// sets `error` for an unknown type or an sksl shader that fails to compile.
std::shared_ptr<canvas::ViewEffect> build_view_effect(
        const choc::value::ValueView& spec, std::string& error) {
    if (!spec.isObject()) { error = "effect spec must be an object"; return nullptr; }
    std::string type(spec.hasObjectMember("type")
                         ? spec["type"].getWithDefault<std::string_view>("") : "");
    if (type == "blur") {
        auto e = std::make_shared<canvas::GpuBlurEffect>();
        e->radius_x = effect_num(spec, "radiusX", effect_num(spec, "radius", e->radius_x));
        e->radius_y = effect_num(spec, "radiusY", effect_num(spec, "radius", e->radius_y));
        return e;
    }
    if (type == "bloom") {
        auto e = std::make_shared<canvas::GpuBloomEffect>();
        e->intensity = effect_num(spec, "intensity", e->intensity);
        e->threshold = effect_num(spec, "threshold", e->threshold);
        e->radius = effect_num(spec, "radius", e->radius);
        return e;
    }
    if (type == "vignette") {
        auto e = std::make_shared<canvas::VignetteEffect>();
        e->intensity = effect_num(spec, "intensity", e->intensity);
        e->radius = effect_num(spec, "radius", e->radius);
        e->edge_color = effect_color(spec, "color", e->edge_color);
        return e;
    }
    if (type == "chromatic") {
        auto e = std::make_shared<canvas::ChromaticAberrationEffect>();
        e->offset = effect_num(spec, "offset", e->offset);
        return e;
    }
    if (type == "sksl") {
        std::string src(spec.hasObjectMember("source")
                            ? spec["source"].getWithDefault<std::string_view>("") : "");
        if (src.empty()) { error = "sksl effect requires a non-empty 'source'"; return nullptr; }
        // Validate at install time like setWidgetShader — reject with the
        // compile error rather than installing a shader that paints nothing.
        auto compile_error = canvas::Canvas::compile_sksl(src);
        if (!compile_error.empty()) { error = compile_error; return nullptr; }
        auto e = std::make_shared<canvas::SkslPostEffect>();
        e->sksl = std::move(src);
        e->value = effect_num(spec, "value", 0.0f);
        e->time = effect_num(spec, "time", 0.0f);
        e->sample_radius = effect_num(spec, "sampleRadius", 0.0f);
        if (spec.hasObjectMember("blend")) {
            e->blend_mode = parse_blend_keyword(
                std::string(spec["blend"].getWithDefault<std::string_view>("")));
        }
        if (spec.hasObjectMember("uniforms"))
            parse_named_uniforms(spec["uniforms"], e->uniforms);
        return e;
    }
    error = "unknown effect type '" + type + "'";
    return nullptr;
}

// CSS / W3C blend keyword → canvas BlendMode (subset relevant to post-effects;
// `lighter`/`additive`/`plus-*` all map to the additive kPlus glow).
canvas::Canvas::BlendMode parse_blend_keyword(const std::string& kw) {
    using BM = canvas::Canvas::BlendMode;
    if (kw == "multiply")    return BM::multiply;
    if (kw == "screen")      return BM::screen;
    if (kw == "overlay")     return BM::overlay;
    if (kw == "darken")      return BM::darken;
    if (kw == "lighten")     return BM::lighten;
    if (kw == "color-dodge") return BM::color_dodge;
    if (kw == "color-burn")  return BM::color_burn;
    if (kw == "hard-light")  return BM::hard_light;
    if (kw == "soft-light")  return BM::soft_light;
    if (kw == "difference")  return BM::difference;
    if (kw == "exclusion")   return BM::exclusion;
    if (kw == "lighter" || kw == "additive" ||
        kw == "plus-lighter" || kw == "plus-darker") return BM::lighter;
    return BM::normal;
}

// Parse an `{name: number | [x,y,z,w]}` object into arbitrary shader uniforms.
void parse_named_uniforms(const choc::value::ValueView& obj,
                          std::vector<canvas::Canvas::NamedUniform>& out) {
    if (!obj.isObject()) return;
    for (uint32_t i = 0; i < obj.size(); ++i) {
        canvas::Canvas::NamedUniform nu;
        nu.name = std::string(obj.getObjectMemberAt(i).name);
        const auto& val = obj[nu.name.c_str()];
        if (val.isArray()) {
            nu.count = std::min<int>(4, static_cast<int>(val.size()));
            for (int k = 0; k < nu.count; ++k)
                nu.v[k] = static_cast<float>(val[static_cast<uint32_t>(k)]
                                                 .getWithDefault<double>(0.0));
        } else {
            nu.count = 1;
            nu.v[0] = static_cast<float>(val.getWithDefault<double>(0.0));
        }
        out.push_back(std::move(nu));
    }
}

choc::value::Value effect_result(bool success, const std::string& error) {
    auto result = choc::value::createObject("");
    result.addMember("success", choc::value::createBool(success));
    result.addMember("error", choc::value::createString(error));
    return result;
}

} // namespace

void BridgeRegistrars::register_widget_style_filter_clip_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setFilter(id, "blur(4px) brightness(0.8) saturate(1.2) drop-shadow(...)")
    // Walks the function sequence and builds View::FilterOp entries; the
    // View paint path passes the chain to canvas.save_layer_with_filters,
    // which composes via SkImageFilters on the Skia backend (CG falls
    // through to blur-only for now).
    register_bridge_function(api, "setFilter", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto filter_str = args.get<std::string>(1, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
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
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto blur_px = args.get<double>(1, 0.0);
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_backdrop_blur(static_cast<float>(blur_px));
            return choc::value::Value();
        });

    // setClipPath(id, value) - CSS `clip-path` -> {applied, warning}. The paint
    // side feeds the stored slot to `Canvas::clip_path_svg` which calls
    // `SkPath::FromSVGString`; that parser only accepts raw SVG path "d" data.
    // Unwrap `path("...")` here. The CSS basic-shape / url() forms
    // (circle/ellipse/inset/polygon/rect/xywh/url) cannot be honored by the
    // paint side, so instead of clearing the clip SILENTLY (which reads like a
    // rendering bug) we name the unsupported shape in the returned `warning` and
    // a host log line — the same "surface the reason" contract setWidgetShader
    // uses for compile errors.
    register_bridge_function(api, "setClipPath",
        [&self](choc::javascript::ArgumentList args) {
            auto clip_result = [](bool applied, const std::string& warning) {
                auto r = choc::value::createObject("");
                r.addMember("applied", choc::value::createBool(applied));
                r.addMember("warning", choc::value::createString(warning));
                return r;
            };
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (!v) return clip_result(false, "No widget with id '" + id + "'");
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
                return clip_result(true, "");
            }
            if (t_lower.size() > 6 && t_lower.rfind("path(", 0) == 0 && t.back() == ')') {
                std::string inner = trim(t.substr(5, t.size() - 6));
                if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'')
                    && inner.back() == inner.front()) {
                    inner = inner.substr(1, inner.size() - 2);
                }
                v->set_clip_path(inner);
                return clip_result(true, "");
            }
            for (const char* p : {"circle(", "ellipse(", "inset(", "polygon(",
                                   "rect(", "xywh(", "url("}) {
                if (t_lower.rfind(p, 0) == 0) {
                    v->set_clip_path("");  // cannot honor — leave unclipped
                    std::string warning =
                        "clip-path '" + t + "' is not supported: Pulp can only "
                        "clip to a path(\"<svg-d>\") (basic-shape and url() "
                        "clip-path forms need primitives Pulp does not implement). "
                        "No clip applied.";
                    runtime::log_warn("[clip-path] {}", warning);
                    return clip_result(false, warning);
                }
            }
            // A bare SVG path "d" string (no wrapper) — install as-is.
            v->set_clip_path(t);
            return clip_result(true, "");
        });

    // setViewEffect(id, specJson) -> {success, error}
    //
    // Attach a per-view GPU post-effect — the ViewEffect system that
    // View::paint_all already consumes, but which was previously unreachable
    // from JS (no bridge entry, so app authors could not use bloom / vignette /
    // chromatic aberration / a custom SkSL post-effect at all).
    //
    // `specJson` is either a single object
    //   {type:'blur'|'bloom'|'vignette'|'chromatic'|'sksl', ...params}
    // or an array of them (composed into an EffectChain, one layer per effect).
    // JSON `null` / "" / "none" clears the effect. An `sksl` effect is compiled
    // at install time and rejected with its error (mirrors setWidgetShader)
    // rather than installing a shader that would paint nothing.
    register_bridge_function(api, "setViewEffect",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto spec_json = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (!v) return effect_result(false, "No widget with id '" + id + "'");

            if (spec_json.empty() || spec_json == "none" || spec_json == "null") {
                v->set_effect(nullptr);
                self.request_repaint();
                return effect_result(true, "");
            }

            choc::value::Value parsed;
            try {
                parsed = choc::json::parse(spec_json);
            } catch (...) {
                return effect_result(false, "invalid effect spec JSON");
            }
            if (parsed.isVoid()) {  // JSON null parses to a void value
                v->set_effect(nullptr);
                self.request_repaint();
                return effect_result(true, "");
            }

            std::string error;
            std::shared_ptr<canvas::ViewEffect> effect;
            if (parsed.isArray()) {
                auto chain = std::make_shared<canvas::EffectChain>();
                for (uint32_t i = 0; i < parsed.size(); ++i) {
                    auto child = build_view_effect(parsed[i], error);
                    if (!child) return effect_result(false, error);
                    chain->add(std::move(child));
                }
                effect = std::move(chain);
            } else {
                effect = build_view_effect(parsed, error);
                if (!effect) return effect_result(false, error);
            }

            v->set_effect(std::move(effect));
            self.request_repaint();
            return effect_result(true, "");
        });
}

void BridgeRegistrars::register_widget_style_blend_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

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
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "normal");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
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

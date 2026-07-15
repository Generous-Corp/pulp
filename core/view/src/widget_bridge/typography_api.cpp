// widget_bridge/typography_api.cpp - typography registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <pulp/view/text_editor.hpp>

#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

void WidgetBridge::register_widget_typography_api() {
    BridgeApiContext api{engine_};

    // setFontFamily(id, family) wires web-compat font-family declarations
    // through to Label::set_font_family(); Label::paint() honors it via
    // canvas.set_font_full().
    register_bridge_function(api, "setFontFamily", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto family = args.get<std::string>(1, "");
        if (!v) return choc::value::Value();
        // Pass the comma-separated CSS family list verbatim so the SkiaCanvas typeface resolver
        // (skia_canvas.cpp:get_cached_typeface) can walk the fallback
        // chain through registered, bundled, and SkFontMgr sources.
        //
        // Storage layer accepts the raw comma-list; the resolver
        // splits it at paint time. Single-family inputs (no comma)
        // still hit the get_cached_typeface_single fast path with
        // identical single-family behavior.
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_font_family(family);
        } else {
            // Mirrors the setFontWeight / setLetterSpacing pattern so a container
            // View's font-family flows down to descendant Labels.
            v->set_inheritable_font_family(family);
        }
        return choc::value::Value();
    });

    // Typography setters cascade. Label gets its own value;
    // any other View (Panel, Box, container) stores the value on the
    // View's inheritable_* slot so descendant Labels pick it up. Do not
    // silently no-op on container Views; that was the dom-adapter
    // workaround this replaces.
    register_bridge_function(api, "setFontWeight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        int w = static_cast<int>(args.get<double>(1, 400));
        if (!v) return choc::value::Value();
        if (auto* l = dynamic_cast<Label*>(v)) l->set_font_weight(w);
        else v->set_inheritable_font_weight(w);
        return choc::value::Value();
    });

    register_bridge_function(api, "setFontStyle", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto s = args.get<std::string>(1, "normal");
        if (auto* l = dynamic_cast<Label*>(v)) l->set_font_style(s == "italic" ? 1 : 0);
        return choc::value::Value();
    });

    register_bridge_function(api, "setLetterSpacing", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto sp = static_cast<float>(args.get<double>(1, 0));
        if (!v) return choc::value::Value();
        if (auto* l = dynamic_cast<Label*>(v)) l->set_letter_spacing(sp);
        else v->set_inheritable_letter_spacing(sp);
        return choc::value::Value();
    });

    register_bridge_function(api, "setLineHeight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto lh = static_cast<float>(args.get<double>(1, 0));
        if (auto* l = dynamic_cast<Label*>(v)) l->set_line_height(lh);
        return choc::value::Value();
    });

    register_bridge_function(api, "setTextAlign", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto a = args.get<std::string>(1, "left");
        if (!v) return choc::value::Value();
        // Accept all six CSS / RN textAlign values:
        //   "left"/"start"     -> LabelAlign::left         (0)
        //   "center"           -> LabelAlign::center       (1)
        //   "right"/"end"      -> LabelAlign::right        (2)
        //   "auto"             -> LabelAlign::auto_        (3)
        //   "justify"          -> LabelAlign::justify      (4)
        //   "match-parent"     -> LabelAlign::match_parent (5)
        int aligned;
        LabelAlign label_a;
        if      (a == "center")               { aligned = 1; label_a = LabelAlign::center; }
        else if (a == "right" || a == "end")  { aligned = 2; label_a = LabelAlign::right; }
        else if (a == "auto")                 { aligned = 3; label_a = LabelAlign::auto_; }
        else if (a == "justify")              { aligned = 4; label_a = LabelAlign::justify; }
        else if (a == "match-parent")         { aligned = 5; label_a = LabelAlign::match_parent; }
        else                                  { aligned = 0; label_a = LabelAlign::left; }
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_text_align(label_a);
        } else {
            // Container Views store the alignment in the inheritable slot for
            // descendant Labels. Encoding extends
            // 0..5 to cover auto / justify / match-parent.
            v->set_inheritable_text_align(aligned);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "setMultiLine", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto ml = args.get<double>(1, 0) > 0.5;
        if (auto* l = dynamic_cast<Label*>(v)) l->set_multi_line(ml);
        else if (auto* e = dynamic_cast<TextEditor*>(v)) e->multi_line = ml;
        return choc::value::Value();
    });

    register_bridge_function(api, "setFontSize", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto size = static_cast<float>(args.get<double>(1, 14));
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_font_size(size);
        } else if (auto* e = dynamic_cast<TextEditor*>(v)) {
            e->set_font_size(size);
        } else {
            // Container Views store the size for descendant Labels via the
            // inheritable slot.
            v->set_inheritable_font_size(size);
        }
        return choc::value::Value();
    });
}

void WidgetBridge::register_widget_typography_color_api(std::function<canvas::Color(const std::string&)> parse_color) {
    BridgeApiContext api{engine_};
    auto parseHexColor = std::move(parse_color);

    register_bridge_function(api, "setTextColor", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v || hex.empty()) return choc::value::Value();
        auto color = parseHexColor(hex);
        // CSS-style cascade.
        // - On a Label: set the Label's own explicit text_color, which
        //   wins over inheritance and theme tokens.
        // - On a container View: store the color on the inheritable
        //   slot so descendant Labels pick it up. This replaces the
        //   dom-adapter's manual "walk children and pushdown" hack.
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_text_color(color);
        } else {
            v->set_inheritable_text_color(color);
        }
        // Keep the theme-token fallback in sync so widgets that resolve
        // through resolve_color("text.primary") (e.g. Knob/ToggleButton)
        // also pick up the override on their own subtree; preserves the
        // legacy behavior for those widgets.
        auto theme = v->theme();
        theme.colors["text.primary"] = color;
        v->set_theme(theme);
        return choc::value::Value();
    });
}

void WidgetBridge::register_widget_typography_decoration_api(std::function<canvas::Color(const std::string&)> parse_color) {
    BridgeApiContext api{engine_};
    auto parseHexColor = std::move(parse_color);

    // setTextTransform(id, "uppercase"/"lowercase"/"capitalize"/"none") - CSS text-transform.
    register_bridge_function(api, "setTextTransform", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto t = args.get<std::string>(1, "none");
        if (auto* l = dynamic_cast<Label*>(v)) {
            if (t == "uppercase") l->set_text_transform(Label::TextTransform::uppercase);
            else if (t == "lowercase") l->set_text_transform(Label::TextTransform::lowercase);
            else if (t == "capitalize") l->set_text_transform(Label::TextTransform::capitalize);
            else l->set_text_transform(Label::TextTransform::none);
        }
        return choc::value::Value();
    });

    // setTextDecoration(id, "underline"/"line-through"/"overline"/"none") - CSS text-decoration.
    register_bridge_function(api, "setTextDecoration", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto d = args.get<std::string>(1, "none");
        if (auto* l = dynamic_cast<Label*>(v)) {
            if (d == "underline") l->set_text_decoration(Label::TextDecoration::underline);
            else if (d == "line-through") l->set_text_decoration(Label::TextDecoration::line_through);
            else if (d == "overline") l->set_text_decoration(Label::TextDecoration::overline);
            else l->set_text_decoration(Label::TextDecoration::none);
        }
        return choc::value::Value();
    });

    // Text-decoration longhands. CSS shorthand text-decoration routes through setTextDecoration
    // above (line keyword only). The longhand triplet
    // text-decoration-line / -color / -style reaches each setter
    // independently so authors can build the decoration up piece-by-piece
    // without losing previously-set siblings.

    // setTextDecorationColor(id, "#rrggbb"|color-token)
    register_bridge_function(api, "setTextDecorationColor",
        [this, parseHexColor](choc::javascript::ArgumentList args) {
            auto* v = widget(args.get<std::string>(0, ""));
            auto hex = args.get<std::string>(1, "");
            if (auto* l = dynamic_cast<Label*>(v); l && !hex.empty()) {
                l->set_text_decoration_color(parseHexColor(hex));
            }
            return choc::value::Value();
        });

    // setTextDecorationStyle(id, "solid"|"double"|"dotted"|"dashed"|"wavy")
    // The paint path renders solid regardless today, but the value is
    // stored on the Label so future paint logic can honor it without an
    // API break.
    register_bridge_function(api, "setTextDecorationStyle",
        [this](choc::javascript::ArgumentList args) {
            auto* v = widget(args.get<std::string>(0, ""));
            auto s = args.get<std::string>(1, "solid");
            if (auto* l = dynamic_cast<Label*>(v)) {
                if (s == "double") l->set_text_decoration_style(Label::TextDecorationStyle::double_);
                else if (s == "dotted") l->set_text_decoration_style(Label::TextDecorationStyle::dotted);
                else if (s == "dashed") l->set_text_decoration_style(Label::TextDecorationStyle::dashed);
                else if (s == "wavy") l->set_text_decoration_style(Label::TextDecorationStyle::wavy);
                else l->set_text_decoration_style(Label::TextDecorationStyle::solid);
            }
            return choc::value::Value();
        });

    // CSS line-clamp and -webkit-line-clamp route through the same shared
    // case in web-compat-style-decl.js. Numeric only; 0 disables clamping.
    register_bridge_function(api, "setLineClamp", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        int n = static_cast<int>(args.get<double>(1, 0.0));
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_line_clamp(n);
            // line-clamp implies multi-line; without multi_line_, the
            // paint path takes the single-line branch and the clamp is a no-op.
            if (n > 0) l->set_multi_line(true);
        }
        return choc::value::Value();
    });
}

void WidgetBridge::register_widget_typography_overflow_api() {
    BridgeApiContext api{engine_};

    // setTextOverflow(id, "ellipsis"|"clip") - CSS text-overflow.
    register_bridge_function(api, "setTextOverflow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "clip");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_text_overflow_ellipsis(mode == "ellipsis");
        return choc::value::Value();
    });
}

void WidgetBridge::register_widget_typography_extended_api() {
    BridgeApiContext api{engine_};

    // setTextIndent(id, px) - CSS text-indent. Storage-only today;
    // SkParagraph::setTextIndent integration is not wired yet.
    register_bridge_function(api, "setTextIndent",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto v = static_cast<float>(args.get<double>(1, 0.0));
            auto* w = id.empty() ? &root_ : widget(id);
            if (w) w->set_text_indent(v);
            return choc::value::Value();
        });

    // setVerticalAlign(id, "top"|"middle"|"bottom"|"baseline"|...)
    // CSS vertical-align. Maps the keyword to the existing
    // canvas::TextVerticalAlign enum on Label; non-Label widgets no-op.
    register_bridge_function(api, "setVerticalAlign",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "baseline");
            using VA = pulp::canvas::TextVerticalAlign;
            VA mode = VA::baseline;
            if      (kw == "top")      mode = VA::top;
            else if (kw == "middle")   mode = VA::center;
            else if (kw == "center")   mode = VA::center;
            else if (kw == "bottom")   mode = VA::bottom;
            else if (kw == "baseline") mode = VA::baseline;
            else if (kw == "auto")     mode = VA::baseline;
            if (auto* l = dynamic_cast<Label*>(widget(id))) {
                l->set_vertical_align(mode);
            }
            return choc::value::Value();
        });

    // setWordBreak(id, kw) - CSS word-break / overflow-wrap.
    // Storage-only today; HarfBuzz line-break feature is deferred.
    register_bridge_function(api, "setWordBreak",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "normal");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_word_break(kw);
            return choc::value::Value();
        });

    // setFontVariant(id, kw) - CSS / RN font-variant. Storage-only;
    // HarfBuzz feature wiring is deferred.
    register_bridge_function(api, "setFontVariant",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "normal");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_font_variant(kw);
            return choc::value::Value();
        });

    // RN textShadow* per-attribute setters. Storage-only; SkPaint shadow
    // integration is not wired yet.
    register_bridge_function(api, "setTextShadowColor",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto c  = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_text_shadow_color(c);
            return choc::value::Value();
        });
    register_bridge_function(api, "setTextShadowOffset",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto dx = static_cast<float>(args.get<double>(1, 0.0));
            auto dy = static_cast<float>(args.get<double>(2, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_text_shadow_offset(dx, dy);
            return choc::value::Value();
        });
    register_bridge_function(api, "setTextShadowRadius",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto r  = static_cast<float>(args.get<double>(1, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_text_shadow_radius(r);
            return choc::value::Value();
        });
}

void WidgetBridge::register_widget_typography_shadow_shorthand_api() {
    BridgeApiContext api{engine_};

    // CSS-shorthand setTextShadow(id, dx, dy, blur, color).
    // Composes the three existing per-attribute text-shadow slots.
    register_bridge_function(api, "setTextShadow",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto dx = static_cast<float>(args.get<double>(1, 0.0));
            auto dy = static_cast<float>(args.get<double>(2, 0.0));
            auto r  = static_cast<float>(args.get<double>(3, 0.0));
            auto c  = args.get<std::string>(4, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) {
                v->set_text_shadow_offset(dx, dy);
                v->set_text_shadow_radius(r);
                v->set_text_shadow_color(c);
            }
            return choc::value::Value();
        });
}

} // namespace pulp::view

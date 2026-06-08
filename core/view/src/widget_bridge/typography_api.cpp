// widget_bridge/typography_api.cpp - typography registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>

#include <pulp/view/text_editor.hpp>

#include <string>

namespace pulp::view {

void WidgetBridge::register_widget_typography_api() {
    // pulp #927: setFontFamily(id, family) was called from web-compat JS
    // but had no C++ binding, so the call became a silent no-op. Now wires
    // through to Label::set_font_family() and Label::paint() honors it via
    // canvas.set_font_full().
    engine_.register_function("setFontFamily", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto family = args.get<std::string>(1, "");
        if (!v) return choc::value::Value();
        // pulp #1737 (#932 followup): pass the comma-separated CSS
        // family list verbatim so the SkiaCanvas typeface resolver
        // (skia_canvas.cpp:get_cached_typeface) can walk the fallback
        // chain. Pre-fix the bridge stripped to the first family,
        // dropping the rest. Now the resolver tries each family in
        // order through registered, bundled, and SkFontMgr sources.
        //
        // Storage layer accepts the raw comma-list; the resolver
        // splits it at paint time. Single-family inputs (no comma)
        // still hit the get_cached_typeface_single fast path with
        // identical behaviour to pre-fix.
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_font_family(family);
        } else {
            // pulp #1434 Phase A2-5: inheritable cascade. Mirrors the
            // setFontWeight / setLetterSpacing pattern so a container
            // View's font-family flows down to descendant Labels.
            v->set_inheritable_font_family(family);
        }
        return choc::value::Value();
    });

    // issue-969: typography setters cascade. Label gets its own value;
    // any other View (Panel, Box, container) stores the value on the
    // View's inheritable_* slot so descendant Labels pick it up. Do not
    // silently no-op on container Views; that was the dom-adapter
    // workaround this replaces.
    engine_.register_function("setFontWeight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        int w = static_cast<int>(args.get<double>(1, 400));
        if (!v) return choc::value::Value();
        if (auto* l = dynamic_cast<Label*>(v)) l->set_font_weight(w);
        else v->set_inheritable_font_weight(w);
        return choc::value::Value();
    });

    engine_.register_function("setFontStyle", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto s = args.get<std::string>(1, "normal");
        if (auto* l = dynamic_cast<Label*>(v)) l->set_font_style(s == "italic" ? 1 : 0);
        return choc::value::Value();
    });

    engine_.register_function("setLetterSpacing", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto sp = static_cast<float>(args.get<double>(1, 0));
        if (!v) return choc::value::Value();
        if (auto* l = dynamic_cast<Label*>(v)) l->set_letter_spacing(sp);
        else v->set_inheritable_letter_spacing(sp);
        return choc::value::Value();
    });

    engine_.register_function("setLineHeight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto lh = static_cast<float>(args.get<double>(1, 0));
        if (auto* l = dynamic_cast<Label*>(v)) l->set_line_height(lh);
        return choc::value::Value();
    });

    engine_.register_function("setTextAlign", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto a = args.get<std::string>(1, "left");
        if (!v) return choc::value::Value();
        // pulp #1434: accept all six CSS / RN textAlign values:
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
            // issue-969: container Views store the alignment in the
            // inheritable slot for descendant Labels. Encoding extends
            // 0..5 to cover auto / justify / match-parent.
            v->set_inheritable_text_align(aligned);
        }
        return choc::value::Value();
    });

    engine_.register_function("setMultiLine", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto ml = args.get<double>(1, 0) > 0.5;
        if (auto* l = dynamic_cast<Label*>(v)) l->set_multi_line(ml);
        else if (auto* e = dynamic_cast<TextEditor*>(v)) e->multi_line = ml;
        return choc::value::Value();
    });

    engine_.register_function("setFontSize", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto size = static_cast<float>(args.get<double>(1, 14));
        auto* v = widget(id);
        if (!v) return choc::value::Value();
        if (auto* l = dynamic_cast<Label*>(v)) {
            l->set_font_size(size);
        } else if (auto* e = dynamic_cast<TextEditor*>(v)) {
            e->set_font_size(size);
        } else {
            // issue-969: container Views store the size for descendant
            // Labels via the inheritable slot.
            v->set_inheritable_font_size(size);
        }
        return choc::value::Value();
    });
}

} // namespace pulp::view

// widget_bridge/outline_api.cpp - outline style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <cctype>
#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

void BridgeRegistrars::register_widget_outline_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // CSS / RN outline cluster. Outline is paint-time only: it does NOT
    // take up Yoga layout space (parent never reserves room
    // for it). Each setter mutates one slot in isolation so a JSX prop
    // diff that touches only `outlineColor` doesn't clobber `outlineWidth`.
    // Skia paint inflates the box by (offset + width/2) and strokes with
    // the standard borderStyle dash effect for dashed/dotted; other named
    // styles (double/groove/ridge/inset/outset) currently degrade to solid
    // - same paint-side gap as borderStyle.
    //
    // setOutlineColor(id, hex) - accepts the same color forms as
    // setBackground / setBorderColor (#hex / rgb() / named), plus the
    // CSS `currentColor` keyword which resolves to the View's text
    // color via the inheritable cascade.
    register_bridge_function(api, "setOutlineColor", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v || hex.empty()) return choc::value::Value();
        // CSS `currentColor`: resolve to the element's own computed text
        // color first, then the inheritable cascade, then theme fallback.
        // Order matters: a Label that set its own `color` via setTextColor
        // stores it in `Label::text_color_` (`has_own_text_color_=true`) and
        // does NOT touch the inheritable slot, so `inheritable_text_color()`
        // would skip the Label and climb to the parent's inheritable color.
        // CSS semantics: an element's own `color` always wins over
        // inheritance for that element's `currentColor`.
        std::string lower;
        lower.reserve(hex.size());
        for (char c : hex) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "currentcolor") {
            if (auto* l = dynamic_cast<Label*>(v); l && l->has_own_text_color()) {
                v->set_outline_color(l->text_color());
            } else if (auto inherited = v->inheritable_text_color()) {
                v->set_outline_color(*inherited);
            } else {
                v->set_outline_color(v->resolve_color("text.primary",
                                                      canvas::Color::rgba8(220, 220, 220)));
            }
        } else {
            v->set_outline_color(parse_bridge_css_color(hex));
        }
        return choc::value::Value();
    });

    // setOutlineOffset(id, px) - gap between border-box edge and outline.
    register_bridge_function(api, "setOutlineOffset", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto offset = static_cast<float>(args.get<double>(1, 0.0));
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) v->set_outline_offset(offset);
        return choc::value::Value();
    });

    // setOutlineStyle(id, "solid"|"dashed"|...) - same keyword set as
    // setBorderStyle. Reuses View::BorderStyle since the CSS spec lists
    // the same line-style values for both properties.
    register_bridge_function(api, "setOutlineStyle", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = args.get<std::string>(1, "solid");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (s == "dashed")        v->set_outline_style(View::BorderStyle::dashed);
        else if (s == "dotted")   v->set_outline_style(View::BorderStyle::dotted);
        else if (s == "double")   v->set_outline_style(View::BorderStyle::double_);
        else if (s == "groove")   v->set_outline_style(View::BorderStyle::groove);
        else if (s == "ridge")    v->set_outline_style(View::BorderStyle::ridge);
        else if (s == "inset")    v->set_outline_style(View::BorderStyle::inset);
        else if (s == "outset")   v->set_outline_style(View::BorderStyle::outset);
        else if (s == "none")     v->set_outline_style(View::BorderStyle::none);
        else if (s == "hidden")   v->set_outline_style(View::BorderStyle::hidden);
        else                      v->set_outline_style(View::BorderStyle::solid);
        return choc::value::Value();
    });

    // setOutlineWidth(id, px) - outline stroke thickness. width<=0 or
    // outline_style==none/hidden short-circuits the paint.
    register_bridge_function(api, "setOutlineWidth", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto width = static_cast<float>(args.get<double>(1, 0.0));
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) v->set_outline_width(width);
        return choc::value::Value();
    });
}

} // namespace pulp::view

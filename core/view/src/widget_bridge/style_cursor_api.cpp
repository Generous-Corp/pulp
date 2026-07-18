// widget_bridge/style_cursor_api.cpp - cursor and direction style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <string>

namespace pulp::view {

void BridgeRegistrars::register_widget_style_cursor_direction_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setCursor(id, "pointer"|"crosshair"|"text"|"default") - CSS cursor
    register_bridge_function(api, "setCursor", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto c = args.get<std::string>(1, "default");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        // Map the CSS cursor keyword set to the View::CursorStyle slots that exist
        // today (4 base + 7 resize + invisible + multi-directional = 12
        // distinct visuals). Where multiple CSS keywords map to the
        // same visual (n/s/e/w-resize all map to the axis-aligned
        // resize cursor), we collapse to the closest existing slot.
        //
        // Dedicated slots for `alias` / `copy` / `zoom-in` / `zoom-out` /
        // `context-menu` use real NSCursor backings on
        // macOS - see core/view/platform/mac/window_host_mac.mm).
        // `wait` / `help` / `progress` / `cell` stay routed to default
        // because macOS has no native cursor for them - listed in
        // compat.json unsupportedValues for honesty.
        using CS = View::CursorStyle;
        if (c == "pointer")              v->set_cursor(CS::pointer);
        else if (c == "crosshair")       v->set_cursor(CS::crosshair);
        else if (c == "text")            v->set_cursor(CS::text);
        else if (c == "vertical-text")   v->set_cursor(CS::text);
        else if (c == "grab")            v->set_cursor(CS::grab);
        else if (c == "grabbing")        v->set_cursor(CS::grabbing);
        else if (c == "not-allowed")     v->set_cursor(CS::not_allowed);
        else if (c == "no-drop")         v->set_cursor(CS::not_allowed);
        else if (c == "none" || c == "hidden") v->set_cursor(CS::invisible);
        else if (c == "col-resize" || c == "ew-resize"
                 || c == "e-resize" || c == "w-resize") {
            v->set_cursor(CS::horizontal_resize);
        }
        else if (c == "row-resize" || c == "ns-resize"
                 || c == "n-resize" || c == "s-resize") {
            v->set_cursor(CS::vertical_resize);
        }
        else if (c == "nwse-resize" || c == "nw-resize" || c == "se-resize") {
            v->set_cursor(CS::top_left_resize);
        }
        else if (c == "nesw-resize" || c == "ne-resize" || c == "sw-resize") {
            v->set_cursor(CS::top_right_resize);
        }
        else if (c == "move" || c == "all-scroll") {
            v->set_cursor(CS::multi_directional_resize);
        }
        // CSS cursor keywords with macOS NSCursor backings.
        else if (c == "alias")           v->set_cursor(CS::alias);
        else if (c == "copy")            v->set_cursor(CS::copy);
        else if (c == "zoom-in")         v->set_cursor(CS::zoom_in);
        else if (c == "zoom-out")        v->set_cursor(CS::zoom_out);
        else if (c == "context-menu")    v->set_cursor(CS::context_menu);
        else                             v->set_cursor(CS::default_);
        return choc::value::Value();
    });

    // setDirection(id, "ltr"|"rtl"|"auto") maps the CSS keyword to
    // View::WritingDirection. Yoga's flow honors direction for
    // flexDirection 'row' (which visually reverses under RTL); Skia's
    // paragraph_style.setTextDirection picks up the same value at
    // shape time. Logical-edge mapping in the @pulp/react prop-applier
    // currently stays LTR-only.
    register_bridge_function(api, "setDirection", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto d = args.get<std::string>(1, "ltr");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (!v) return choc::value::Value();
        if (d == "rtl")        v->set_direction(View::WritingDirection::rtl);
        else if (d == "ltr")   v->set_direction(View::WritingDirection::ltr);
        else                   v->set_direction(View::WritingDirection::auto_);
        return choc::value::Value();
    });
}

} // namespace pulp::view

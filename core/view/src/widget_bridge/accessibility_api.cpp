// widget_bridge/accessibility_api.cpp - accessibility registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <string>
#include <utility>

namespace pulp::view {

void WidgetBridge::register_accessibility_api() {
    BridgeApiContext api{engine_};

    // setAccessibilityLabel / setAccessibilityRole are the bridge-side
    // entry points the html-compat layer calls when JS does
    //   el.setAttribute('aria-label', '...')
    //   el.setAttribute('role',       '...').
    //
    // Storage lives on View::access_label_ / View::access_role_ (the
    // existing widget-level a11y slots already consumed by the macOS
    // NSAccessibility bridge in core/view/platform/mac/accessibility_mac.mm
    // and the cross-platform AccessibilityTree snapshot in
    // accessibility_tree.cpp). Linux AT-SPI / Windows UIA platform
    // routing remains a platform concern: the bridge entry point is
    // platform-agnostic; JS-side authors can rely on the same surface
    // on every platform and the storage round-trips through getAttribute
    // either way.
    //
    // Role mapping mirrors the W3C ARIA -> AccessRole bucket used by the
    // existing widget setters: ARIA roles outside Pulp's enum collapse to
    // `group` (the most neutral container role) so VoiceOver still
    // announces the element as an interactive group rather than an
    // unknown blob, and the JS-author intent ("yes, this is exposed to
    // assistive tech") is preserved. Unknown / empty role clears the
    // role back to AccessRole::none.
    register_bridge_function(api, "setAccessibilityLabel",
                             [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto label = args.get<std::string>(1, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) it->second->set_access_label(std::move(label));
        return choc::value::Value();
    });

    register_bridge_function(api, "setAccessibilityRole",
                             [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto role = args.get<std::string>(1, "");
        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();

        // ARIA role token -> View::AccessRole bucket. Mirrors the spirit
        // of NSAccessibilityRole / UIA control-type mapping: collapse the
        // long ARIA tail onto the small Pulp enum, but keep semantic
        // hints alive so the platform layer announces something sensible.
        View::AccessRole r = View::AccessRole::none;
        if (role.empty() || role == "none" || role == "presentation") {
            r = View::AccessRole::none;
        } else if (role == "slider") {
            r = View::AccessRole::slider;
        } else if (role == "checkbox" || role == "switch" || role == "radio") {
            r = View::AccessRole::toggle;
        } else if (role == "img" || role == "image") {
            r = View::AccessRole::image;
        } else if (role == "progressbar" || role == "meter") {
            r = View::AccessRole::meter;
        } else if (role == "heading" || role == "label" ||
                   role == "text"    || role == "paragraph") {
            r = View::AccessRole::label;
        } else {
            // Everything else (button, link, navigation, region, dialog,
            // listbox, menu, ...) collapses to `group`: VoiceOver
            // announces it as a generic interactive group, which is
            // strictly better than treating it as untyped.
            r = View::AccessRole::group;
        }
        it->second->set_access_role(r);
        return choc::value::Value();
    });

    // ARIA state attributes (aria-pressed / aria-checked / aria-disabled /
    // aria-hidden). Tri-state per ARIA 1.2: `true` /
    // `false` / `mixed` / unset. We store the raw string the JS shim
    // hands us so platform AT bridges can read it back verbatim.
    // Single bridge fn rather than four; `attr` selects the slot.
    register_bridge_function(api, "setAccessibilityState",
                             [this](choc::javascript::ArgumentList args) {
        auto id    = args.get<std::string>(0, "");
        auto attr  = args.get<std::string>(1, "");
        auto value = args.get<std::string>(2, "");
        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();
        if      (attr == "pressed")  it->second->set_access_pressed(std::move(value));
        else if (attr == "checked")  it->second->set_access_checked(std::move(value));
        else if (attr == "disabled") it->second->set_access_disabled(std::move(value));
        else if (attr == "hidden")   it->second->set_access_hidden(std::move(value));
        // Unknown aria-* state attr is a no-op for forward compatibility.
        return choc::value::Value();
    });
}

} // namespace pulp::view

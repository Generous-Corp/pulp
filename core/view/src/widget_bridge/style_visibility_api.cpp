// widget_bridge/style_visibility_api.cpp - visibility and interaction style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <string>

namespace pulp::view {

void WidgetBridge::register_widget_style_visibility_api() {
    BridgeApiContext api{engine_};

    // setVisible(id, bool)
    register_bridge_function(api, "setVisible", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto vis = args.get<double>(1, 1) > 0.5;
        auto it = widgets_.find(id);
        if (it != widgets_.end()) it->second->set_visible(vis);
        return choc::value::Value();
    });
}

void WidgetBridge::register_widget_style_interaction_api() {
    BridgeApiContext api{engine_};

    // setPointerEvents(id, "none"|"auto") - CSS pointer-events: skip in hit_test.
    // RN-shaped 4-valued pointerEvents:
    //   "auto"     - default, this view + children intercept events.
    //   "none"     - neither this view nor descendants intercept events.
    //   "box-only" - this view intercepts; children do NOT.
    //   "box-none" - this view does NOT intercept; children do.
    // Keep set_hit_testable() in sync for the binary cases for back-compat
    // with existing scripts, and route the four-valued enum via
    // View::set_pointer_events().
    register_bridge_function(api, "setPointerEvents", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "auto");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (mode == "none") {
            v->set_hit_testable(false);
            v->set_pointer_events(View::PointerEvents::none);
        } else if (mode == "box-only" || mode == "box_only") {
            v->set_hit_testable(true);
            v->set_pointer_events(View::PointerEvents::box_only);
        } else if (mode == "box-none" || mode == "box_none") {
            v->set_hit_testable(true);
            v->set_pointer_events(View::PointerEvents::box_none);
        } else {
            v->set_hit_testable(true);
            v->set_pointer_events(View::PointerEvents::auto_);
        }
        return choc::value::Value();
    });

    // RN backfaceVisibility ("visible"|"hidden"). Stored on the View for
    // plumbing parity with @pulp/react. Pulp's transform model is currently
    // 2D-affine, so this is a no-op for painting today.
    register_bridge_function(api, "setBackfaceVisibility", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "visible");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_backface_visible(mode != "hidden");
        return choc::value::Value();
    });

    // setVisibility(id, "visible"|"hidden") - hidden preserves layout space
    register_bridge_function(api, "setVisibility", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto vis = args.get<std::string>(1, "visible");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            // visibility:hidden = still takes space but not painted
            // We use opacity 0 + still visible for layout
            if (vis == "hidden") { v->set_opacity(0); }
            else { v->set_opacity(1); }
        }
        return choc::value::Value();
    });

    // setWhiteSpace(id, "normal"|"nowrap"|"pre"|"pre-wrap")
    //
    // Sets a generic `View::white_space_nowrap()` flag so ANY widget with a
    // textual surface (Button, custom text-bearing
    // views, future TextEditor surfaces) and `TextShaper` consumers can
    // observe nowrap without dynamic_casting to Label. The original
    // Label::set_multi_line side-effect stays in lock-step so existing
    // single-line Label paint paths, including ellipsis truncation,
    // keep working when only one of the flags is set.
    register_bridge_function(api, "setWhiteSpace", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto ws = args.get<std::string>(1, "normal");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        // Map keyword to View::WhiteSpaceMode enum. The set_white_space_mode
        // setter also maintains the legacy
        // white_space_nowrap_ bool (true for nowrap + pre) so existing
        // consumers of white_space_nowrap() keep working.
        using M = View::WhiteSpaceMode;
        M mode = M::normal;
        if      (ws == "nowrap")        mode = M::nowrap;
        else if (ws == "pre")           mode = M::pre;
        else if (ws == "pre-wrap")      mode = M::pre_wrap;
        else if (ws == "pre-line")      mode = M::pre_line;
        else if (ws == "break-spaces")  mode = M::break_spaces;
        // Unknown keyword falls back to normal (per CSS forward-compat).
        v->set_white_space_mode(mode);
        // Label.multi_line is TRUE for all modes except `nowrap`. Originally `pre`
        // mapped to multi_line=false to match the CSS spec's
        // "no-soft-wrap" semantic, but Pulp's Label only emits hard
        // line breaks via the multi_line splitting path - single-line
        // mode draws the whole string in one fill_text call, dropping
        // `\n`. So <pre> content with newlines silently lost its
        // breaks. Per CSS spec, pre MUST preserve newlines as hard
        // breaks; the only thing it disables is SOFT wrapping at word
        // boundaries (long lines overflow). Pulp's Label doesn't have
        // a separate soft-wrap-vs-hard-break knob today, so we honour
        // the spec-critical "preserve newlines" by keeping
        // multi_line=true for `pre`. Long lines overflow horizontally
        // - a degraded but spec-correct behaviour. Soft-wrap
        // suppression for `pre` is not wired yet.
        if (auto* l = dynamic_cast<Label*>(widget(id))) {
            const bool wraps = (mode != M::nowrap);
            l->set_multi_line(wraps);
        }
        return choc::value::Value();
    });

    // setUserSelect(id, "auto"|"none"|"text"|"all"|"contain") - CSS
    // user-select. Stores the keyword on View::user_select_ so widgets
    // that participate in selection can read it. Unknown keywords map to
    // the spec default (auto).
    register_bridge_function(api, "setUserSelect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto kw = args.get<std::string>(1, "auto");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if      (kw == "none")    v->set_user_select(View::UserSelect::none);
        else if (kw == "text")    v->set_user_select(View::UserSelect::text);
        else if (kw == "all")     v->set_user_select(View::UserSelect::all);
        else if (kw == "contain") v->set_user_select(View::UserSelect::contain);
        else                       v->set_user_select(View::UserSelect::auto_);
        return choc::value::Value();
    });
}

} // namespace pulp::view

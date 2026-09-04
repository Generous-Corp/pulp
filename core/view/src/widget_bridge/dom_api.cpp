// widget_bridge/dom_api.cpp - DOM mutation registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>
#include "api_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace pulp::view {

namespace {

struct InteractionSnapshot {
    ViewCapture focused;
    ViewCapture overlay;
    ViewCapture popup;
    bool had_focused = false;
    bool had_overlay = false;
    bool had_popup = false;

    void capture(View* owner) {
        if (!owner) return;
        if (auto* state = owner->existing_interaction()) {
            if (state->focused_input) {
                focused.set(state->focused_input);
                had_focused = true;
            }
            if (state->active_overlay) {
                overlay.set(state->active_overlay);
                had_overlay = true;
            }
            if (state->active_popup) {
                popup.set(state->active_popup);
                had_popup = true;
            }
        }
    }

    void restore(View* destination) {
        if (!destination) return;
        View* root = destination;
        while (root->parent()) root = root->parent();
        if (had_focused) {
            if (auto* view = focused.live_in(*root)) {
                view->on_focus_changed(true);
                view->claim_input_focus();
            }
        }
        if (had_overlay) {
            if (auto* view = overlay.live_in(*root))
                view->claim_overlay();
        }
        if (had_popup) {
            if (auto* view = dynamic_cast<ComboBox*>(popup.live_in(*root));
                view && !view->is_open())
                view->restore_open_state();
        }
    }
};

} // namespace

void BridgeRegistrars::register_dom_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // __domAppend(parentId, childId, tag) - native appendChild.
    // Creates a native widget under parentId, purely in C++ - no re-entrant
    // JS evaluation which causes stack overflow in QuickJS.
    register_bridge_function(api, "__domAppend", [&self](choc::javascript::ArgumentList args) {
        auto parentId = args.get<std::string>(0, "");
        auto childId = args.get<std::string>(1, "");
        auto tag = args.get<std::string>(2, "div");
        auto hint = args.get<std::string>(3, "");
        auto* existing = self.widget(childId);
        // Move an already-upgraded retained wrapper as one transaction. The
        // wrapper is the actual native owner; moving only its authored child
        // strands the scroll container and invalidates the alias map. Keep the
        // unique_ptr recoverable if destination attachment rejects the move.
        const auto move_wrapper = [](ScrollView* wrapper, View* destination) {
            if (!wrapper || !destination) return false;
            if (wrapper->parent() == destination) return true;
            auto* old_parent = wrapper->parent();
            if (!old_parent) return false;
            auto moved = old_parent->remove_child(wrapper);
            // A reentrant lifecycle/drag callback may have removed the
            // wrapper while remove_child was running. Never treat a null
            // ownership result as a successful native move.
            if (!moved) return false;
            try {
                destination->add_child_transactional(moved);
            } catch (...) {
                if (moved) {
                    try { old_parent->add_child_transactional(moved); }
                    catch (...) { /* preserve the original exception */ }
                }
                throw;
            }
            return true;
        };
        if (existing) {
            auto* destination = self.resolve_parent(parentId);
            // Refuse self/descendant reparenting before touching ownership;
            // otherwise a retained scroll upgrade could create a View cycle.
            bool cyclic = destination == existing;
            for (auto* p = destination; !cyclic && p; p = p->parent())
                cyclic = p == existing;
            if (cyclic) return choc::value::Value();
            if (hint == "scroll") {
                // React may replay the same retained append more than once.
                // If this content already belongs to our wrapper, keep the
                // existing identity instead of nesting a second wrapper.
                auto* wrapper = self.scroll_wrapper(childId);
                const bool wrapper_live = wrapper && std::any_of(
                    self.owned_widgets_.begin(), self.owned_widgets_.end(),
                    [wrapper](const auto& state) { return state.view == wrapper; });
                if (wrapper_live && wrapper->child_count() == 1 &&
                    wrapper->child_at(0) == existing) {
                    if (wrapper->parent() != destination) {
                        InteractionSnapshot interaction;
                        interaction.capture(wrapper);
                        if (!move_wrapper(wrapper, destination))
                            throw std::runtime_error("retained scroll wrapper is detached");
                        interaction.restore(destination);
                    }
                    return choc::value::Value();
                }
            }
            // document.createElement('button') may eagerly materialize a
            // ToggleButton through _ensureNative before appendChild reaches
            // this fast path. Preserve the HTML element's implicit semantics
            // on that already-created widget too; otherwise the early
            // reparent return below skips the new-widget button defaults.
            if (tag == "button") {
                existing->set_implicit_access_role(View::AccessRole::button);
                // _ensureNative currently represents an HTML button with a
                // ToggleButton, whose constructor supplies the stock Pulp
                // `toggle` role. That is an implementation default, not an
                // authored ARIA override. Replace it with the HTML role here;
                // __replayAriaAttributes runs immediately after append and
                // restores any real explicit role from the DOM attributes.
                existing->restore_implicit_access_role();
                existing->set_default_hover_feedback(true);
            }
            // Once a retained authored view has been upgraded, its native
            // parent is the wrapper. A later ordinary DOM reparent must move
            // that wrapper as a unit; moving only the content would strand an
            // empty wrapper and leave the alias map pointing at it.
            if (auto* wrapper = self.scroll_wrapper(childId);
                wrapper && std::any_of(
                    self.owned_widgets_.begin(), self.owned_widgets_.end(),
                    [wrapper](const auto& state) { return state.view == wrapper; }) &&
                wrapper->child_count() == 1 && wrapper->child_at(0) == existing) {
                if (wrapper->parent() != destination) {
                    InteractionSnapshot interaction;
                    interaction.capture(wrapper);
                    if (!move_wrapper(wrapper, destination))
                        throw std::runtime_error("retained scroll wrapper is detached");
                    interaction.restore(destination);
                }
                return choc::value::Value();
            }
            if (auto* p = existing->parent()) {
                // A preserved DOM reparent can carry a stronger container
                // hint than the widget's original materialization.  In
                // particular, Settings upgrades a retained plain div to a
                // real ScrollView after the portal is reopened.  Do not let
                // the existing-widget fast path silently discard that hint.
                if (hint == "scroll"
                    && dynamic_cast<ScrollView*>(existing) == nullptr) {
                    InteractionSnapshot interaction;
                    interaction.capture(p);
                    auto removed = p->remove_child(existing);
                    std::unique_ptr<View> scroll = std::make_unique<ScrollView>();
                    // Keep the authored View as the ScrollView's content
                    // child instead of replacing/destroying it. This
                    // preserves every callback, style, focus, gesture, and
                    // DOM identity while the wrapper supplies native scroll
                    // layout/hit-testing. The registry points at the wrapper
                    // for scroll APIs; event delivery still reaches the
                    // original child with its identity-safe closures.
                    const auto content_bounds = removed->bounds();
                    auto* content = removed.get();
                    scroll->set_bounds(content_bounds);
                    removed->set_bounds({0.0f, 0.0f,
                                         content_bounds.width,
                                         content_bounds.height});
                    auto* wrapper = static_cast<ScrollView*>(scroll.get());
                    // The authored parent id is authoritative during portal
                    // replay. The retained native parent can be the stale
                    // root container even while the JS parent has recovered.
                    try {
                        destination->add_child_transactional(scroll);
                    } catch (...) {
                        // The retained content is still owned locally here;
                        // put it back under its original parent before
                        // propagating the attach failure.
                        try { p->add_child_transactional(removed); }
                        catch (...) { /* preserve the original exception */ }
                        throw;
                    }
                    // Attach the retained content only after its new wrapper
                    // is attached. This preserves host/frame-clock ordering:
                    // on_attached() observes the real destination, not an
                    // unattached intermediate wrapper.
                    try {
                        wrapper->add_child_transactional(removed);
                    } catch (...) {
                        // add_child_transactional restores `removed` on a
                        // throwing hook. Remove the empty wrapper and restore
                        // the original parent/identity before rethrowing.
                        auto stranded = destination->remove_child(wrapper);
                        (void)stranded;
                        try { p->add_child_transactional(removed); }
                        catch (...) { /* preserve the original exception */ }
                        throw;
                    }
                    // Publish the wrapper alias only after both structural
                    // operations succeed. This prevents a throwing attach
                    // hook from exposing a half-built wrapper in the bridge
                    // registries; the authored content remains the canonical
                    // widget identity for ordinary DOM/value APIs.
                    self.owned_widgets_.emplace_back(wrapper);
                    self.scroll_wrappers_[childId] = wrapper;
                    self.widgets_.cache(childId, content);
                    // remove_child() intentionally retires root interaction
                    // slots while detached. Restore slots that belonged to
                    // this retained subtree after its new ancestry is live.
                    // remove_child() deliberately retires root interaction
                    // slots while detached. Resolve the captured identities
                    // only after the new ancestry is live; destroyed or
                    // replaced owners are ignored safely.
                    interaction.restore(destination);
                    return choc::value::Value();
                }
                // Move the existing subtree to the new parent - don't erase widgets.
                auto* destination = self.resolve_parent(parentId);
                // Refresh the cache before structural mutation. The assignment
                // is then unable to strand a detached subtree if allocation
                // fails while recording the same authored identity.
                self.widgets_.cache(childId, existing);
                auto removed = p->remove_child(existing);
                if (!removed)
                    throw std::runtime_error("native reparent lost widget ownership");
                try {
                    destination->add_child_transactional(removed);
                } catch (...) {
                    if (removed) {
                        try { p->add_child_transactional(removed); }
                        catch (...) { /* preserve the original exception */ }
                    }
                    throw;
                }
                return choc::value::Value();
            }
        }
        // Create the appropriate widget type based on HTML tag.
        //
        // This fast-path bypasses the JS-side `_ensureNative` for performance
        // and QuickJS stack reasons, but it MUST mirror the
        // tag->widget mapping in `web-compat-element.js` or web-compat
        // semantics drift between the createElement+appendChild path and
        // the React-style commit path that goes through here.
        std::unique_ptr<View> child;
        if (hint == "scroll") {
            auto scroll = std::make_unique<ScrollView>();
            scroll->set_id(childId);
            child = std::move(scroll);
        } else if (tag == "span" || tag == "p" || tag == "label" ||
            tag == "h1" || tag == "h2" || tag == "h3" ||
            tag == "h4" || tag == "h5" || tag == "h6") {
            auto lbl = std::make_unique<Label>();
            lbl->set_id(childId);
            child = std::move(lbl);
        } else if (tag == "canvas") {
            auto canvas = std::make_unique<CanvasWidget>();
            canvas->set_id(childId);
            canvas->set_native_gpu_texture_provider([&self, childId]() {
                return self.describe_native_canvas_frame(childId);
            });
            child = std::move(canvas);
        } else if (tag == "rect") {
            // SVG primitives other than <path>. Spectr's bottom-toolbar
            // mini-icons (segmented mode toggles, analyzer
            // pills) emit lowercase <rect> / <line> / <circle> inside the
            // parent <svg>. Without this routing they fall through to the
            // unknown-tag default and paint nothing.
            auto r = std::make_unique<SvgRectWidget>();
            r->set_id(childId);
            child = std::move(r);
        } else if (tag == "line") {
            auto l = std::make_unique<SvgLineWidget>();
            l->set_id(childId);
            child = std::move(l);
        } else if (tag == "circle") {
            // No dedicated SvgCircleWidget; map to SvgPath and synthesize a
            // `d` arc in JS (web-compat-element.js
            // __replaySvgCircleAttributes__) from cx/cy/r.
            auto svg = std::make_unique<SvgPathWidget>();
            svg->set_id(childId);
            child = std::move(svg);
        } else if (tag == "path") {
            // Mirror the JS-side _ensureNative routing: `<path>` (typically
            // inside an `<svg>`) materializes as the
            // SvgPathWidget so the d / stroke / stroke-width / fill /
            // viewBox attribute replay actually paints. Without this
            // branch the React/JSX commit path (which goes through
            // __domAppend, bypassing _ensureNative) would silently
            // create a plain View and the SVG glyph never renders.
            auto svg = std::make_unique<SvgPathWidget>();
            svg->set_id(childId);
            child = std::move(svg);
        } else if (tag == "input") {
            // `<input>` needs the JS-side `_type` to pick a widget.
            // JS callers pass it through the optional 4th `hint`
            // arg ("range:horizontal", "range:vertical", "checkbox", "text").
            // Without a hint, fall back to a plain View so the element
            // still receives child/style ops.
            if (hint == "range:horizontal" || hint == "range:vertical") {
                auto fader = std::make_unique<Fader>();
                fader->set_id(childId);
                if (hint == "range:horizontal") {
                    fader->set_orientation(Fader::Orientation::horizontal);
                }
                child = std::move(fader);
            } else if (hint == "checkbox") {
                auto cb = std::make_unique<Checkbox>();
                cb->set_id(childId);
                child = std::move(cb);
            } else if (hint == "text") {
                // Plain text `<input>` (and text-like subtypes: search / email / url
                // / tel / password) materialize as a TextEditor so they
                // accept keyboard input instead of becoming a non-editable View.
                auto te = std::make_unique<TextEditor>();
                te->set_id(childId);
                child = std::move(te);
            } else {
                auto v = std::make_unique<View>();
                v->set_id(childId);
                child = std::move(v);
            }
        } else if (auto widget_for_tag = self.make_widget_for_tag(tag, childId)) {
            // Route lowercase `@pulp/react` widget intrinsics
            // (knob/fader/toggle/combo/
            // checkbox/spectrum/waveform/meter/xypad/listbox/virtuallist/icon, plus the
            // select/progress/img HTML aliases) to native widgets here in the
            // React-commit fast path. `<Knob>` etc. lower to lowercase DOM
            // tags in the live-JSX path (`pulp import-design --from jsx --mode
            // live --emit js`, run via `Standalone --pulp-bundle`), and child
            // widgets are created HERE rather than through JS `_ensureNative`
            // or the `createX` factories. Before this they fell to the
            // plain-View default below - no drag, no callbacks. The shared
            // `make_widget_for_tag` table wires callbacks (the load-bearing
            // `on_change -> __dispatch__`) and keeps this surface in lockstep
            // with `_ensureNative` and the `@pulp/react` host-config map.
            child = std::move(widget_for_tag);
        } else {
            auto v = std::make_unique<View>();
            v->set_id(childId);
            if (tag == "div" || tag == "section" || tag == "article" || tag == "aside" ||
                tag == "header" || tag == "footer" || tag == "nav" || tag == "main")
                v->flex().direction = FlexDirection::column;
            // <svg> is a layout-leaf media element. Default direction stays
            // column so child <path>/<g> attaches; the
            // presentational width/height attributes are replayed via
            // setFlex() on the JS side (see web-compat-element.js
            // setAttribute() path).
            child = std::move(v);
        }
        // Native/materialized HTML buttons may never receive an explicit ARIA
        // role: the React-DOM commit fast path appends the lowercase tag
        // directly through __domAppend. Give that semantic element the same
        // built-in role and hover feedback as an explicitly-role'd button.
        // A later explicit role remains authoritative; removing it restores
        // this implicit HTML role and its default hover feedback.
        if (tag == "button") {
            child->set_implicit_access_role(View::AccessRole::button);
            child->restore_implicit_access_role();
            child->set_default_hover_feedback(true);
        }
        self.widgets_[childId] = child.get();
        self.resolve_parent(parentId)->add_child(std::move(child));
        return choc::value::Value();
    });

    // __domRemove(childId) - native removeChild implementation.
    register_bridge_function(api, "__domRemove", [&self](choc::javascript::ArgumentList args) {
        auto childId = args.get<std::string>(0, "");
        const bool preserve_js_dom_state = args.get<int>(1, 0) != 0;
        auto* w = self.widget(childId);
        if (w) {
            if (auto* p = w->parent()) {
                auto removed = p->remove_child(w);
                self.forget_widget_subtree(removed.get(), preserve_js_dom_state);
                self.retire_removed_widget(std::move(removed));
            }
        }
        return choc::value::Value();
    });
}

} // namespace pulp::view

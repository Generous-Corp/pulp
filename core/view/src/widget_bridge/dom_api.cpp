// widget_bridge/dom_api.cpp - DOM mutation registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>
#include "api_registry.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace pulp::view {

namespace {

void erase_widget_subtree(std::unordered_map<std::string, View*>& widgets, View* node) {
    if (node == nullptr) {
        return;
    }

    for (size_t i = 0; i < node->child_count(); ++i) {
        erase_widget_subtree(widgets, node->child_at(i));
    }

    if (!node->id().empty()) {
        widgets.erase(node->id());
    }
}

} // namespace

void WidgetBridge::register_dom_api() {
    BridgeApiContext api{engine_};

    // __domAppend(parentId, childId, tag) - native appendChild.
    // Creates a native widget under parentId, purely in C++ - no re-entrant
    // JS evaluation which causes stack overflow in QuickJS.
    register_bridge_function(api, "__domAppend", [this](choc::javascript::ArgumentList args) {
        auto parentId = args.get<std::string>(0, "");
        auto childId = args.get<std::string>(1, "");
        auto tag = args.get<std::string>(2, "div");
        auto* existing = widget(childId);
        if (existing) {
            if (auto* p = existing->parent()) {
                // Move the existing subtree to the new parent - don't erase widgets.
                auto removed = p->remove_child(existing);
                widgets_[childId] = removed.get();
                resolve_parent(parentId)->add_child(std::move(removed));
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
        if (tag == "span" || tag == "p" || tag == "label" ||
            tag == "h1" || tag == "h2" || tag == "h3" ||
            tag == "h4" || tag == "h5" || tag == "h6") {
            auto lbl = std::make_unique<Label>();
            lbl->set_id(childId);
            child = std::move(lbl);
        } else if (tag == "canvas") {
            auto canvas = std::make_unique<CanvasWidget>();
            canvas->set_id(childId);
            canvas->set_native_gpu_texture_provider([this, childId]() {
                return this->describe_native_canvas_frame(childId);
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
            auto hint = args.get<std::string>(3, "");
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
        } else if (auto widget_for_tag = make_widget_for_tag(tag, childId)) {
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
        widgets_[childId] = child.get();
        resolve_parent(parentId)->add_child(std::move(child));
        return choc::value::Value();
    });

    // __domRemove(childId) - native removeChild implementation.
    register_bridge_function(api, "__domRemove", [this](choc::javascript::ArgumentList args) {
        auto childId = args.get<std::string>(0, "");
        auto* w = widget(childId);
        if (w) {
            if (auto* p = w->parent()) {
                auto removed = p->remove_child(w);
                erase_widget_subtree(widgets_, removed.get());
                prune_dangling_bindings();
            }
        }
        return choc::value::Value();
    });
}

} // namespace pulp::view

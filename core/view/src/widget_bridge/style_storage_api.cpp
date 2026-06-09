// widget_bridge/style_storage_api.cpp - storage-only CSS style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <string>

namespace pulp::view {

void WidgetBridge::register_widget_style_background_repeat_api() {
    BridgeApiContext api{engine_};

    // setBackgroundRepeat(id, kw) - CSS background-repeat keyword. Storage-
    // only on the View (no-op for solid-color backgrounds, which is the
    // only currently rendered case). Future paint work for
    // `background-image: url(...)` / repeating gradients consults the
    // stored slot; setting the keyword today makes the round-trip work
    // and lets authors express intent without dropping the prop silently.
    // Accepts: `repeat` / `repeat-x` / `repeat-y` / `no-repeat` /
    // `space` / `round`. Unknown / empty resets to "" (paint defaults to
    // CSS initial `repeat`).
    register_bridge_function(api, "setBackgroundRepeat", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto kw = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_background_repeat(kw);
        return choc::value::Value();
    });
}

void WidgetBridge::register_widget_style_mask_object_api() {
    BridgeApiContext api{engine_};

    // setMaskImage(id, value) - CSS `mask-image` (pulp #1515).
    // Storage-only today; the saveLayer + SkBlendMode::kDstIn shader
    // composite is a follow-up paint slice. The slot round-trips
    // through View::mask_image() so harness tests can assert the
    // bridge accepted the value.
    register_bridge_function(api, "setMaskImage",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_mask_image(value);
            return choc::value::Value();
        });

    // setMask(id, shorthand) - CSS `mask` shorthand (pulp #1515).
    // Stores the verbatim shorthand on the View; the JS shim
    // (web-compat-style-decl.js) is responsible for fanning out into
    // the maskImage longhand. Storage-only today.
    register_bridge_function(api, "setMask",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_mask(value);
            return choc::value::Value();
        });

    // setMaskSize(id, value) - CSS `mask-size`, pairs with mask-image
    // (pulp #1515 followup). Storage-only; consumed by the same
    // future paint slice that wires the mask shader.
    register_bridge_function(api, "setMaskSize",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_mask_size(value);
            return choc::value::Value();
        });

    // setAppearance(id, value) - CSS `appearance`. Pulp paints all
    // widgets custom (no native form-widget rendering), so this is
    // observably storage-only: `none` is the effective default for
    // every Pulp View regardless of what the slot says. The slot
    // exists so authors who set `appearance: none` for reset-style
    // consistency see a no-op (not an unsupported drop).
    register_bridge_function(api, "setAppearance",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_appearance(value);
            return choc::value::Value();
        });

    // setObjectFit(id, value) - CSS `object-fit`. Storage-only today;
    // the ImageView paint slice that consumes this needs access to
    // the decoded image's natural size (planned follow-up).
    register_bridge_function(api, "setObjectFit",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_object_fit(value);
            return choc::value::Value();
        });

    // setObjectPosition(id, value) - CSS `object-position`. Pairs
    // with object-fit. Storage-only today.
    register_bridge_function(api, "setObjectPosition",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_object_position(value);
            return choc::value::Value();
        });
}

void WidgetBridge::register_widget_style_background_subproperty_api() {
    BridgeApiContext api{engine_};

    // pulp #1517 - background sub-property setters. Storage-only today;
    // see View::set_background_{attachment,clip,origin}() doc for the
    // partial-vs-noop semantics. Wiring them here unblocks the JS shim
    // path and lets the catalog honestly report `noop` / `partial`
    // instead of `missing`.
    register_bridge_function(api, "setBackgroundAttachment",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_attachment(kw);
            return choc::value::Value();
        });
    register_bridge_function(api, "setBackgroundClip",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_clip(kw);
            return choc::value::Value();
        });
    register_bridge_function(api, "setBackgroundOrigin",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_origin(kw);
            return choc::value::Value();
        });

    // Wave 5 css.5 - setBackgroundPosition / setBackgroundSize. The JS
    // shim (web-compat-style-decl.js cases backgroundPosition /
    // backgroundSize) was already calling these as `typeof set... ===
    // "function"` guards; without a registered bridge fn the calls were
    // silent no-ops and the catalog claim of `supported` was a fiction.
    // Storage-only landing here makes the round-trip honest (JS -> bridge
    // -> View slot -> get_attribute pulls it back) and unblocks a future
    // raster background-image paint slice - see View::set_background_*
    // doc for the architectural caveat.
    register_bridge_function(api, "setBackgroundPosition",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_position(kw);
            return choc::value::Value();
        });
    register_bridge_function(api, "setBackgroundSize",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_background_size(kw);
            return choc::value::Value();
        });
}

} // namespace pulp::view

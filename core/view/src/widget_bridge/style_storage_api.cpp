// widget_bridge/style_storage_api.cpp - storage-only CSS style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <string>

namespace pulp::view {

namespace {

// Rewrite each `url(...)` image reference in a CSS image value so that a
// filesystem reference names the path resolve_script_relative() admits, and
// a refused one becomes `none`. Fragment (`#id`) and `data:` references are
// not files and are kept as written.
std::string confine_css_image_urls(const WidgetBridge& bridge, const std::string& value) {
    std::string out;
    std::size_t pos = 0;
    while (true) {
        const auto open = value.find("url(", pos);
        if (open == std::string::npos)
            break;
        const auto close = value.find(')', open);
        if (close == std::string::npos)
            break;
        out.append(value, pos, open - pos);
        std::string ref = value.substr(open + 4, close - (open + 4));
        while (!ref.empty() && (ref.front() == ' ' || ref.front() == '"' || ref.front() == '\''))
            ref.erase(ref.begin());
        while (!ref.empty() && (ref.back() == ' ' || ref.back() == '"' || ref.back() == '\''))
            ref.pop_back();
        if (ref.empty() || ref.front() == '#' || ref.rfind("data:", 0) == 0) {
            out.append(value, open, close + 1 - open);
        } else {
            const auto resolved = bridge.resolve_script_relative(ref);
            if (resolved.empty() || resolved.find_first_of("\"')") != std::string::npos)
                out += "none";
            else
                out += "url(\"" + resolved + "\")";
        }
        pos = close + 1;
    }
    out.append(value, pos, std::string::npos);
    return out;
}

} // namespace

void BridgeRegistrars::register_widget_style_background_repeat_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setBackgroundRepeat(id, kw) - CSS background-repeat keyword. Storage-
    // only on the View (no-op for solid-color backgrounds, which is the
    // only currently rendered case). Keeping the slot makes the round-trip
    // work for imported styles and gives future raster / gradient
    // background paint code an explicit repeat keyword to consume.
    // Accepts: `repeat` / `repeat-x` / `repeat-y` / `no-repeat` /
    // `space` / `round`. Unknown / empty resets to "" (paint defaults to
    // CSS initial `repeat`).
    register_bridge_function(api, "setBackgroundRepeat", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto kw = args.get<std::string>(1, "");
        auto* v = id.empty() ? &self.root_ : self.widget(id);
        if (v) v->set_background_repeat(kw);
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_widget_style_mask_object_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // setMaskImage(id, value) - CSS `mask-image`.
    // The bridge stores the value on the View; mask-capable paint backends
    // consume the slot, while fallback backends still round-trip it through
    // View::mask_image().
    register_bridge_function(api, "setMaskImage",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            // A url() mask is decoded from the file it names, so it is held
            // to the same containment rule as every other image path.
            if (v)
                v->set_mask_image(confine_css_image_urls(self, value));
            return choc::value::Value();
        });

    // setMask(id, shorthand) - CSS `mask` shorthand.
    // Stores the verbatim shorthand on the View; the JS shim
    // (web-compat-style-decl.js) is responsible for fanning out into
    // the maskImage longhand.
    register_bridge_function(api, "setMask",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_mask(value);
            return choc::value::Value();
        });

    // setMaskSize(id, value) - CSS `mask-size`, stored with mask-image for
    // paint paths that can apply a mask.
    register_bridge_function(api, "setMaskSize",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
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
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_appearance(value);
            return choc::value::Value();
        });

    // setObjectFit(id, value) - CSS `object-fit`. The bridge stores the
    // keyword; visualizer/image-like paint paths consume it when mapping
    // source content into their bounds.
    register_bridge_function(api, "setObjectFit",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_object_fit(value);
            return choc::value::Value();
        });

    // setObjectPosition(id, value) - CSS `object-position`. Stored with
    // object-fit for paint paths that align content inside the destination rect.
    register_bridge_function(api, "setObjectPosition",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto value = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_object_position(value);
            return choc::value::Value();
        });
}

void BridgeRegistrars::register_widget_style_background_subproperty_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // Background sub-property setters. See
    // View::set_background_{attachment,clip,origin}() for the
    // partial-vs-noop semantics; wiring them here lets the JS shim path
    // round-trip these keywords instead of dropping them.
    register_bridge_function(api, "setBackgroundAttachment",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_background_attachment(kw);
            return choc::value::Value();
        });
    register_bridge_function(api, "setBackgroundClip",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_background_clip(kw);
            return choc::value::Value();
        });
    register_bridge_function(api, "setBackgroundOrigin",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_background_origin(kw);
            return choc::value::Value();
        });

    // setBackgroundPosition / setBackgroundSize. The JS shim calls these
    // behind `typeof set... === "function"` guards; registering the bridge
    // functions makes the round-trip explicit (JS -> bridge -> View slot ->
    // get_attribute) while raster background-image paint remains deferred.
    register_bridge_function(api, "setBackgroundPosition",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_background_position(kw);
            return choc::value::Value();
        });
    register_bridge_function(api, "setBackgroundSize",
        [&self](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "");
            auto* v = id.empty() ? &self.root_ : self.widget(id);
            if (v) v->set_background_size(kw);
            return choc::value::Value();
        });
}

} // namespace pulp::view

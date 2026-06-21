// widget_bridge/animation_api.cpp - animation and transform registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <pulp/view/css_animation.hpp>
#include <pulp/view/ui_components.hpp>

#include <choc/text/choc_JSON.h>

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void WidgetBridge::register_animation_api() {
    BridgeApiContext api{engine_};

    // animate(id, property, targetValue, durationMs, easingName)
    // animate(id, property, target, duration_ms, easing) - CSS transition equivalent.
    // Smoothly interpolates a property from current to target over duration.
    register_bridge_function(api, "animate", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "value");
        auto target = static_cast<float>(args.get<double>(2, 0));
        auto dur_ms = static_cast<float>(args.get<double>(3, 150));
        auto ease_name = args.get<std::string>(4, "ease_out_cubic");

        auto* v = widget(id);
        if (!v) return choc::value::Value();

        float dur = dur_ms / 1000.0f;
        (void)ease_name; // easing for future ValueAnimation integration

        // Apply property changes; duration is retained for future interpolation work.
        if (prop == "opacity") {
            v->set_opacity(target);
        } else if (prop == "scale") {
            v->set_scale(target);
        } else if (prop == "translate_x") {
            v->set_translate(target, v->translate_y());
        } else if (prop == "translate_y") {
            v->set_translate(v->translate_x(), target);
        } else if (prop == "rotation") {
            v->set_rotation(target);
        } else if (prop == "value") {
            if (auto* k = dynamic_cast<Knob*>(v)) k->set_value(target);
            else if (auto* f = dynamic_cast<Fader*>(v)) f->set_value(target);
            else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_on(target > 0.5f);
        }
        (void)dur;
        return choc::value::Value();
    });
}

void WidgetBridge::register_animation_style_api() {
    BridgeApiContext api{engine_};

    // setTransitionDuration(id, seconds) - CSS transition duration for animated property changes.
    register_bridge_function(api, "setTransitionDuration", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto dur = static_cast<float>(args.get<double>(1, 0.15));
        // Store transition duration on the view's theme as a dimension token.
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            auto theme = v->theme();
            theme.dimensions["transition.duration"] = dur;
            v->set_theme(theme);
        }
        return choc::value::Value();
    });

    // setTransition(id, "opacity 200ms ease, transform 300ms").
    // Parses the full CSS shorthand into View::transitions_ so property
    // application can consult the parsed transition specs when values change.
    register_bridge_function(api, "setTransition", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto css = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (css.empty() || css == "none") {
            v->clear_transitions();
            return choc::value::Value();
        }
        v->set_transitions(parse_transition_shorthand(css));
        return choc::value::Value();
    });

    // setTransitionProperty(id, "opacity, transform") - comma-separated
    // property list. We synthesize TransitionSpecs with just the property
    // name; durations are picked up from setTransitionDuration / the
    // shorthand path. CSS spec: shorthand wins over longhand when both
    // are set in the same rule; we treat them as additive at this layer.
    register_bridge_function(api, "setTransitionProperty", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto props = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        std::vector<TransitionSpec> ts;
        std::string acc;
        auto flush = [&]() {
            while (!acc.empty() && std::isspace(static_cast<unsigned char>(acc.front()))) acc.erase(0, 1);
            while (!acc.empty() && std::isspace(static_cast<unsigned char>(acc.back()))) acc.pop_back();
            if (!acc.empty()) {
                TransitionSpec s{};
                s.property_name = acc;
                s.property = animatable_property_from_css_name(acc);
                ts.push_back(std::move(s));
                acc.clear();
            }
        };
        for (char c : props) {
            if (c == ',') flush(); else acc += c;
        }
        flush();
        v->set_transitions(std::move(ts));
        return choc::value::Value();
    });

    // setTransitionTimingFunction(id, "ease-in-out") - applies to all
    // existing TransitionSpecs on the View. CSS spec: longhand applies
    // uniformly across the property list.
    register_bridge_function(api, "setTransitionTimingFunction", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto easing_str = args.get<std::string>(1, "ease");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        auto current = v->transitions();
        for (auto& t : current) {
            t.easing = CssEasing::from_keyword(easing_str);
        }
        v->set_transitions(std::move(current));
        return choc::value::Value();
    });

    // setTransitionDelay(id, seconds)
    register_bridge_function(api, "setTransitionDelay", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto delay = static_cast<float>(args.get<double>(1, 0.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        auto current = v->transitions();
        for (auto& t : current) t.delay_seconds = delay;
        v->set_transitions(std::move(current));
        return choc::value::Value();
    });

    // setTranslate(id, x, y) - CSS transform: translate().
    register_bridge_function(api, "setTranslate", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = static_cast<float>(args.get<double>(1, 0));
        auto y = static_cast<float>(args.get<double>(2, 0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_translate(x, y);
        return choc::value::Value();
    });

    // setRotation(id, degrees) - CSS transform: rotate().
    register_bridge_function(api, "setRotation", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto deg = static_cast<float>(args.get<double>(1, 0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_rotation(deg);
        return choc::value::Value();
    });

    // setTransform(id, a, b, c, d, e, f) - CSS transform: matrix(a,b,c,d,e,f).
    // Applied at View paint time as a concat onto the current canvas matrix
    // so it composes with parent transforms and child Views inherit it.
    // Layout (Yoga + hit-test) sees the un-transformed bounds: paint-only.
    // Companion to canvasSetTransform, but applied to the View's painting
    // frame rather than a Canvas2D context.
    register_bridge_function(api, "setTransform", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto a = static_cast<float>(args.get<double>(1, 1.0));
        auto b = static_cast<float>(args.get<double>(2, 0.0));
        auto c = static_cast<float>(args.get<double>(3, 0.0));
        auto d = static_cast<float>(args.get<double>(4, 1.0));
        auto e = static_cast<float>(args.get<double>(5, 0.0));
        auto f = static_cast<float>(args.get<double>(6, 0.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_transform_matrix(a, b, c, d, e, f);
        return choc::value::Value();
    });

    // clearTransform(id) - drop the affine matrix; the View reverts to its
    // CSS-transform scalars (translate/rotate/scale) only. Mirrors removing
    // the inline `transform` property in CSS.
    register_bridge_function(api, "clearTransform", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->clear_transform_matrix();
        return choc::value::Value();
    });

    // setTransformOrigin(id, x, y) - CSS transform-origin (0-1 normalized).
    register_bridge_function(api, "setTransformOrigin", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = static_cast<float>(args.get<double>(1, 0.5));
        auto y = static_cast<float>(args.get<double>(2, 0.5));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_transform_origin(x, y);
        return choc::value::Value();
    });

    // defineKeyframes(name, stops_json_string).
    // Stops are JSON-encoded for bridge simplicity:
    //   defineKeyframes('fade', JSON.stringify([
    //     { offset: 0,   properties: { opacity: '0' } },
    //     { offset: 1.0, properties: { opacity: '1' } },
    //   ]));
    // The CSS shim's @keyframes parser produces this shape directly.
    // Populates the application-wide registry consulted by setAnimation.
    register_bridge_function(api, "defineKeyframes", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto stops_json = args.get<std::string>(1, "[]");
        if (name.empty()) return choc::value::Value();
        CssKeyframesBlock block;
        block.name = name;
        // Parse the JSON via choc::json which already lives in the
        // tree (see DEPENDENCIES.md). Walk the array structurally.
        try {
            auto parsed = choc::json::parse(stops_json);
            if (parsed.isArray()) {
                for (uint32_t i = 0; i < parsed.size(); ++i) {
                    const auto& entry = parsed[i];
                    CssKeyframe kf{};
                    if (entry.hasObjectMember("offset")) {
                        kf.offset = static_cast<float>(entry["offset"].getWithDefault<double>(0.0));
                    }
                    if (entry.hasObjectMember("properties") && entry["properties"].isObject()) {
                        const auto& props = entry["properties"];
                        for (uint32_t j = 0; j < props.size(); ++j) {
                            std::string mn(props.getObjectMemberAt(j).name);
                            std::string val(props[mn.c_str()].getWithDefault<std::string_view>(""));
                            kf.properties.emplace_back(mn, val);
                        }
                    }
                    block.stops.push_back(std::move(kf));
                }
            }
        } catch (...) {
            // Malformed input: silently drop (registry stays unchanged).
            return choc::value::Value();
        }
        css_keyframes_registry_.define(std::move(block));
        return choc::value::Value();
    });

    // setAnimation supports two ABIs.
    //
    //   Positional (new, @pulp/react direct callers):
    //     setAnimation(id, animation_name, duration, iterations, direction)
    //   Legacy control-token (web-compat-style-decl.js, one CSS longhand
    //   per call, and prop-applier's animation* fan-out):
    //     setAnimation(id, "name",       <animation-name>)
    //     setAnimation(id, "duration",   <seconds>)
    //     setAnimation(id, "delay",      <seconds>)
    //     setAnimation(id, "easing",     <css-easing-keyword>)
    //     setAnimation(id, "iterations", <number | -1 for infinite>)
    //     setAnimation(id, "direction",  <"normal"|"reverse"|...>)
    //     setAnimation(id, "fill",       <"none"|"forwards"|...>)
    //
    // Detection: if arg1 matches one of the control tokens, dispatch to
    // the legacy path; otherwise treat as positional. The legacy path
    // accumulates state in View::staged_animation(); when `name` arrives
    // and resolves against the keyframes registry, we seed entries into
    // active_animations() using the staged values. Keep the token
    // dispatch ahead of the positional path; otherwise web-compat
    // longhand calls such as "name" / "duration" are mistaken for
    // animation names and the keyframe registry lookup misses.
    register_bridge_function(api, "setAnimation", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto arg1 = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();

        // Legacy control-token dispatch.
        const bool is_control_token =
            arg1 == "name"      || arg1 == "duration"  || arg1 == "delay"
         || arg1 == "easing"    || arg1 == "iterations"|| arg1 == "direction"
         || arg1 == "fill"      || arg1 == "play_state";
        if (is_control_token) {
            auto& staged = v->staged_animation();
            if (arg1 == "name") {
                staged.name = args.get<std::string>(2, "");
                // Re-resolve when the name token arrives (the typical
                // call order is name -> duration -> easing -> ..., but
                // either order is valid; if duration arrives first we
                // simply seed when name arrives, using whatever
                // duration was previously staged).
                const auto* block = css_keyframes_registry_.find(staged.name);
                if (block && !block->stops.empty()) {
                    const auto& first = block->stops.front();
                    for (const auto& [prop, _val] : first.properties) {
                        CssAnimation a{};
                        a.property = animatable_property_from_css_name(prop);
                        a.spec.property_name = prop;
                        a.spec.property = a.property;
                        a.spec.duration_seconds = staged.duration_seconds;
                        a.spec.delay_seconds    = staged.delay_seconds;
                        a.spec.easing           = staged.easing;
                        v->active_animations().push_back(std::move(a));
                    }
                }
            } else if (arg1 == "duration") {
                staged.duration_seconds = static_cast<float>(args.get<double>(2, 0.0));
                for (auto& a : v->active_animations()) {
                    a.spec.duration_seconds = staged.duration_seconds;
                }
            } else if (arg1 == "delay") {
                staged.delay_seconds = static_cast<float>(args.get<double>(2, 0.0));
                for (auto& a : v->active_animations()) {
                    a.spec.delay_seconds = staged.delay_seconds;
                }
            } else if (arg1 == "easing") {
                staged.easing = CssEasing::from_keyword(args.get<std::string>(2, ""));
                for (auto& a : v->active_animations()) {
                    a.spec.easing = staged.easing;
                }
            } else if (arg1 == "iterations") {
                staged.iterations = static_cast<float>(args.get<double>(2, 1.0));
            } else if (arg1 == "direction") {
                staged.direction = args.get<std::string>(2, "normal");
            } else if (arg1 == "fill") {
                staged.fill_mode = args.get<std::string>(2, "");
            } else if (arg1 == "play_state") {
                // Storage-only today; the playback driver does not pause or
                // resume active animations yet. Mirror to View's storage slot so
                // round-trip queries work without poking into the
                // staged struct.
                v->set_animation_play_state(args.get<std::string>(2, "running"));
            }
            return choc::value::Value();
        }

        // Positional dispatch (new ABI).
        const auto& anim_name = arg1;
        auto duration = static_cast<float>(args.get<double>(2, 0.0));
        (void)args.get<double>(3, 1.0);  // iterations, not driven by playback yet
        (void)args.get<std::string>(4, "normal");  // direction, not driven by playback yet
        const auto* block = css_keyframes_registry_.find(anim_name);
        if (!block || block->stops.empty()) return choc::value::Value();
        // Seed one Animation per property the first stop touches. Playback
        // currently records the parsed property and timing state; property-
        // specific value interpolation remains owned by the frame driver.
        const auto& first = block->stops.front();
        for (const auto& [prop, _val] : first.properties) {
            CssAnimation a{};
            a.property = animatable_property_from_css_name(prop);
            a.spec.property_name = prop;
            a.spec.property = a.property;
            a.spec.duration_seconds = duration;
            a.spec.delay_seconds = 0.0f;
            a.spec.easing = CssEasing{};
            v->active_animations().push_back(std::move(a));
        }
        return choc::value::Value();
    });

    // setScale(id, scale) - CSS transform: scale().
    register_bridge_function(api, "setScale", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_scale(s);
        return choc::value::Value();
    });

    // setSkew(id, x_deg, y_deg) - CSS transform: skewX() / skewY().
    // The CSS shim's parseTransform dispatches each axis independently
    // (skewX(alpha) -> setSkew(id, alpha, 0); skewY(beta) -> setSkew(id, 0, beta));
    // when both appear in the same transform string the second
    // call's arg-pattern preserves the axis the first call set
    // (caller-side accumulation since within-string order is
    // canonical CSS application order). The @pulp/react prop-applier
    // walker accumulates skewX/skewY in its snapshot the same way.
    register_bridge_function(api, "setSkew", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = static_cast<float>(args.get<double>(1, 0.0));
        auto y = static_cast<float>(args.get<double>(2, 0.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_skew(x, y);
        return choc::value::Value();
    });
}

} // namespace pulp::view

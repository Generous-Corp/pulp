// pulp #1434 Phase A2-1 — CSS animations + transitions infrastructure.
//
// CSS-spec types — `transition` shorthand parser, easing curves
// matching the CSS Animations Module Level 1 vocabulary, the
// AnimatableProperty enum the dispatcher consults, and the
// KeyframesRegistry that holds @keyframes blocks. Sits alongside the
// existing animation.hpp (which holds Pulp's AnimationManager — used
// for native UI animations driven via the FrameClock) without
// colliding: this file is the CSS-driven side, that file is the
// host-app-driven side.
//
// Multi-PR ladder per the umbrella spec:
//   PR 1 (this PR): infrastructure + transition shorthand wiring +
//                   bridge foundation. No frame-driven playback yet.
//   PR 2: Property-typed tween functions tied into the rAF idle pump.
//   PR 3: Transition shorthand parser end-to-end + setTransition
//         bridge integration with the prop-applier.
//   PR 4: @keyframes registry + animation-name resolver.
//   PR 5: Catalog flips + end-to-end tests.

#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

/// CSS easing function — maps a normalized progress t ∈ [0,1] to an
/// eased progress p ∈ [0,1]. Pulp follows the CSS Animations Module
/// Level 1 vocabulary; advanced spec values (`steps()`, `cubic-bezier`)
/// are parsed into structured forms below and evaluated here.
struct CssEasing {
    enum class Kind {
        linear,
        ease,             // cubic-bezier(0.25, 0.1, 0.25, 1)
        ease_in,          // cubic-bezier(0.42, 0, 1, 1)
        ease_out,         // cubic-bezier(0, 0, 0.58, 1)
        ease_in_out,      // cubic-bezier(0.42, 0, 0.58, 1)
        cubic_bezier,     // custom 4-control-point bezier
        steps_start,      // steps(N, jump-start)
        steps_end,        // steps(N, jump-end)
    };

    /// CSS spec — `transition-timing-function` defaults to `ease`, NOT
    /// `linear`. Default-constructed CssEasing matches the spec so
    /// declarations like `transition: opacity 200ms` (no explicit
    /// timing token) animate with the right curve out of the box.
    /// Codex audit on pulp #1508 caught this as a P2.
    Kind kind = Kind::ease;
    /// Cubic-bezier control points. Default initializes to the `ease`
    /// curve (cubic-bezier(0.25, 0.1, 0.25, 1)) so the matrix view of
    /// `kind` and the explicit control-point view stay coherent.
    float p1x = 0.25f, p1y = 0.1f, p2x = 0.25f, p2y = 1.0f; // cubic-bezier
    int steps_count = 1;                                   // steps()

    /// Evaluate the easing at progress `t` ∈ [0,1].
    float at(float t) const {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        switch (kind) {
            case Kind::linear:
                return t;
            case Kind::ease:
                return cubic_bezier_y(t, 0.25f, 0.1f, 0.25f, 1.0f);
            case Kind::ease_in:
                return cubic_bezier_y(t, 0.42f, 0.0f, 1.0f, 1.0f);
            case Kind::ease_out:
                return cubic_bezier_y(t, 0.0f, 0.0f, 0.58f, 1.0f);
            case Kind::ease_in_out:
                return cubic_bezier_y(t, 0.42f, 0.0f, 0.58f, 1.0f);
            case Kind::cubic_bezier:
                return cubic_bezier_y(t, p1x, p1y, p2x, p2y);
            case Kind::steps_start: {
                const float step = 1.0f / static_cast<float>(steps_count);
                const float v = std::floor(t / step) * step + step;
                return v > 1.0f ? 1.0f : v;
            }
            case Kind::steps_end: {
                const float step = 1.0f / static_cast<float>(steps_count);
                return std::floor(t / step) * step;
            }
        }
        return t;
    }

    /// Evaluate a CSS cubic-bezier(p1x, p1y, p2x, p2y) at progress x.
    /// Newton-Raphson solve for x(u) → u, then evaluate y(u). 6
    /// iterations is plenty for 1/255 precision.
    static float cubic_bezier_y(float x, float p1x, float p1y, float p2x, float p2y) {
        const float cx = 3.0f * p1x;
        const float bx = 3.0f * (p2x - p1x) - cx;
        const float ax = 1.0f - cx - bx;
        const float cy = 3.0f * p1y;
        const float by = 3.0f * (p2y - p1y) - cy;
        const float ay = 1.0f - cy - by;
        float u = x;
        for (int i = 0; i < 6; ++i) {
            const float xu = ((ax * u + bx) * u + cx) * u;
            const float dxu = (3.0f * ax * u + 2.0f * bx) * u + cx;
            if (dxu == 0.0f) break;
            const float du = (xu - x) / dxu;
            u -= du;
            if (std::abs(du) < 1e-5f) break;
        }
        return ((ay * u + by) * u + cy) * u;
    }

    /// Parse a CSS easing keyword. Unrecognized → linear.
    static CssEasing from_keyword(const std::string& s) {
        CssEasing e;
        if (s == "ease")             e.kind = Kind::ease;
        else if (s == "ease-in")     e.kind = Kind::ease_in;
        else if (s == "ease-out")    e.kind = Kind::ease_out;
        else if (s == "ease-in-out") e.kind = Kind::ease_in_out;
        else                          e.kind = Kind::linear;
        return e;
    }
};

/// Property whose values can be tweened. The set is closed; PR 2 of
/// the ladder ships the per-type tween dispatchers.
enum class AnimatableProperty {
    opacity,
    background_color,
    color,
    border_color,
    translate_x,
    translate_y,
    rotate_deg,
    scale,
    width,
    height,
    margin_top,
    margin_right,
    margin_bottom,
    margin_left,
    padding_top,
    padding_right,
    padding_bottom,
    padding_left,
    top,
    right,
    bottom,
    left,
    none,
};

/// One transition spec — one CSS property with its own duration / delay
/// / easing. CSS `transition: opacity 200ms ease, transform 300ms` is
/// parsed into a vector of these.
struct TransitionSpec {
    AnimatableProperty property = AnimatableProperty::none;
    /// Original CSS property name. Preserved for the `'all'` and
    /// unrecognized cases the named enum can't represent.
    std::string property_name;
    float duration_seconds = 0.0f;
    float delay_seconds = 0.0f;
    CssEasing easing{};

    /// CSS spec — duration <= 0 means "snap" (no tween). The dispatcher
    /// takes the fast path.
    bool is_snap() const { return duration_seconds <= 0.0f; }
};

/// One running animation. Lifecycle: created when a property changes
/// and a TransitionSpec matches; ticks each frame via `tick()`;
/// finishes when `elapsed >= delay + duration`. PR 2 wires the
/// dispatcher to call setOpacity / setTranslate / etc. with the
/// tweened scalar.
struct CssAnimation {
    AnimatableProperty property = AnimatableProperty::none;
    TransitionSpec spec{};
    /// Start + end values are property-typed; PR 1 carries floats since
    /// every supported property reduces to a float scalar after
    /// component decomposition (color → 4 floats handled separately
    /// by PR 2's color-typed dispatcher).
    float start_value = 0.0f;
    float end_value = 0.0f;
    float elapsed_seconds = 0.0f;
    bool active = true;

    /// Advance by `dt` seconds. Returns the current eased value. When
    /// `active` flips to false the caller should commit `end_value`
    /// and clean up.
    float tick(float dt) {
        elapsed_seconds += dt;
        const float total = spec.delay_seconds + spec.duration_seconds;
        if (elapsed_seconds >= total) {
            active = false;
            return end_value;
        }
        if (elapsed_seconds < spec.delay_seconds) {
            return start_value;
        }
        const float local = elapsed_seconds - spec.delay_seconds;
        const float t = spec.duration_seconds > 0.0f
                        ? local / spec.duration_seconds
                        : 1.0f;
        const float p = spec.easing.at(t);
        return start_value + (end_value - start_value) * p;
    }
};

/// One @keyframe stop. PR 4 wires the value-type registry; PR 1 ships
/// the raw-string carrier so the registry is consultable today.
struct CssKeyframe {
    /// Position along the timeline: 0.0 = from, 1.0 = to. CSS percent
    /// keyframes (`50%`) are stored as 0.5.
    float offset = 0.0f;
    /// Property → value snapshots. PR 4 specializes the value type.
    std::vector<std::pair<std::string /*prop*/, std::string /*raw value*/>> properties;
};

/// One @keyframes block: name + ordered stops.
struct CssKeyframesBlock {
    std::string name;
    std::vector<CssKeyframe> stops;
};

/// Application-wide registry for @keyframes blocks. WidgetBridge owns
/// one; lookups happen by name during animation-name resolution.
struct CssKeyframesRegistry {
    std::vector<CssKeyframesBlock> blocks;

    void define(CssKeyframesBlock block) {
        for (auto& b : blocks) {
            if (b.name == block.name) {
                b = std::move(block);
                return;
            }
        }
        blocks.push_back(std::move(block));
    }

    const CssKeyframesBlock* find(const std::string& name) const {
        for (const auto& b : blocks) {
            if (b.name == name) return &b;
        }
        return nullptr;
    }
};

/// Map CSS property name to AnimatableProperty enum. Returns
/// `AnimatableProperty::none` for unrecognized names; the caller
/// should preserve the original string in `TransitionSpec::property_name`
/// for later round-trip.
inline AnimatableProperty animatable_property_from_css_name(const std::string& name) {
    if (name == "opacity")            return AnimatableProperty::opacity;
    if (name == "background-color")   return AnimatableProperty::background_color;
    if (name == "color")              return AnimatableProperty::color;
    if (name == "border-color")       return AnimatableProperty::border_color;
    if (name == "translate-x")        return AnimatableProperty::translate_x;
    if (name == "translate-y")        return AnimatableProperty::translate_y;
    if (name == "rotate")             return AnimatableProperty::rotate_deg;
    if (name == "scale")              return AnimatableProperty::scale;
    if (name == "width")              return AnimatableProperty::width;
    if (name == "height")             return AnimatableProperty::height;
    if (name == "margin-top")         return AnimatableProperty::margin_top;
    if (name == "margin-right")       return AnimatableProperty::margin_right;
    if (name == "margin-bottom")      return AnimatableProperty::margin_bottom;
    if (name == "margin-left")        return AnimatableProperty::margin_left;
    if (name == "padding-top")        return AnimatableProperty::padding_top;
    if (name == "padding-right")      return AnimatableProperty::padding_right;
    if (name == "padding-bottom")     return AnimatableProperty::padding_bottom;
    if (name == "padding-left")       return AnimatableProperty::padding_left;
    if (name == "top")                return AnimatableProperty::top;
    if (name == "right")              return AnimatableProperty::right;
    if (name == "bottom")             return AnimatableProperty::bottom;
    if (name == "left")               return AnimatableProperty::left;
    return AnimatableProperty::none;
}

/// Parse a CSS time string (`200ms` / `0.3s` / bare seconds) into
/// seconds. Returns 0 on parse failure.
inline float parse_css_time_seconds(const std::string& s) {
    if (s.empty()) return 0.0f;
    try {
        size_t pos = 0;
        float v = std::stof(s, &pos);
        std::string unit = s.substr(pos);
        while (!unit.empty() && std::isspace(static_cast<unsigned char>(unit.front()))) unit.erase(0, 1);
        while (!unit.empty() && std::isspace(static_cast<unsigned char>(unit.back()))) unit.pop_back();
        if (unit == "ms") return v / 1000.0f;
        if (unit == "s")  return v;
        return v; // bare number → seconds (lenient)
    } catch (...) {
        return 0.0f;
    }
}

/// Parse a CSS `transition` shorthand into a list of TransitionSpecs.
///
///   transition: opacity 200ms ease, transform 300ms ease-in 100ms
///
/// Per the CSS spec, each comma-separated entry has up to four tokens:
/// property (default `'all'`), duration (default 0s), timing-function
/// (default ease), delay (default 0s). The first time-valued token is
/// duration; the second is delay. Cubic-bezier(...) and steps(...) are
/// parsed into the structured `CssEasing` form.
inline std::vector<TransitionSpec> parse_transition_shorthand(const std::string& css) {
    std::vector<TransitionSpec> out;
    if (css.empty() || css == "none") return out;

    // Split on commas at depth 0 (cubic-bezier(...) has commas inside
    // parens; need to skip those).
    std::vector<std::string> entries;
    {
        std::string acc;
        int depth = 0;
        for (char c : css) {
            if (c == '(') ++depth;
            else if (c == ')') --depth;
            if (c == ',' && depth == 0) {
                entries.push_back(acc);
                acc.clear();
            } else {
                acc += c;
            }
        }
        if (!acc.empty()) entries.push_back(acc);
    }

    for (const auto& entry : entries) {
        // Tokenize on whitespace (preserving cubic-bezier(...) and
        // steps(...) parens).
        std::vector<std::string> tokens;
        {
            std::string acc;
            int depth = 0;
            for (char c : entry) {
                if (c == '(') { ++depth; acc += c; continue; }
                if (c == ')') { --depth; acc += c; continue; }
                if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
                    if (!acc.empty()) { tokens.push_back(acc); acc.clear(); }
                    continue;
                }
                acc += c;
            }
            if (!acc.empty()) tokens.push_back(acc);
        }

        if (tokens.empty()) continue;

        TransitionSpec spec{};
        bool saw_duration = false;
        for (const auto& tok : tokens) {
            // Time tokens — `200ms` or `0.3s` (but NOT `ease`).
            const bool ends_ms = (tok.size() >= 2 && tok.substr(tok.size() - 2) == "ms");
            const bool ends_s_only = (!ends_ms && tok.size() >= 2 && tok.back() == 's'
                                      && (std::isdigit(static_cast<unsigned char>(tok[tok.size() - 2]))
                                          || tok[tok.size() - 2] == '.'));
            if (ends_ms || ends_s_only) {
                if (!saw_duration) { spec.duration_seconds = parse_css_time_seconds(tok); saw_duration = true; }
                else                spec.delay_seconds = parse_css_time_seconds(tok);
                continue;
            }
            // Easing keyword
            if (tok == "linear" || tok == "ease" || tok == "ease-in"
                || tok == "ease-out" || tok == "ease-in-out") {
                spec.easing = CssEasing::from_keyword(tok);
                continue;
            }
            // cubic-bezier(p1x, p1y, p2x, p2y)
            if (tok.rfind("cubic-bezier(", 0) == 0 && tok.back() == ')') {
                spec.easing.kind = CssEasing::Kind::cubic_bezier;
                std::string inner = tok.substr(13, tok.size() - 14);
                std::vector<float> nums;
                std::string acc;
                auto flush = [&]() {
                    if (!acc.empty()) {
                        try { nums.push_back(std::stof(acc)); } catch (...) {}
                        acc.clear();
                    }
                };
                for (char c : inner) {
                    if (c == ',') flush();
                    else if (!std::isspace(static_cast<unsigned char>(c))) acc += c;
                }
                flush();
                if (nums.size() == 4) {
                    spec.easing.p1x = nums[0]; spec.easing.p1y = nums[1];
                    spec.easing.p2x = nums[2]; spec.easing.p2y = nums[3];
                }
                continue;
            }
            // steps(N) / steps(N, end|start)
            if (tok.rfind("steps(", 0) == 0 && tok.back() == ')') {
                std::string inner = tok.substr(6, tok.size() - 7);
                size_t comma = inner.find(',');
                std::string n_str = comma == std::string::npos ? inner : inner.substr(0, comma);
                try { spec.easing.steps_count = std::stoi(n_str); } catch (...) {}
                spec.easing.kind = CssEasing::Kind::steps_end;
                if (comma != std::string::npos) {
                    std::string mode = inner.substr(comma + 1);
                    while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.front()))) mode.erase(0, 1);
                    if (mode.rfind("jump-start", 0) == 0 || mode == "start") {
                        spec.easing.kind = CssEasing::Kind::steps_start;
                    }
                }
                continue;
            }
            // Otherwise — property name. Last property name wins (CSS
            // spec: the last unparsed token is the property).
            spec.property_name = tok;
            spec.property = animatable_property_from_css_name(tok);
        }
        if (spec.property_name.empty()) spec.property_name = "all";
        out.push_back(std::move(spec));
    }
    return out;
}

} // namespace pulp::view

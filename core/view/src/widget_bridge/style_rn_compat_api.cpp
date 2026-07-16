// widget_bridge/style_rn_compat_api.cpp - RN/CSS compatibility style registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

void WidgetBridge::register_widget_style_rn_compat_api() {
    BridgeApiContext api{engine_};

    // RN iOS-legacy shadow{Color,Offset,Opacity,Radius} per-attribute setters for
    // box-shadow. Mirrors the textShadow* pattern above so a JSX prop
    // diff that touches one prop doesn't clobber the others. Modern
    // RN code uses `boxShadow` (CSS shorthand) — Pulp fully supports
    // that via setBoxShadow — but the per-attribute API is still in
    // upstream RN's surface, especially for code carrying iOS-legacy
    // styling. Each setter mutates one field of View::shadow_ and
    // flips has_shadow_ on, mirroring how text-shadow longhand works.
    register_bridge_function(api, "setShadowColor",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto hex = args.get<std::string>(1, "");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_box_shadow_color(parse_bridge_css_color(hex));
            return choc::value::Value();
        });
    register_bridge_function(api, "setShadowOffset",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto dx = static_cast<float>(args.get<double>(1, 0.0));
            auto dy = static_cast<float>(args.get<double>(2, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_box_shadow_offset(dx, dy);
            return choc::value::Value();
        });
    register_bridge_function(api, "setShadowOpacity",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto a  = static_cast<float>(args.get<double>(1, 1.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_box_shadow_opacity(a);
            return choc::value::Value();
        });
    register_bridge_function(api, "setShadowRadius",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto r  = static_cast<float>(args.get<double>(1, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_box_shadow_radius(r);
            return choc::value::Value();
        });

    // RN's `includeFontPadding` is an Android-only legacy prop. Android's
    // TextView paints extra vertical padding around text glyphs;
    // setting `includeFontPadding: false` removes it. Pulp's text-
    // shaping pipeline (Skia/SkParagraph) uses tight baseline-relative
    // positioning and DOESN'T add Android-style vestigial padding, so
    // Pulp's default behavior already matches the `false` outcome that
    // most authors want from this prop.
    //
    // Setting `true` is a silent no-op: Pulp can't add padding it
    // doesn't have without restructuring text shaping. Setting `false`
    // matches Pulp's existing default. Either way, the visible result
    // is the same — tight glyph layout with no Android-vestigial padding.
    //
    // Bridge fn stores the keyword on a View slot purely for round-
    // trip (element.style / style.X reading the value back). This
    // mirrors the overscroll-behavior pattern: all keywords accepted,
    // single consistent behavior produced, and the catalog can report a
    // CSS-spec subset where Pulp's behavior matches the dominant author intent.
    register_bridge_function(api, "setIncludeFontPadding",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto v  = id.empty() ? &root_ : widget(id);
            // Accept the value, store it, no paint impact — Pulp's
            // text shaping doesn't add Android-vestigial padding
            // regardless of this flag.
            if (v) v->set_include_font_padding(args.get<bool>(1, true));
            return choc::value::Value();
        });

    // RN's `borderCurve` corner shape.
    // `circular` (default) keeps the standard quarter-circle rounded
    // corner; `continuous` switches View::paint_all to the iOS-style
    // squircle approximation (super-ellipse path with extension factor
    // 1.528 and flatter kappa 0.85). Visible difference on large-radius
    // cards (24px+); subtle below 12px. See view.cpp's
    // build_continuous_corner_rounded_rect_path for the path math.
    register_bridge_function(api, "setBorderCurve",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "circular");
            auto* v = id.empty() ? &root_ : widget(id);
            if (!v) return choc::value::Value();
            v->set_border_curve(kw == "continuous"
                                ? View::BorderCurve::continuous
                                : View::BorderCurve::circular);
            return choc::value::Value();
        });

    // CSS `isolation` + RN `isolation` subset. Pulp's per-View render model
    // is structurally isolated by default: each View with mix-blend-mode
    // opens its own save_layer_with_blend composition and composites
    // back to its parent normally — there's no
    // "cross-stacking-context blend leakage" that CSS isolation: isolate
    // is designed to prevent. Similarly, z-index is paint-order scoped
    // to siblings within a parent, so a child's z-index can't promote
    // past the parent in z-order. Both author intents of `isolation:
    // isolate` (blend-mode containment + stacking-context creation)
    // happen by default in Pulp.
    //
    // Bridge fn stores the keyword on View::isolation_ for round-trip
    // reads (el.style.isolation === "isolate"). Paint has no special
    // case because Pulp's existing per-View layering already provides
    // the isolation contract. Same CSS-subset pattern as
    // overscrollBehavior, includeFontPadding, and scrollBehavior.
    register_bridge_function(api, "setIsolation",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "auto");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_isolation(kw);
            return choc::value::Value();
        });

    // RN's `elevation` is Android-only Material elevation (0–24dp). Pulp catalogs boxShadow
    // as the cross-platform equivalent; this shim translates elevation
    // to a single-shadow approximation of the Material dual-shadow
    // spec so consumers shipping unchanged RN-Android styles get a
    // visible shadow on every Pulp platform.
    //
    // Approximation formula (Material Design system, simplified to
    // Pulp's single-shadow BoxShadow):
    //   elevation=0 -> clear box-shadow (no shadow)
    //   elevation N -> offset_y = max(1, N/2)
    //                  blur     = N + 1        (slightly larger than dp)
    //                  spread   = 0
    //                  color    = rgba(0, 0, 0, clamp(0.15+N*0.01, 0.15, 0.30))
    //
    // The blur ≈ elevation+1 and offset_y ≈ elevation/2 are the same
    // ratios Material's `mat-elevation-z*` mixin uses. The alpha ramp
    // matches Material's umbra-shadow opacity curve well enough to be
    // recognizable; the catalog notes call out the single-shadow
    // approximation honestly.
    register_bridge_function(api, "setElevation",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto e  = static_cast<float>(args.get<double>(1, 0.0));
            auto* v = id.empty() ? &root_ : widget(id);
            if (!v) return choc::value::Value();
            if (e <= 0.0f) {
                v->clear_box_shadow();
            } else {
                const float offset_y = std::max(1.0f, e * 0.5f);
                const float blur     = e + 1.0f;
                const float alpha    = std::min(0.30f, std::max(0.15f, 0.15f + e * 0.01f));
                v->set_box_shadow(0.0f, offset_y, blur, 0.0f,
                                  canvas::Color::rgba(0.0f, 0.0f, 0.0f, alpha),
                                  /*inset=*/false);
            }
            return choc::value::Value();
        });

    // CSS scroll-behavior + overscroll-behavior. Stored on the View slot; ScrollView reads
    // scroll_behavior_ in scroll_by (auto → instant, else smooth) and
    // overscroll_behavior_ via the existing clamp_scroll_targets path
    // (Pulp already clamps at content bounds and doesn't scroll-chain
    // to parents, so all three keywords [auto/contain/none] behave as
    // CSS `contain` — a valid subset of the spec).
    register_bridge_function(api, "setScrollBehavior",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "smooth");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_scroll_behavior(kw);
            return choc::value::Value();
        });
    register_bridge_function(api, "setOverscrollBehavior",
        [this](choc::javascript::ArgumentList args) {
            auto id = args.get<std::string>(0, "");
            auto kw = args.get<std::string>(1, "auto");
            auto* v = id.empty() ? &root_ : widget(id);
            if (v) v->set_overscroll_behavior(kw);
            return choc::value::Value();
        });
}

} // namespace pulp::view

#include "test_design_import_shared.hpp"
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/view/pointer_dispatch.hpp>

// Paint lowering: fades, blur, corner radii, shadows, gradients, borders,
// and slider fill reconnection.

namespace {

// A node of every kind that carries the same fade + shadow, so a branch that
// drops one is visible as a missing setOpacity for that id.
DesignIR ir_with_faded_node_of_every_kind() {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;

    auto fade = [](IRNode& n) {
        n.style.opacity = 0.5f;
        n.style.box_shadow = parse_css_box_shadow("0 2px 8px #00000080");
        n.style.mix_blend_mode = "multiply";
        n.style.filter = "blur(3px)";
        n.style.backdrop_filter = "blur(9px)";
        // Asymmetric on purpose: a uniform radius lowers through the single
        // 'All' call and would hide a missing per-corner emit.
        n.style.border_top_left_radius = 8.0f;
        n.style.border_top_right_radius = 8.0f;
        n.style.border_bottom_right_radius = 0.0f;
        n.style.border_bottom_left_radius = 0.0f;
    };

    IRNode text;
    text.type = "text";
    text.name = "Caption";
    text.text_content = "Ghosted";
    text.style.font_size = 12.0f;
    fade(text);
    ir.root.children.push_back(text);

    IRNode image;
    image.type = "image";
    image.name = "Overlay";
    image.attributes["asset_path"] = "/tmp/grain.png";
    image.style.width = 64.0f;
    image.style.height = 64.0f;
    fade(image);
    ir.root.children.push_back(image);

    IRNode knob;
    knob.type = "frame";
    knob.name = "Cutoff";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 48.0f;
    knob.style.height = 48.0f;
    fade(knob);
    ir.root.children.push_back(knob);

    IRNode container;
    container.type = "frame";
    container.name = "Group";
    container.style.width = 100.0f;
    container.style.height = 40.0f;
    IRNode inner;
    inner.type = "text";
    inner.text_content = "x";
    container.children.push_back(inner);
    fade(container);
    ir.root.children.push_back(container);

    return ir;
}

std::string native_js(const DesignIR& ir) {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    return generate_pulp_js(ir, opts);
}

// Count non-overlapping occurrences — a property emitted from BOTH a branch and
// the shared lambda writes the line twice, which is how the setBoxShadow
// duplicate survived review.
size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t p = haystack.find(needle); p != std::string::npos;
         p = haystack.find(needle, p + needle.size()))
        ++n;
    return n;
}

} // namespace

TEST_CASE("native codegen fades text, image, widget and container alike",
          "[view][import][visual-overrides]") {
    const auto js = native_js(ir_with_faded_node_of_every_kind());

    // Four faded nodes in, four setOpacity calls out. A branch-local emit fails
    // this by count, so it cannot be satisfied by fixing only one kind.
    REQUIRE(count_occurrences(js, "setOpacity(") == 4);
    REQUIRE(count_occurrences(js, "setBoxShadow(") == 4);
    REQUIRE(count_occurrences(js, "setMixBlendMode(") == 4);
    REQUIRE(count_occurrences(js, "setFilter(") == 4);
    REQUIRE(count_occurrences(js, "setBackdropFilter(") == 4);

    // Four nodes with four distinct corners each. Codegen carried one
    // setCornerRadius(id, 'All', r) and dropped asymmetric corners on the
    // floor, under a comment claiming the IR could not hold them — it could.
    REQUIRE(count_occurrences(js, "setCornerRadius(") == 16);
    REQUIRE(count_occurrences(js, "'TopLeft', 8") == 4);
    REQUIRE(count_occurrences(js, "'BottomRight', 0") == 4);
    // The uniform path must not also fire, or 'All' squares the rounded pair.
    REQUIRE(js.find("'All'") == std::string::npos);
}

TEST_CASE(
    "native codegen lowers a layer blur to setFilter and a background blur to setBackdropFilter",
    "[view][import][visual-overrides]") {
    // A Figma LAYER_BLUR reaches the IR as `filter: blur(Npx)` and a
    // BACKGROUND_BLUR as `backdrop_filter: blur(Npx)` (all three producers).
    // The bridge's setFilter takes the CSS string (it walks the function
    // sequence into a View::FilterOp chain); setBackdropFilter is numeric, so
    // codegen parses the radius out of the CSS value.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 100.0f;
    ir.root.style.filter = "blur(4px)";

    IRNode frosted;
    frosted.type = "frame";
    frosted.name = "Frosted";
    frosted.style.width = 80.0f;
    frosted.style.height = 40.0f;
    frosted.style.backdrop_filter = "blur(12px)";
    ir.root.children.push_back(frosted);

    const auto js = native_js(ir);
    REQUIRE(js.find("setFilter('root', 'blur(4px)')") != std::string::npos);
    REQUIRE(count_occurrences(js, "setFilter(") == 1);
    // The numeric bridge form: the radius, parsed out of blur(12px).
    const auto bdf = js.find("setBackdropFilter('");
    REQUIRE(bdf != std::string::npos);
    const auto bdf_line = js.substr(bdf, js.find('\n', bdf) - bdf);
    REQUIRE(bdf_line.find(", 12)") != std::string::npos);
    REQUIRE(count_occurrences(js, "setBackdropFilter(") == 1);
}

TEST_CASE("native codegen keeps one call when every corner agrees",
          "[view][import][visual-overrides]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;
    ir.root.style.border_radius = 6.0f;

    const auto js = native_js(ir);
    REQUIRE(count_occurrences(js, "setCornerRadius(") == 1);
    REQUIRE(js.find("'All', 6") != std::string::npos);
}

TEST_CASE("native codegen clips a container that declares overflow and leaves the default open",
          "[view][import][visual-overrides]") {
    // `overflow: clip` is how a decoder says "Figma clips this container's
    // content" — a component master whose decoration overhangs its bounds
    // renders clipped in Figma, so dropping the key painted the overhang over
    // whatever sat below the instance. `visible` is the View default and must
    // NOT emit a call: an explicit setOverflow('visible') would be noise on
    // nearly every node.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Card";
    ir.root.style.width = 60.0f;
    ir.root.style.height = 235.0f;
    ir.root.style.overflow = "clip";
    IRNode open;
    open.type = "frame";
    open.name = "Open";
    open.style.width = 20.0f;
    open.style.height = 20.0f;
    open.style.overflow = "visible";
    ir.root.children.push_back(open);

    const auto js = native_js(ir);
    REQUIRE(count_occurrences(js, "setOverflow(") == 1);
    REQUIRE(js.find("setOverflow('root', 'clip')") != std::string::npos);
}

TEST_CASE("native codegen fades a text node — the branch that had no setOpacity",
          "[view][import][visual-overrides]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode text;
    text.type = "text";
    text.name = "Caption";
    text.text_content = "Ghosted";
    text.style.font_size = 12.0f;
    text.style.opacity = 0.35f;
    ir.root.children.push_back(text);

    const auto js = native_js(ir);

    // The label's own id must be the one faded — not its parent.
    const auto create = js.find("createLabel('");
    REQUIRE(create != std::string::npos);
    const auto id_start = create + std::string("createLabel('").size();
    const auto label_id = js.substr(id_start, js.find('\'', id_start) - id_start);
    REQUIRE(js.find("setOpacity('" + label_id + "', 0.35)") != std::string::npos);
}

TEST_CASE("native codegen emits an audio widget's shadow, blend and opacity",
          "[view][import][visual-overrides]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode knob;
    knob.type = "frame";
    knob.name = "Cutoff";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 48.0f;
    knob.style.height = 48.0f;
    knob.style.opacity = 0.3f; // designer's "disabled" fade
    knob.style.box_shadow = parse_css_box_shadow("0 2px 8px #00000080");
    ir.root.children.push_back(knob);

    const auto js = native_js(ir);

    REQUIRE(js.find("createKnob('") != std::string::npos);
    REQUIRE(js.find("setOpacity('") != std::string::npos);
    REQUIRE(js.find("setBoxShadow('") != std::string::npos);
}

TEST_CASE("native codegen emits every box-shadow layer, in CSS author order",
          "[view][import][visual-overrides]") {
    // The exact declaration a Figma knob base carries: a soft 10%-black halo
    // that spreads the glow, plus a tight 25%-black contact shadow that seats
    // the knob on the panel. Codegen used to emit only the first layer, which
    // kept the halo and dropped the depth.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode knob;
    knob.type = "frame";
    knob.name = "Cutoff";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 48.0f;
    knob.style.height = 48.0f;
    knob.style.box_shadow =
        parse_css_box_shadow("0px 16px 6px 0px #0000001a, 0px 4px 4px 0px #00000040");
    REQUIRE(knob.style.box_shadow.size() == 2); // the parser already keeps both
    ir.root.children.push_back(knob);

    const auto js = native_js(ir);

    // Two declared layers, two emitted calls: the first replaces, the rest append.
    REQUIRE(count_occurrences(js, "setBoxShadow(") == 1);
    REQUIRE(count_occurrences(js, "addBoxShadow(") == 1);

    const auto set_at = js.find("setBoxShadow(");
    const auto add_at = js.find("addBoxShadow(");
    REQUIRE(set_at != std::string::npos);
    REQUIRE(add_at != std::string::npos);
    // CSS author order must survive: the halo is declared first, the contact
    // shadow second. Swapping them paints the halo over the contact shadow.
    REQUIRE(set_at < add_at);

    const auto set_line = js.substr(set_at, js.find('\n', set_at) - set_at);
    const auto add_line = js.substr(add_at, js.find('\n', add_at) - add_at);
    REQUIRE(set_line.find("0, 16, 6, 0") != std::string::npos);
    REQUIRE(set_line.find("#0000001a") != std::string::npos);
    REQUIRE(add_line.find("0, 4, 4, 0") != std::string::npos);
    REQUIRE(add_line.find("#00000040") != std::string::npos);
}

TEST_CASE("native codegen leaves a single-layer shadow as one replacing call",
          "[view][import][visual-overrides]") {
    // Guards the other half of the contract: one declared layer must not grow
    // an addBoxShadow, or every existing single-shadow design gains a
    // duplicate layer and doubles its darkness.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;
    ir.root.style.box_shadow = parse_css_box_shadow("0 2px 8px #00000080");

    const auto js = native_js(ir);
    REQUIRE(count_occurrences(js, "setBoxShadow(") == 1);
    REQUIRE(count_occurrences(js, "addBoxShadow(") == 0);
}

TEST_CASE("native codegen positions an unlabeled non-knob widget absolutely",
          "[view][import][visual-overrides]") {
    // Only the knob sub-branch emitted the position, so every other widget kind
    // lost it. Fader is the canonical miss: a channel-strip fader pinned at an
    // absolute offset drifted to wherever flex put it.
    for (auto wtype : {AudioWidgetType::fader, AudioWidgetType::meter, AudioWidgetType::xy_pad,
                       AudioWidgetType::waveform}) {
        DesignIR ir;
        ir.source = DesignSource::figma;
        ir.root.type = "frame";
        ir.root.style.width = 400.0f;

        IRNode w;
        w.type = "frame";
        w.audio_widget = wtype; // no label/value/range → no wrapper column
        w.style.width = 40.0f;
        w.style.height = 120.0f;
        w.style.position = "absolute";
        w.style.left = 72.0f;
        w.style.top = 24.0f;
        ir.root.children.push_back(w);

        const auto js = native_js(ir);

        INFO("widget kind index " << static_cast<int>(wtype) << " js:\n" << js);
        REQUIRE(js.find("setPosition('") != std::string::npos);
        REQUIRE(js.find("', 72)") != std::string::npos); // setLeft
        REQUIRE(js.find("', 24)") != std::string::npos); // setTop
    }
}

TEST_CASE("native codegen paints a childless non-frame node's gradient and fade",
          "[view][import][visual-overrides]") {
    // The fall-through branch. Reachable for real input: the v0 lane maps void
    // tags (<input>, <canvas>, …) to childless non-frame nodes, and a
    // gradient-filled <rect> lands here because synthesize_node declines to
    // path-ify it — on the stated grounds that the frame paints its own box.
    auto ir = parse_v0_tsx(R"tsx(
        export default function P() {
            return (
                <div style={{ display: "flex" }}>
                    <input style={{
                        background: "linear-gradient(90deg, #0f0, #00f)",
                        opacity: 0.25,
                        borderRadius: 6,
                        width: 80,
                        height: 20
                    }} />
                </div>
            );
        }
    )tsx");

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("setBackgroundGradient('input0', 'linear-gradient(90deg, #0f0, #00f)')") !=
            std::string::npos);
    REQUIRE(js.find("setOpacity('input0', 0.25)") != std::string::npos);
    REQUIRE(js.find("setCornerRadius('input0', 'All', 6)") != std::string::npos);
}

TEST_CASE("native codegen paints a text node's background gradient",
          "[view][import][visual-overrides]") {
    // v0 maps `background: linear-gradient(...)` on any inline tag, and <span>
    // lowers to a text node — the gradient plate behind a heading was dropped.
    auto ir = parse_v0_tsx(R"tsx(
        export default function P() {
            return (
                <div style={{ display: "flex" }}>
                    <span style={{ background: "linear-gradient(90deg, #f00, #00f)" }}>Hi</span>
                </div>
            );
        }
    )tsx");

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("setBackgroundGradient('span0', 'linear-gradient(90deg, #f00, #00f)')") !=
            std::string::npos);
}

TEST_CASE("native codegen never emits a gradient box behind an SVG path",
          "[view][import][visual-overrides]") {
    // The counter-example that keeps setBackgroundGradient OUT of the shared
    // visual-override lambda: a gradient on a node that lowers to an
    // SvgPathWidget would paint a gradient SQUARE behind the path — the stray
    // box behind a knob that synthesize_node exists to prevent.
    //
    // The node must carry AUTHORED path_data. A bare primitive does not prove
    // anything here: synthesize_node moves its gradient onto the synthesized
    // path and RESETS style.background_gradient, so the lambda would find
    // nothing to emit and this test would pass with the bug present — it did,
    // until the fixture was fixed. An authored path takes synthesize_node's
    // early return, so the style gradient survives to codegen and the branch is
    // the only thing standing between it and the box.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode glyph;
    glyph.type = "path";
    glyph.name = "Play";
    glyph.attributes["path_data"] = "M0 0 L32 16 L0 32 Z";
    glyph.attributes["svg_viewbox"] = "0 0 32 32";
    glyph.attributes["svg_fill_gradient"] = "linear-gradient(90deg, #f00, #900)";
    glyph.style.width = 32.0f;
    glyph.style.height = 32.0f;
    glyph.style.background_gradient = "linear-gradient(90deg, #f00, #900)";
    glyph.style.opacity = 0.6f;
    ir.root.children.push_back(glyph);

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("createSvgPath('") != std::string::npos);
    // The gradient rides the path, and ONLY the path.
    REQUIRE(js.find("setSvgFillGradient('") != std::string::npos);
    REQUIRE(js.find("setBackgroundGradient(") == std::string::npos);
    // The fade still rides along — opacity is a View property, unlike the fill.
    REQUIRE(js.find("setOpacity('") != std::string::npos);
}

TEST_CASE("native codegen keeps a synthesized primitive's gradient off its box",
          "[view][import][visual-overrides]") {
    // The other half of the same invariant, via the path synthesize_node DOES
    // take: a filled ellipse gets its gradient moved onto the synthesized path,
    // cleared off its own style, and must not paint a box either.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode dot;
    dot.type = "ellipse";
    dot.name = "Record";
    dot.style.width = 32.0f;
    dot.style.height = 32.0f;
    dot.style.background_gradient = "linear-gradient(90deg, #f00, #900)";
    dot.style.opacity = 0.6f;
    ir.root.children.push_back(dot);

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("createSvgPath('") != std::string::npos);
    REQUIRE(js.find("setSvgFillGradient('") != std::string::npos);
    REQUIRE(js.find("setBackgroundGradient(") == std::string::npos);
    REQUIRE(js.find("setOpacity('") != std::string::npos);
}

TEST_CASE("a synthesized ellipse carries its gradient stroke onto the path",
          "[view][import][stroke-gradient]") {
    // The knob-base rim: an ELLIPSE with a fill and a GRADIENT_LINEAR stroke.
    // The decoder lowers the stroke to strokeGradient/strokeWidth (captured as
    // svg_stroke_gradient / svg_stroke_width even though the node has no path
    // yet), synthesize_primitive_paths grows the path, and codegen must emit
    // the stroke pair beside the fill — this stroke used to be dropped with
    // "no solid paint to flatten to".
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;

    IRNode base;
    base.type = "ellipse";
    base.name = "Base";
    base.style.width = 22.0f;
    base.style.height = 22.0f;
    base.style.background_color = "#2a2b2dcc";
    base.attributes["svg_stroke_gradient"] =
        "linear-gradient(180deg, #ffffff40 0%, #31313140 100%)";
    base.attributes["svg_stroke_width"] = "0.94";
    ir.root.children.push_back(base);

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("createSvgPath('") != std::string::npos);
    REQUIRE(js.find("setSvgFill('") != std::string::npos);
    REQUIRE(js.find("setSvgStrokeGradient('") != std::string::npos);
    REQUIRE(js.find("linear-gradient(180deg, #ffffff40 0%, #31313140 100%)") != std::string::npos);
    REQUIRE(js.find("setSvgStrokeWidth(") != std::string::npos);
}

TEST_CASE("native codegen paints a gradient behind a transparent image",
          "[view][import][visual-overrides]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;

    // The canonical case: a transparent PNG over a gradient plate. The image
    // node owns BOTH, and the gradient paints the box behind the bitmap — so a
    // codegen that emits the image and forgets the box loses the plate and the
    // art lands on nothing.
    IRNode image;
    image.type = "image";
    image.name = "Grain";
    image.attributes["asset_path"] = "/tmp/grain.png";
    image.style.width = 64.0f;
    image.style.height = 64.0f;
    image.style.background_gradient = "linear-gradient(180deg, #ffffff, #000000)";
    ir.root.children.push_back(image);

    const auto js = native_js(ir);
    REQUIRE(js.find("setBackgroundGradient('") != std::string::npos);
    REQUIRE(js.find("linear-gradient(180deg, #ffffff, #000000)") != std::string::npos);
    // Both, not either: the plate is behind the bitmap, not instead of it.
    REQUIRE(js.find("setImageSource(") != std::string::npos);
}

TEST_CASE("native codegen lowers the Figma paint-stack fields", "[view][import][paints]") {
    // Audit item 7: paint-level opacity rides in the emitted rgba color; image
    // scale modes ride in object-fit (image nodes, honored by ImageView::paint)
    // or background-size/background-repeat (frame-shaped nodes); a solid plate
    // below an image fill paints BEHIND the bitmap.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;

    // A 50%-opacity solid fill: the producer folds paint opacity into the
    // color's alpha, and the codegen must pass the rgba through verbatim.
    IRNode chip;
    chip.type = "frame";
    chip.name = "Half";
    chip.style.width = 40.0f;
    chip.style.height = 40.0f;
    chip.style.background_color = "rgba(0, 0, 0, 0.500)";
    ir.root.children.push_back(chip);

    // An image fill in FIT mode over a solid plate (Figma stack
    // [SOLID, IMAGE]): contain keyword + the plate behind the bitmap.
    IRNode photo;
    photo.type = "image";
    photo.name = "Fit Photo";
    photo.attributes["asset_path"] = "/tmp/photo.png";
    photo.style.width = 64.0f;
    photo.style.height = 64.0f;
    photo.style.object_fit = "contain";
    photo.style.background_color = "#112233";
    ir.root.children.push_back(photo);

    // A TILE'd texture on a frame-shaped node (REST lane shape): the scale
    // mode lands in the View's background repeat/size slots.
    IRNode tiled;
    tiled.type = "frame";
    tiled.name = "Texture";
    tiled.style.width = 80.0f;
    tiled.style.height = 80.0f;
    tiled.style.background_image = "url(assets/noise.png)";
    tiled.style.background_repeat = "repeat";
    tiled.style.background_size = "auto";
    ir.root.children.push_back(tiled);

    const auto js = native_js(ir);
    INFO(js);

    REQUIRE(js.find("'rgba(0, 0, 0, 0.500)'") != std::string::npos);
    REQUIRE(js.find("setObjectFit(") != std::string::npos);
    REQUIRE(js.find("'contain'") != std::string::npos);
    // Both, not either: the plate is behind the bitmap, not instead of it.
    REQUIRE(js.find("setImageSource(") != std::string::npos);
    REQUIRE(js.find("'#112233'") != std::string::npos);
    REQUIRE(js.find("setBackgroundRepeat(") != std::string::npos);
    REQUIRE(js.find("'repeat'") != std::string::npos);
    REQUIRE(js.find("setBackgroundSize(") != std::string::npos);
    REQUIRE(js.find("'auto'") != std::string::npos);

    // The C++ codegen mirrors the same slots.
    const auto cpp = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(cpp.source.find("set_object_fit(\"contain\")") != std::string::npos);
    REQUIRE(cpp.source.find("set_background_repeat(\"repeat\")") != std::string::npos);
    REQUIRE(cpp.source.find("set_background_size(\"auto\")") != std::string::npos);
}

TEST_CASE("native codegen paints a stroke declared as the CSS border shorthand",
          "[view][import][border]") {
    // IRStyle carries `border` — "1px solid #333" — AND the discrete
    // border_color / border_width. Every producer writes the shorthand — the
    // .fig decoder, the Claude bundle reader, the v0 TSX reader — and every
    // native consumer reads only the parts. So a stroke reached the IR and then
    // went nowhere: a real 1115-node import declared six strokes and emitted
    // zero setBorder and zero setSvgStroke calls.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;

    IRNode card;
    card.type = "frame";
    card.name = "Card";
    card.style.width = 100.0f;
    card.style.height = 40.0f;
    card.style.border = "1px solid #f56161";
    ir.root.children.push_back(card);

    // A vector that arrives WITH its own path: synthesize_node returns early on
    // path_data, so nothing moves the stroke onto the path. This is every `Oval`
    // knob rim in a real design.
    IRNode ring;
    ring.type = "vector";
    ring.name = "Ring";
    ring.attributes["path_data"] = "M0 0 L10 0 L10 10 Z";
    ring.style.width = 24.0f;
    ring.style.height = 24.0f;
    ring.style.border = "2px solid #ffffff8c";
    ir.root.children.push_back(ring);

    const auto js = native_js(ir);

    REQUIRE(js.find("setBorder('") != std::string::npos);
    REQUIRE(js.find("#f56161") != std::string::npos);

    REQUIRE(js.find("setSvgStroke('") != std::string::npos);
    REQUIRE(js.find("#ffffff8c") != std::string::npos);
    // The weight travels with the color: a stroke emitted without it paints at
    // the widget default, which is wrong in a way that looks deliberate.
    REQUIRE(js.find("setSvgStrokeWidth('") != std::string::npos);
}

namespace {

// Build the TRIAZ slider triplet: a short wide container with a full-width dark
// track, a shorter colored progress fill, and a round thumb. Callers tweak the
// fill/thumb geometry per case.
IRNode make_slider(float fill_x, float fill_w, float thumb_x) {
    IRNode container;
    container.type = "frame";
    container.style.width = 60.0f;
    container.style.height = 8.0f;

    IRNode track;
    track.type = "frame";
    track.style.left = 0.0f;
    track.style.top = 3.0f;
    track.style.width = 60.0f;
    track.style.height = 2.0f;
    track.style.background_color = "#00000059";

    IRNode fill;
    fill.type = "frame";
    fill.style.left = fill_x;
    fill.style.top = 3.0f;
    fill.style.width = fill_w;
    fill.style.height = 2.0f;
    fill.style.background_color = "#f56161d9";

    IRNode thumb;
    thumb.type = "ellipse";
    thumb.style.left = thumb_x;
    thumb.style.top = 0.0f;
    thumb.style.width = 8.0f;
    thumb.style.height = 8.0f;
    thumb.style.background_color = "#f56161d9";

    container.children = {track, fill, thumb};
    return container;
}

} // namespace

TEST_CASE("a detached slider fill is reconnected to its thumb", "[view][import][slider]") {
    // The real TRIAZ geometry: thumb at [8,16], fill floating at [30,48] with a
    // 14px gap between them. Faithfully rendering the stored fill draws a broken
    // detached red bar; Figma's live component render keeps the fill on the
    // thumb. Bridge the gap so the bar meets the handle.
    IRNode slider = make_slider(/*fill_x=*/30.0f, /*fill_w=*/18.0f, /*thumb_x=*/8.0f);
    reconnect_slider_fill(slider);

    // Thumb center is 12; the fill's far edge (48) is preserved.
    REQUIRE(slider.children[1].style.left == Catch::Approx(12.0f));
    REQUIRE(slider.children[1].style.width == Catch::Approx(36.0f));
    // Track and thumb are never moved.
    REQUIRE(slider.children[0].style.left == Catch::Approx(0.0f));
    REQUIRE(slider.children[0].style.width == Catch::Approx(60.0f));
    REQUIRE(slider.children[2].style.left == Catch::Approx(8.0f));
}

TEST_CASE("slider reconnection bridges a gap on either side of the thumb",
          "[view][import][slider]") {
    // Fill entirely LEFT of the thumb: push its right edge to the thumb center.
    IRNode left = make_slider(/*fill_x=*/0.0f, /*fill_w=*/10.0f, /*thumb_x=*/40.0f);
    reconnect_slider_fill(left);
    // Thumb center 44; fill keeps its left edge (0), width grows to 44.
    REQUIRE(left.children[1].style.left == Catch::Approx(0.0f));
    REQUIRE(left.children[1].style.width == Catch::Approx(44.0f));
}

TEST_CASE("slider reconnection leaves an already-connected fill untouched",
          "[view][import][slider]") {
    // Fill [10,30] overlaps thumb [8,16]: the stored geometry already reads as a
    // connected bar, so it is faithful and must not be rewritten.
    IRNode ok = make_slider(/*fill_x=*/10.0f, /*fill_w=*/20.0f, /*thumb_x=*/8.0f);
    reconnect_slider_fill(ok);
    REQUIRE(ok.children[1].style.left == Catch::Approx(10.0f));
    REQUIRE(ok.children[1].style.width == Catch::Approx(20.0f));
}

TEST_CASE("slider reconnection ignores non-slider structures", "[view][import][slider]") {
    // A track-plus-thumb fader with no distinct colored fill (the TRIAZ "fx vol"
    // faders) has nothing to bridge — leave it alone and never crash.
    IRNode fader;
    fader.type = "frame";
    fader.style.width = 74.0f;
    fader.style.height = 7.0f;
    IRNode track;
    track.type = "frame";
    track.style.left = 0.0f;
    track.style.top = 3.0f;
    track.style.width = 74.0f;
    track.style.height = 1.0f;
    track.style.background_color = "#00000059";
    IRNode thumb;
    thumb.type = "ellipse";
    thumb.style.left = 50.0f;
    thumb.style.top = 0.0f;
    thumb.style.width = 7.0f;
    thumb.style.height = 7.0f;
    thumb.style.background_color = "#aeafb1";
    fader.children = {track, thumb};
    reconnect_slider_fill(fader);
    REQUIRE(fader.children[0].style.width == Catch::Approx(74.0f));
    REQUIRE(fader.children[1].style.left == Catch::Approx(50.0f));

    // A tall panel with three colored rects and no round thumb is not a slider:
    // its detached "fill" is real content and must survive verbatim.
    IRNode panel;
    panel.type = "frame";
    panel.style.width = 60.0f;
    panel.style.height = 40.0f; // not short: fails the wide-and-short gate
    IRNode a, b, c;
    for (auto* r : {&a, &b, &c}) {
        r->type = "frame";
        r->style.top = 0.0f;
        r->style.height = 10.0f;
        r->style.background_color = "#123456";
    }
    a.style.left = 0.0f;
    a.style.width = 60.0f;
    b.style.left = 30.0f;
    b.style.width = 18.0f;
    c.style.left = 8.0f;
    c.style.width = 8.0f;
    panel.children = {a, b, c};
    reconnect_slider_fill(panel);
    REQUIRE(panel.children[1].style.left == Catch::Approx(30.0f));
    REQUIRE(panel.children[1].style.width == Catch::Approx(18.0f));
}

TEST_CASE("the border shorthand splits without losing a functional color",
          "[view][import][border]") {
    IRNode n;
    n.type = "frame";
    // rgba() is ONE value containing spaces and commas. A plain space split
    // yields "rgba(255," as the color and the border vanishes again, one layer
    // further down — so the tokenizer tracks paren depth.
    n.style.border = "3px dashed rgba(255, 0, 0, 0.5)";
    normalize_border_shorthand(n);
    REQUIRE(n.style.border_color.has_value());
    REQUIRE(*n.style.border_color == "rgba(255, 0, 0, 0.5)");
    REQUIRE(n.style.border_width == 3.0f);
    REQUIRE(n.style.border_style.has_value());
    REQUIRE(*n.style.border_style == "dashed");
}

TEST_CASE("border shorthand normalization defers and declines", "[view][import][border]") {
    // A producer that set the discrete field said what it meant more precisely
    // than the shorthand can, so the shorthand must not overwrite it.
    IRNode explicit_color;
    explicit_color.type = "frame";
    explicit_color.style.border = "1px solid #000000";
    explicit_color.style.border_color = "#abcdef";
    normalize_border_shorthand(explicit_color);
    REQUIRE(*explicit_color.style.border_color == "#abcdef");

    // `border: none` is a positive statement that there is no edge. Inventing a
    // color-less 1px width here would be a border where the design says none.
    IRNode none;
    none.type = "frame";
    none.style.border = "none";
    normalize_border_shorthand(none);
    REQUIRE_FALSE(none.style.border_color.has_value());
    REQUIRE_FALSE(none.style.border_width.has_value());

    // A width-less shorthand still paints — CSS's initial border-width is
    // medium, and a design that says "solid red" means a visible edge.
    IRNode widthless;
    widthless.type = "frame";
    widthless.style.border = "solid #ff0000";
    normalize_border_shorthand(widthless);
    REQUIRE(*widthless.style.border_color == "#ff0000");
    REQUIRE(widthless.style.border_width == 1.0f);

    // Recurses: a stroke on a deep child is exactly the case that was lost.
    IRNode root;
    root.type = "frame";
    IRNode mid;
    mid.type = "frame";
    IRNode leaf;
    leaf.type = "frame";
    leaf.style.border = "2px solid #123456";
    mid.children.push_back(leaf);
    root.children.push_back(mid);
    normalize_border_shorthand(root);
    REQUIRE(*root.children[0].children[0].style.border_color == "#123456");
}

TEST_CASE("a border on a generic-frame fall-through node survives to setBorder",
          "[view][import][border]") {
    // A childless node whose kind is neither a recognized container, widget,
    // vector, image, nor text lowers via emit_js_generic_frame. That path
    // emitted setBackground / setCornerRadius but NOT setBorder, so a bordered
    // v0/claude/stitch `button`/`canvas`/`input` div lost its stroke even though
    // normalize_border_shorthand had already split the shorthand. Regression:
    // this is exactly the v0 audio-control-panel case (0 setBorder for two
    // bordered nodes) before the fix.
    DesignIR ir;
    ir.source = DesignSource::v0;
    ir.root.type = "frame";
    ir.root.name = "root";

    IRNode cta;
    cta.type = "button"; // unmapped kind, no children -> generic frame
    cta.name = "cta";
    cta.style.border = "1px solid #475569"; // shorthand: normalize splits it
    cta.style.border_radius = 6.0f;
    ir.root.children.push_back(cta);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    opts.include_comments = false;
    const auto js = generate_pulp_js(ir, opts);

    // The stroke, its split-out width, and the corner radius all reach the call.
    // (The bridge id carries a depth counter suffix, so match on the prefix.)
    REQUIRE(js.find("setBorder('cta") != std::string::npos);
    REQUIRE(js.find("'#475569', 1, 6") != std::string::npos);
}
// ── rgba() must survive the baked-C++ lane, not just the live-JS one ─────
// color_literal_expr() parsed hex only and returned an empty expression for
// rgb()/rgba(), so the baked-C++ emitter silently dropped colors the live
// materializer renders. Once the materializer learned rgba(), that gap became
// a LANE DIVERGENCE — the same design, two different pictures, depending on
// which backend materialized it — which the import-design skill's
// screenshot-parity invariant forbids. This pins both halves.
TEST_CASE("generated C++ carries rgba() colors like the materializer does",
          "[view][import][codegen][cpp][color]") {
    pulp::view::DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.background_color = "rgba(255, 0, 0, 0.5)";
    ir.root.style.border_color = "rgba(255,255,255,0.2)";
    ir.root.style.color = "rgb(200, 60, 60)";

    pulp::view::IRAssetManifest manifest;
    const auto gen = pulp::view::generate_pulp_cpp(ir, manifest);

    // 0.5 alpha -> 128 (round-half-up), 0.2 -> 51. If the rgb() arm regressed,
    // these emit NOTHING rather than a wrong value -- the failure mode is a
    // missing setter -- so assert on the literal we expect to see.
    CHECK(gen.source.find("rgba8(255, 0, 0, 128)") != std::string::npos);
    CHECK(gen.source.find("rgba8(255, 255, 255, 51)") != std::string::npos);
    CHECK(gen.source.find("rgba8(200, 60, 60, 255)") != std::string::npos);

    // Control: hex still works, so the rgb() arm didn't displace the hex path.
    pulp::view::DesignIR hex_ir;
    hex_ir.root.type = "frame";
    hex_ir.root.name = "HexPanel";
    hex_ir.root.style.background_color = "#1a1a2e";
    const auto hex_gen = pulp::view::generate_pulp_cpp(hex_ir, manifest);
    CHECK(hex_gen.source.find("rgba8(26, 26, 46, 255)") != std::string::npos);
}

TEST_CASE("generated JS opts its layout pass into sub-pixel geometry", "[view][import][subpixel]") {
    // Imported designs replay geometry the design tool solved at fractional
    // coordinates; Yoga's whole-pixel rounding visibly de-centered every
    // knob ring in "A Channel FX" relative to its body ellipse. The bundle
    // must opt out — typeof-guarded so it still loads on runtimes that
    // predate setSubpixelLayout and on web-compat DOM hosts.
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.root.type = "frame";
    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);
    REQUIRE(js.find("if (typeof setSubpixelLayout === 'function') "
                    "setSubpixelLayout('', true);") != std::string::npos);
}

TEST_CASE("generate_pulp_cpp re-opens a promoted button so its interactive child stays reachable") {
    // TextButton defaults to PointerEvents::box_only so a centred icon cannot
    // swallow its own click — but hit_test() then never descends, which would
    // make the box_none it emits for the child inert. The generated code must
    // re-open the parent, exactly as the runtime materializer does, or baked
    // output silently loses the interactive descendant the sibling Knob test
    // above proves is preserved. Knob has no box_only default, which is why
    // that test kept passing while the button path was broken.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.width = 120.0f;
    ir.root.style.height = 120.0f;
    ir.root.layout.direction = LayoutDirection::column;

    IRNode outer_button;
    outer_button.type = "button";
    outer_button.name = "Outer Button";
    outer_button.text_content = "Outer";
    outer_button.style.width = 80.0f;
    outer_button.style.height = 80.0f;
    outer_button.layout.direction = LayoutDirection::column;

    IRNode container;
    container.type = "frame";
    container.name = "Interactive Container";
    container.style.width = 80.0f;
    container.style.height = 40.0f;
    container.layout.direction = LayoutDirection::column;

    IRNode nested_button;
    nested_button.type = "button";
    nested_button.name = "Nested Fine Button";
    nested_button.text_content = "Fine";
    nested_button.style.width = 60.0f;
    nested_button.style.height = 20.0f;

    container.children.push_back(std::move(nested_button));
    outer_button.children.push_back(std::move(container));
    ir.root.children.push_back(std::move(outer_button));

    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(count_occurrences(result.source,
                              "->set_pointer_events(pulp::view::View::PointerEvents::box_none);") ==
            1);
    // ...and the parent re-opened, or that box_none reaches nothing.
    REQUIRE(count_occurrences(result.source,
                              "->set_pointer_events(pulp::view::View::PointerEvents::auto_);") ==
            1);
}

TEST_CASE("generate_pulp_js escapes raw box shadows and keeps audio labels escaped",
          "[view][import][security]") {
    // Design-supplied text reaching executable JS. The audio-label and comment
    // paths are covered by the widget-lowering change that escapes the label and
    // folds newlines out of the comment; this asserts both stay closed.
    //
    // Box shadows are the same class of hole and are still open: they emit
    // through box_shadow_to_css, which returns the design's OWN text verbatim
    // when a raw shadow string was kept for round-tripping, straight into a JS
    // literal.
    //
    // Observed from Forge, whose designer test plants `setTheme('light');` into
    // a node name, an anchor id, an audio label and a token key, then asserts
    // none of it survives lowering.
    //
    // The payload ends in `//` on purpose: once the literal is broken, that
    // comments out the orphaned `',` and the result parses, which is what makes
    // this executable rather than merely malformed.
    const std::string payload = "Rate\nsetTheme('light');\n//";

    DesignIR ir;
    ir.source = DesignSource::claude;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.layout.direction = LayoutDirection::column;

    IRNode knob;
    knob.type = "knob";
    knob.name = "rate_knob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = payload;
    knob.audio_min = 0.0f;
    knob.audio_max = 1.0f;
    knob.audio_default = 0.5f;

    ir.root.children.push_back(knob);

    // The shadow rides a plain node, not the knob: an audio widget lowers
    // through the web-compat DOM path, whose style never reaches the boxShadow
    // emitter, so a payload parked there would never be emitted and the
    // assertion below would pass without proving anything.
    //
    // `raw` is the vector that matters — box_shadow_to_css returns it verbatim
    // (design_ir_json.cpp: `if (!s.raw.empty()) { out += s.raw; continue; }`)
    // so whatever the design authored lands directly in the JS literal.
    IRNode panel;
    panel.type = "frame";
    panel.name = "shadow_panel";
    IRBoxShadow shadow;
    shadow.raw = "0 0 4px #000');\nsetTheme('light');\n//";
    panel.style.box_shadow.push_back(shadow);
    ir.root.children.push_back(panel);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = false;
    const auto js = generate_pulp_js(ir, opts);

    // The payload must not appear in a position where it would RUN.
    REQUIRE(js.find("\nsetTheme('light');\n") == std::string::npos);

    // Positively: the label survives as data, escaped, so the design still
    // round-trips — escaping must not simply delete the text.
    REQUIRE(js.find("setLabel(") != std::string::npos);
    REQUIRE(js.find("\\n") != std::string::npos);

    // Every emitted line must still be a balanced JS statement: a broken literal
    // shows up as a line whose single quotes do not pair.
    std::istringstream lines(js);
    std::string line;
    while (std::getline(lines, line)) {
        std::size_t quotes = 0;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] != '\'')
                continue;
            if (i > 0 && line[i - 1] == '\\')
                continue; // an escaped quote is data
            ++quotes;
        }
        INFO("unbalanced quotes on: " << line);
        REQUIRE(quotes % 2 == 0);
    }

    // And with comments ON, design text must not be able to terminate one.
    CodeGenOptions commented = opts;
    commented.include_comments = true;
    const auto with_comments = generate_pulp_js(ir, commented);
    REQUIRE(with_comments.find("\nsetTheme('light');\n") == std::string::npos);
}

// A faithful browser capture is one absolutely-positioned backdrop image with
// the bound controls placed ON it at their designed coordinates. The web-compat
// audio-widget branch returns before the shared style emitter, so every style
// the panel depends on has to be written inside that branch — and placement was
// not, which dropped each control into flex flow. They then stack down one edge
// sharing an x, which renders cleanly and is invisible to anything that only
// checks the controls exist.

// Tests for the JUCE-port-accelerator "faithful port toolkit" SDK primitives
// (Phase 6). Each primitive a faithful design port would otherwise re-roll by
// hand, tested at the level the GPU-OFF build can assert deterministically:
//
//   P6.3 — SVG fragment handles: extraction / document-build / transform math are
//          pure string+geometry and asserted directly; draw_fragment's Canvas op
//          is asserted against a RecordingCanvas subclass that captures draw_svg
//          (the base RecordingCanvas returns false for draw_svg, so this build —
//          PULP_ENABLE_GPU=OFF, no Skia raster — can't pixel-check; the comment on
//          each SECTION states which mode it is in).
//
// Additional primitives (P6.5 anchored popover, P6.6 drag-to-reorder, P6.7
// paint-space painters) append their own sections below as they land.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <string>
#include <vector>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/callout_box.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/svg_fragment.hpp>

using Catch::Approx;
using namespace pulp;
using pulp::view::CalloutPlacement;
using pulp::view::CalloutSide;
using pulp::view::CalloutStyle;
using pulp::view::AnchoredCallout;
using pulp::view::DesignFrameElement;
using pulp::view::DesignFrameView;
using pulp::view::FragmentTransform;
using pulp::view::Rect;

namespace {

// A RecordingCanvas that ALSO captures draw_svg documents (the base class returns
// false and drops the document, since it models the Canvas2D command stream, not
// the SVG-DOM path). Lets the fragment tests assert the exact mini-document and
// draw box draw_fragment hands the backend without a Skia raster surface.
class SvgRecordingCanvas : public canvas::RecordingCanvas {
public:
    struct SvgCall {
        std::string document;
        float x = 0, y = 0, w = 0, h = 0;
    };
    std::vector<SvgCall> svg_calls;

    bool draw_svg(const std::string& doc, float x, float y, float w, float h) override {
        svg_calls.push_back({doc, x, y, w, h});
        return true;  // pretend the backend rendered it
    }
};

// A tiny hand-authored SVG frame with a big panel <rect> (so detect_panel picks
// it edge-to-edge) plus a knob whose needle is a tagged <path>. The needle `d`
// is the unique fragment marker. Coordinates are the design's own space.
const char* kFrameSvg =
    "<svg width=\"200\" height=\"200\" viewBox=\"0 0 200 200\" "
    "xmlns=\"http://www.w3.org/2000/svg\">"
    "<rect x=\"10\" y=\"10\" width=\"180\" height=\"180\" fill=\"#101418\"/>"
    "<circle cx=\"100\" cy=\"100\" r=\"30\" fill=\"#202830\"/>"
    "<path d=\"M100 100 L100 75\" stroke=\"#39ff64\" fill=\"none\"/>"
    "</svg>";

DesignFrameView make_knob_view() {
    DesignFrameElement knob;
    knob.kind = DesignFrameElement::Kind::knob;
    knob.cx = 100.0f; knob.cy = 100.0f; knob.hit_radius = 30.0f;
    knob.needle_d = "M100 100 L100 75";
    knob.value = 0.5f;
    return DesignFrameView(kFrameSvg, {knob});
}

}  // namespace

// ── P6.3: SVG fragment handles ───────────────────────────────────────────────

TEST_CASE("FragmentTransform emits composed SVG transform attribute",
          "[view][fragment][issue-juce-port]") {
    // Pure math — no canvas.
    SECTION("identity emits nothing") {
        FragmentTransform id;
        CHECK(id.is_identity());
        CHECK(id.to_svg_transform().empty());
    }
    SECTION("translate only") {
        FragmentTransform t; t.dx = 12.0f; t.dy = -4.0f;
        CHECK_FALSE(t.is_identity());
        CHECK(t.to_svg_transform() == "translate(12.000 -4.000)");
    }
    SECTION("rotate about a pivot") {
        FragmentTransform t; t.rotate_deg = 30.0f; t.pivot_x = 100.0f; t.pivot_y = 100.0f;
        CHECK(t.to_svg_transform() == "rotate(30.000 100.000 100.000)");
    }
    SECTION("scale about a pivot expands to translate/scale/translate") {
        FragmentTransform t; t.scale = 1.5f; t.pivot_x = 50.0f; t.pivot_y = 60.0f;
        const std::string s = t.to_svg_transform();
        CHECK(s.find("scale(1.5000)") != std::string::npos);
        CHECK(s.find("translate(50.000 60.000)") != std::string::npos);
        CHECK(s.find("translate(-50.000 -60.000)") != std::string::npos);
    }
    SECTION("translate + rotate compose in order") {
        FragmentTransform t; t.dx = 5.0f; t.dy = 5.0f; t.rotate_deg = 90.0f;
        const std::string s = t.to_svg_transform();
        CHECK(s.rfind("translate(5.000 5.000)", 0) == 0);        // translate first
        CHECK(s.find("rotate(90.000") != std::string::npos);      // then rotate
    }
}

TEST_CASE("extract_svg_fragment pulls the marker's element out whole",
          "[view][fragment][issue-juce-port]") {
    SECTION("self-closing element by its path d marker") {
        const std::string frag =
            pulp::view::extract_svg_fragment(kFrameSvg, "M100 100 L100 75");
        CHECK(frag == "<path d=\"M100 100 L100 75\" stroke=\"#39ff64\" fill=\"none\"/>");
    }
    SECTION("unknown marker yields empty") {
        CHECK(pulp::view::extract_svg_fragment(kFrameSvg, "no-such-marker").empty());
    }
    SECTION("depth-matched container comes out whole") {
        const std::string doc =
            "<svg xmlns=\"http://www.w3.org/2000/svg\">"
            "<g id=\"outer\"><g id=\"inner\"><path d=\"MARK\"/></g><rect/></g>"
            "<circle/></svg>";
        const std::string frag = pulp::view::extract_svg_fragment(doc, "id=\"outer\"");
        CHECK(frag ==
              "<g id=\"outer\"><g id=\"inner\"><path d=\"MARK\"/></g><rect/></g>");
    }
}

TEST_CASE("svg_open_tag preserves the coordinate-space header",
          "[view][fragment][issue-juce-port]") {
    const std::string header = pulp::view::svg_open_tag(kFrameSvg);
    CHECK(header.find("width=\"200\"") != std::string::npos);
    CHECK(header.find("viewBox=\"0 0 200 200\"") != std::string::npos);
    CHECK(header.back() == '>');
    CHECK(pulp::view::svg_open_tag("no svg here").empty());
}

TEST_CASE("recolor_svg_fragment rewrites paint values, leaves none alone",
          "[view][fragment][issue-juce-port]") {
    const std::string frag =
        "<path d=\"X\" stroke=\"#39ff64\" fill=\"none\"/>";
    SECTION("fill=none is preserved; stroke opt-in recolors") {
        const std::string out =
            pulp::view::recolor_svg_fragment(frag, "#ffffff", /*include_stroke=*/true);
        CHECK(out.find("fill=\"none\"") != std::string::npos);   // none untouched
        CHECK(out.find("stroke=\"#ffffff\"") != std::string::npos);
    }
    SECTION("a real fill is replaced") {
        const std::string out = pulp::view::recolor_svg_fragment(
            "<circle fill=\"#202830\"/>", "#ff0000");
        CHECK(out == "<circle fill=\"#ff0000\"/>");
    }
    SECTION("empty hex is a no-op") {
        CHECK(pulp::view::recolor_svg_fragment(frag, "") == frag);
    }
}

TEST_CASE("build_svg_fragment_document composites one fragment in the source space",
          "[view][fragment][issue-juce-port]") {
    const std::string frag =
        pulp::view::extract_svg_fragment(kFrameSvg, "M100 100 L100 75");
    REQUIRE_FALSE(frag.empty());

    SECTION("identity + opaque + no recolor emits the bare fragment") {
        const std::string doc = pulp::view::build_svg_fragment_document(
            kFrameSvg, frag, {}, 1.0f, {});
        // Shares the source header, contains ONLY the fragment (no other element),
        // and needs no wrapping <g>.
        CHECK(doc.rfind("<svg", 0) == 0);
        CHECK(doc.find("viewBox=\"0 0 200 200\"") != std::string::npos);
        CHECK(doc.find(frag) != std::string::npos);
        CHECK(doc.find("<circle") == std::string::npos);   // sibling excluded
        CHECK(doc.find("<g") == std::string::npos);         // no wrapper needed
        CHECK(doc.find("</svg>") != std::string::npos);
    }
    SECTION("transform + opacity + recolor wrap in a <g>") {
        FragmentTransform t; t.rotate_deg = 45.0f; t.pivot_x = 100.0f; t.pivot_y = 100.0f;
        const std::string doc = pulp::view::build_svg_fragment_document(
            kFrameSvg, frag, t, 0.5f, "#ffffff");
        CHECK(doc.find("<g transform=\"rotate(45.000 100.000 100.000)\"") !=
              std::string::npos);
        CHECK(doc.find("opacity=\"0.5000\"") != std::string::npos);
        CHECK(doc.find("stroke=\"#39ff64\"") == std::string::npos);  // recolored...
        CHECK(doc.find("#ffffff") != std::string::npos);             // ...to white
        CHECK(doc.find("fill=\"none\"") != std::string::npos);       // none kept
    }
    SECTION("missing header / empty fragment yields empty") {
        CHECK(pulp::view::build_svg_fragment_document("nope", frag, {}).empty());
        CHECK(pulp::view::build_svg_fragment_document(kFrameSvg, "", {}).empty());
    }
}

TEST_CASE("DesignFrameView::draw_fragment issues one draw_svg over the panel box",
          "[view][fragment][issue-juce-port]") {
    // Mode: RecordingCanvas capture of the draw_svg op (no Skia raster in this
    // GPU-OFF build), asserting the mini-document + draw box are correct.
    DesignFrameView v = make_knob_view();
    v.set_bounds({0, 0, 200, 200});   // bounds == frame size → panel fits at scale 1
    v.layout_children();

    v.register_fragment("needle", "M100 100 L100 75");
    CHECK(v.has_fragment("needle"));
    CHECK_FALSE(v.has_fragment("missing"));

    SECTION("registered fragment draws, transform+recolor reach the document") {
        SvgRecordingCanvas canvas;
        FragmentTransform t; t.rotate_deg = 20.0f; t.pivot_x = 100.0f; t.pivot_y = 100.0f;
        const bool ok = v.draw_fragment(canvas, "needle", t, 0.6f, "#ff8800");
        CHECK(ok);
        REQUIRE(canvas.svg_calls.size() == 1);
        const auto& call = canvas.svg_calls[0];
        // Panel is the 180x180 rect at (10,10) but the SVG is drawn at the frame
        // origin so its coords line up: draw box maps panel top-left to view.
        CHECK(call.document.find("rotate(20.000 100.000 100.000)") != std::string::npos);
        CHECK(call.document.find("opacity=\"0.6000\"") != std::string::npos);
        CHECK(call.document.find("#ff8800") != std::string::npos);
        // Draw box is finite and positive — the panel is laid out.
        CHECK(call.w > 0.0f);
        CHECK(call.h > 0.0f);
    }
    SECTION("unknown id draws nothing") {
        SvgRecordingCanvas canvas;
        CHECK_FALSE(v.draw_fragment(canvas, "missing"));
        CHECK(canvas.svg_calls.empty());
    }
    SECTION("marker absent from the SVG draws nothing") {
        SvgRecordingCanvas canvas;
        CHECK_FALSE(v.draw_fragment_marker(canvas, "not-in-svg"));
        CHECK(canvas.svg_calls.empty());
    }
    SECTION("un-laid-out view (zero bounds) draws nothing") {
        DesignFrameView unlaid = make_knob_view();  // no set_bounds
        SvgRecordingCanvas canvas;
        CHECK_FALSE(unlaid.draw_fragment_marker(canvas, "M100 100 L100 75"));
        CHECK(canvas.svg_calls.empty());
    }
}

TEST_CASE("DesignFrameView hover/bypass restyle rides draw_fragment",
          "[view][fragment][issue-juce-port]") {
    // paint() draws the full frame then, for elements with a fragment handle,
    // redraws that fragment brightened (hovered) or desaturated (disabled). We
    // assert the EXTRA draw_svg calls beyond the one full-frame draw.
    DesignFrameView v = make_knob_view();
    v.set_bounds({0, 0, 200, 200});
    v.layout_children();

    SECTION("idle: only the full frame is drawn") {
        SvgRecordingCanvas canvas;
        v.paint(canvas);
        CHECK(canvas.svg_calls.size() == 1);   // just the frame
    }
    SECTION("hovered element adds a brightened fragment redraw") {
        SvgRecordingCanvas canvas;
        v.simulate_hover({100, 100});          // over the knob
        REQUIRE(v.element_hovered() == 0);
        v.paint(canvas);
        REQUIRE(canvas.svg_calls.size() == 2);
        CHECK(canvas.svg_calls[1].document.find("#ffffff") != std::string::npos);
    }
    SECTION("disabled element adds a desaturated fragment redraw") {
        SvgRecordingCanvas canvas;
        v.set_element_enabled(0, false);
        v.paint(canvas);
        REQUIRE(canvas.svg_calls.size() == 2);
        CHECK(canvas.svg_calls[1].document.find("#6b7280") != std::string::npos);
    }
}

// ── P6.5: Anchored popover with pointer triangle (AnchoredCallout parity) ─────────
// Pure placement math (place_callout) — no canvas needed.

namespace {
constexpr float kWinW = 400.0f, kWinH = 400.0f;
const Rect kWindow{0, 0, kWinW, kWinH};
}  // namespace

TEST_CASE("place_callout keeps the preferred side when it fits",
          "[view][callout][issue-juce-port]") {
    // Anchor mid-window: everything fits, so preferred survives.
    const Rect anchor{190, 190, 20, 20};
    auto p = pulp::view::place_callout(anchor, 120, 80, kWindow, CalloutSide::below);
    CHECK(p.side == CalloutSide::below);
    // Body sits below the anchor, centered on it.
    CHECK(p.body.y > anchor.bottom());
    CHECK(p.body.center().x == Approx(anchor.center().x));
}

TEST_CASE("place_callout auto-flips when the preferred side would clip",
          "[view][callout][issue-juce-port]") {
    CalloutStyle style;  // defaults
    SECTION("near top edge, prefer above -> flips below") {
        const Rect anchor{190, 4, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 120, kWindow,
                                           CalloutSide::above, style);
        CHECK(p.side == CalloutSide::below);
        CHECK(p.body.y >= anchor.bottom());
    }
    SECTION("near bottom edge, prefer below -> flips above") {
        const Rect anchor{190, kWinH - 24, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 120, kWindow,
                                           CalloutSide::below, style);
        CHECK(p.side == CalloutSide::above);
        CHECK(p.body.bottom() <= anchor.y);
    }
    SECTION("near left edge, prefer left_of -> flips right_of") {
        const Rect anchor{4, 190, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 120, kWindow,
                                           CalloutSide::left_of, style);
        CHECK(p.side == CalloutSide::right_of);
        CHECK(p.body.x >= anchor.right());
    }
    SECTION("no room either side keeps preferred (best effort)") {
        const Rect tiny{0, 0, 40, 40};
        const Rect anchor{10, 10, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 120, tiny,
                                           CalloutSide::below, style);
        CHECK(p.side == CalloutSide::below);  // neither side fits -> no flip
    }
}

TEST_CASE("place_callout clamps the body inside the window margins",
          "[view][callout][issue-juce-port]") {
    CalloutStyle style;  // margin = 8
    // Anchor hugging the left edge with a wide body: centering would push the body
    // off the left of the window, so the cross axis clamps to the margin.
    const Rect anchor{2, 190, 12, 12};
    auto p = pulp::view::place_callout(anchor, 160, 80, kWindow,
                                       CalloutSide::below, style);
    CHECK(p.body.x >= style.margin - 0.01f);
    CHECK(p.body.right() <= kWinW - style.margin + 0.01f);
}

TEST_CASE("place_callout arrow tracks the anchor and stays on the body edge",
          "[view][callout][issue-juce-port]") {
    CalloutStyle style;
    SECTION("centered anchor: arrow tip aligns with the anchor center") {
        const Rect anchor{190, 190, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 80, kWindow,
                                           CalloutSide::below, style);
        CHECK(p.arrow_tip_x == Approx(anchor.center().x));
        // Tip points UP at the anchor: above the body top edge.
        CHECK(p.arrow_tip_y < p.body.y);
        CHECK(p.arrow_base_y == Approx(p.body.y));
    }
    SECTION("clamped body: arrow base stays within the body edge, clear of corners") {
        // Anchor at far left forces the body to clamp; the arrow should still
        // point roughly at the anchor but never leave the body's flat edge.
        const Rect anchor{2, 190, 12, 12};
        auto p = pulp::view::place_callout(anchor, 160, 80, kWindow,
                                           CalloutSide::below, style);
        const float half_aw = style.arrow_width * 0.5f;
        CHECK(p.arrow_base_x >= p.body.x + style.corner_radius + half_aw - 0.01f);
        CHECK(p.arrow_base_x <= p.body.right() - style.corner_radius - half_aw + 0.01f);
    }
    SECTION("horizontal side: arrow points sideways at the anchor") {
        const Rect anchor{190, 190, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 80, kWindow,
                                           CalloutSide::right_of, style);
        CHECK(p.side == CalloutSide::right_of);
        CHECK(p.arrow_tip_x < p.body.x);          // tip left of the body (toward anchor)
        CHECK(p.arrow_tip_y == Approx(anchor.center().y));
    }
}

TEST_CASE("AnchoredCallout hosts content and mounts as an overlay",
          "[view][callout][issue-juce-port]") {
    // View-level: mode is geometry assertion (no Skia raster in this build).
    auto root = std::make_unique<pulp::view::View>();
    root->set_bounds({0, 0, kWinW, kWinH});

    auto content = std::make_unique<pulp::view::View>();
    const Rect anchor{190, 20, 20, 20};  // near top -> should flip below
    AnchoredCallout* box = AnchoredCallout::show(root.get(), anchor, std::move(content),
                                       CalloutSide::above);
    REQUIRE(box != nullptr);
    box->set_content_size(100, 60);
    box->set_bounds({0, 0, kWinW, kWinH});
    box->layout_children();

    auto p = box->compute_placement({0, 0, kWinW, kWinH});
    CHECK(p.side == CalloutSide::below);           // flipped away from the top edge
    // Content child is inset inside the body by the padding.
    REQUIRE(box->content() != nullptr);
    const Rect cb = box->content()->bounds();
    CHECK(cb.x > p.body.x);
    CHECK(cb.y > p.body.y);
    CHECK(cb.width == Approx(100.0f));
    CHECK(cb.height == Approx(60.0f));
}

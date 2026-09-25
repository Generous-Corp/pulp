#include "test_design_import_shared.hpp"
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/view/pointer_dispatch.hpp>

// Web-compat control emitters: overlay placement, discrete and selector
// controls, tiled backgrounds, white-space, accents and skins.

namespace {

DesignIR capture_overlay_ir() {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 600.0f;
    ir.root.style.height = 400.0f;

    IRNode backdrop;
    backdrop.type = "image";
    backdrop.name = "capture";
    backdrop.style.position = "absolute";
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.width = 600.0f;
    backdrop.style.height = 400.0f;
    ir.root.children.push_back(std::move(backdrop));

    // Three knobs across the panel — different x AND different y, as a real
    // design places them.
    const float xs[] = {40.0f, 180.0f, 320.0f};
    const float ys[] = {50.0f, 90.0f, 130.0f};
    for (int i = 0; i < 3; ++i) {
        IRNode knob;
        knob.type = "frame";
        knob.name = "knob_" + std::to_string(i);
        knob.audio_widget = AudioWidgetType::knob;
        knob.attributes["binding"] = "param_" + std::to_string(i);
        knob.style.position = "absolute";
        knob.style.left = xs[i];
        knob.style.top = ys[i];
        knob.style.width = 64.0f;
        knob.style.height = 64.0f;
        // z-index 10 is exactly web-compat's auto-overlay threshold. Set here
        // on purpose: the lowering must NOT pass it through, or these three
        // controls would each claim the single global overlay slot.
        knob.style.z_index = 10;
        ir.root.children.push_back(std::move(knob));
    }
    return ir;
}

}  // namespace

TEST_CASE("web-compat overlay controls carry their designed position",
          "[view][import][web-compat][overlay]") {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = false;
    const auto js = generate_pulp_js(capture_overlay_ir(), opts);
    INFO(js);

    // Four absolutely-positioned boxes: the backdrop plus three controls. Before
    // the fix only the backdrop had one.
    REQUIRE(count_occurrences(js, ".style.position = 'absolute';") == 4);

    // Each control at ITS OWN x. This is the assertion that fails when controls
    // fall into flex flow: they would then share the parent's left edge, and the
    // panel is a column of knobs beside the artwork instead of over it.
    REQUIRE(js.find(".style.left = '40px';") != std::string::npos);
    REQUIRE(js.find(".style.left = '180px';") != std::string::npos);
    REQUIRE(js.find(".style.left = '320px';") != std::string::npos);
    REQUIRE(js.find(".style.top = '50px';") != std::string::npos);
    REQUIRE(js.find(".style.top = '90px';") != std::string::npos);
    REQUIRE(js.find(".style.top = '130px';") != std::string::npos);

    // Three distinct x values, not three copies of one. A lowering that emitted
    // the same left for every control would satisfy a mere "left is present"
    // check and still render the stacked column this test exists to catch.
    REQUIRE(count_occurrences(js, ".style.left = '0px';") == 1);  // backdrop only

    // And NO z-index, however the design declared one. `position:absolute`
    // with z-index >= 10 makes web-compat claim the single global overlay slot,
    // so passing a designed stacking order through would have three controls
    // fighting over it — last one wins and the other two are released. Placing
    // them is the fix; promoting them to popovers is not.
    REQUIRE(js.find(".style.zIndex") == std::string::npos);
}

TEST_CASE("a web-compat control with no declared position emits none",
          "[view][import][web-compat][overlay]") {
    // Flex flow is correct for a control the design did not place. Emitting a
    // position anyway would break every non-overlay panel to fix the overlay one.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Rack";
    IRNode knob;
    knob.type = "frame";
    knob.name = "flow_knob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.style.width = 64.0f;
    knob.style.height = 64.0f;
    ir.root.children.push_back(std::move(knob));

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = false;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    REQUIRE(js.find(".style.position") == std::string::npos);
    REQUIRE(js.find(".style.left") == std::string::npos);
    REQUIRE(js.find(".style.zIndex") == std::string::npos);
    // The box itself still lands.
    REQUIRE(js.find(".style.width = '64px';") != std::string::npos);
}

TEST_CASE("an overlay control keeps the capture visible beneath it",
          "[view][import][web-compat][overlay]") {
    // The positioning subset is emitted, NOT the general style run. A control
    // over a capture has no body of its own — the bitmap underneath IS its body
    // — so a background emitted here would paint over the art.
    const auto js = generate_pulp_js(capture_overlay_ir(), [] {
        CodeGenOptions o;
        o.mode = CodeGenMode::web_compat;
        o.include_comments = false;
        return o;
    }());
    INFO(js);
    REQUIRE(js.find(".style.backgroundColor") == std::string::npos);
}

TEST_CASE("discrete control emitters configure choices, defaults, and designed overlays",
          "[view][import][codegen][discrete-controls][overlay]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRNode selector;
    selector.type = "frame";
    selector.audio_widget = AudioWidgetType::selector;
    selector.audio_min = 1.0f;
    selector.audio_max = 5.0f;
    selector.audio_default = 3.0f;
    selector.attributes["pulpRouteId"] = "selector";
    selector.attributes["pulpChoices"] = "Pulse|Saw|Noise";
    selector.attributes["designed_body"] = "underlay";
    ir.root.children.push_back(std::move(selector));

    for (const auto mode : {CodeGenMode::web_compat,
                            CodeGenMode::bridge_native_js}) {
        CodeGenOptions opts;
        opts.mode = mode;
        opts.include_comments = false;
        const auto js = generate_pulp_js(ir, opts);
        INFO(js);
        const auto segments_at = js.find("setSegments(");
        const auto value_at = js.find("setValue(", segments_at);
        REQUIRE(segments_at != std::string::npos);
        REQUIRE(value_at != std::string::npos);
        CHECK(segments_at < value_at);
        CHECK(js.substr(value_at, 80).find("0.5") != std::string::npos);
        CHECK(js.find("setDesignedOverlay(") != std::string::npos);
    }
}

TEST_CASE("selector emitters keep a usable fallback segment",
          "[view][import][codegen][discrete-controls][fallback]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRNode selector;
    selector.type = "frame";
    selector.audio_widget = AudioWidgetType::selector;
    selector.text_content = "Only";
    ir.root.children.push_back(std::move(selector));

    for (const auto mode : {CodeGenMode::web_compat,
                            CodeGenMode::bridge_native_js}) {
        CodeGenOptions opts;
        opts.mode = mode;
        opts.include_comments = false;
        const auto js = generate_pulp_js(ir, opts);
        INFO(js);
        CHECK(js.find("setSegments(") != std::string::npos);
        CHECK(js.find("'Only'") != std::string::npos);
    }
}

// A tiled background is a gradient PLUS a size, and the JS emitter wrote only
// the gradient — so the tile collapsed to one stretched copy.
//
// That is not a texture nicety. A design-system grid or scanline overlay IS
// nothing but a gradient and a size, so losing the size loses the whole
// element: a spectrum display whose only children were `.grid-x` / `.grid-y`
// rendered as an empty panel and was reported as a layout bug on three
// different panels before the missing size was found here.
//
// Order is load-bearing. The `background` shorthand RESETS `background-size`,
// so the size has to be written after it; emitted first, it is silently
// discarded and this test would pass against markup that still renders wrong.
TEST_CASE("the JS emitter carries tiled background size and position after the shorthand",
          "[view][import][codegen][background-size]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    IRNode grid;
    grid.type = "frame";
    grid.name = "grid-x";
    grid.style.background_gradient =
        "linear-gradient(90deg, rgb(36, 38, 84) 1px, rgba(0, 0, 0, 0) 1px), "
        "linear-gradient(0deg, rgb(36, 38, 84) 1px, rgba(0, 0, 0, 0) 1px)";
    grid.style.background_size = "12.5% 100%, 100% 12.5%";
    grid.style.background_position = "0 0, 2px 2px";
    grid.attributes["browser_box_paint_dpr"] = "2";
    grid.attributes["browser_box_paint_left_px"] = "10";
    grid.attributes["browser_box_paint_top_px"] = "12";
    grid.attributes["browser_box_paint_right_px"] = "210";
    grid.attributes["browser_box_paint_bottom_px"] = "112";
    ir.root.children.push_back(std::move(grid));

    // Position is inert on a single, unsized full-box gradient, including both
    // Chromium's computed `0% 0%` and authored offsets. None may become a
    // Swift strict-fidelity failure merely because the field was captured.
    DesignIR full_box_ir;
    full_box_ir.root.type = "frame";
    IRNode full_box_gradient;
    full_box_gradient.type = "frame";
    full_box_gradient.style.background_gradient =
        "linear-gradient(to right, #123456, #654321)";
    full_box_ir.root.children.push_back(std::move(full_box_gradient));
    for (const char* position : {"0% 0%", "0.0% 0.0%", "0 0", "50% 50%", "0px 50%", "50% 0px"}) {
        full_box_ir.root.children.front().style.background_position = position;
        std::vector<FidelityIssue> inert_issues;
        SwiftExportOptions inert_options;
        inert_options.fidelity_report = &inert_issues;
        (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest, inert_options);
        INFO("inert full-box background-position: " << position);
        CHECK_FALSE(std::any_of(inert_issues.begin(), inert_issues.end(),
                                [](const FidelityIssue& issue) {
                                    return issue.kind == "swiftui-background-position" &&
                                        !issue.informational;
                                }));
    }
    full_box_ir.root.children.front().style.background_position = "0% 0%, 0% 0%";
    std::vector<FidelityIssue> layered_default_issues;
    SwiftExportOptions layered_default_options;
    layered_default_options.fidelity_report = &layered_default_issues;
    (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest,
                              layered_default_options);
    CHECK_FALSE(std::any_of(layered_default_issues.begin(), layered_default_issues.end(),
                            [](const FidelityIssue& issue) {
                                return issue.kind == "swiftui-background-position" &&
                                    !issue.informational;
                            }));

    // Unlike percentage positions, a non-zero absolute offset changes the
    // repeated CSS gradient's visible tile phase even at its default size.
    for (const char* position : {"2px 2px", "-0.5px 0px", "calc(2px + 0%) 0px"}) {
        full_box_ir.root.children.front().style.background_position = position;
        std::vector<FidelityIssue> absolute_offset_issues;
        SwiftExportOptions absolute_offset_options;
        absolute_offset_options.fidelity_report = &absolute_offset_issues;
        (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest,
                                  absolute_offset_options);
        INFO("material full-box background-position: " << position);
        CHECK(std::any_of(absolute_offset_issues.begin(), absolute_offset_issues.end(),
                          [](const FidelityIssue& issue) {
                              return issue.kind == "swiftui-background-position" &&
                                  !issue.informational;
                          }));
    }

    // Background-size has the same per-layer default rule. Browser capture
    // can retain either axis spelling or a comma-separated default list; none
    // changes a gradient with no intrinsic dimensions.
    full_box_ir.root.children.front().style.background_position.reset();
    for (const char* size : {"auto, auto", "100% auto", "auto 100%", "100.0% 100%", "contain", "cover"}) {
        full_box_ir.root.children.front().style.background_size = size;
        std::vector<FidelityIssue> default_size_issues;
        SwiftExportOptions default_size_options;
        default_size_options.fidelity_report = &default_size_issues;
        (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest,
                                  default_size_options);
        INFO("inert full-box background-size: " << size);
        CHECK_FALSE(std::any_of(default_size_issues.begin(), default_size_issues.end(),
                                [](const FidelityIssue& issue) {
                                    return issue.kind == "swiftui-background-size" &&
                                        !issue.informational;
                                }));
    }

    // CSS cycles lists only across image layers. Surplus values and values on
    // a flat color cannot create a Swift strict-fidelity divergence.
    full_box_ir.root.children.front().style.background_size = "auto, 12px 100%";
    full_box_ir.root.children.front().style.background_position = "0% 0%, 2px 2px";
    std::vector<FidelityIssue> surplus_issues;
    SwiftExportOptions surplus_options;
    surplus_options.fidelity_report = &surplus_issues;
    (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest, surplus_options);
    CHECK_FALSE(std::any_of(surplus_issues.begin(), surplus_issues.end(),
                            [](const FidelityIssue& issue) { return !issue.informational; }));

    DesignIR flat_ir;
    flat_ir.root.type = "frame";
    IRNode flat;
    flat.type = "frame";
    flat.style.background_color = "#123456";
    flat.style.background_size = "12px 100%";
    flat.style.background_position = "2px 2px";
    flat_ir.root.children.push_back(std::move(flat));
    std::vector<FidelityIssue> flat_issues;
    SwiftExportOptions flat_options;
    flat_options.fidelity_report = &flat_issues;
    (void)generate_pulp_swift(flat_ir, flat_ir.asset_manifest, flat_options);
    CHECK_FALSE(std::any_of(flat_issues.begin(), flat_issues.end(),
                            [](const FidelityIssue& issue) { return !issue.informational; }));

    // Axis materiality is independent: a vertical percentage offset does
    // nothing when only the horizontal gradient tile is smaller than its box.
    full_box_ir.root.children.front().style.background_size = "12px 100%";
    full_box_ir.root.children.front().style.background_position = "0% 50%";
    std::vector<FidelityIssue> inert_axis_issues;
    SwiftExportOptions inert_axis_options;
    inert_axis_options.fidelity_report = &inert_axis_issues;
    (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest, inert_axis_options);
    CHECK_FALSE(std::any_of(inert_axis_issues.begin(), inert_axis_issues.end(),
                            [](const FidelityIssue& issue) {
                                return issue.kind == "swiftui-background-position" &&
                                    !issue.informational;
                            }));
    full_box_ir.root.children.front().style.background_position = "50% 0%";
    std::vector<FidelityIssue> material_axis_issues;
    SwiftExportOptions material_axis_options;
    material_axis_options.fidelity_report = &material_axis_issues;
    (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest, material_axis_options);
    CHECK(std::any_of(material_axis_issues.begin(), material_axis_issues.end(),
                      [](const FidelityIssue& issue) {
                          return issue.kind == "swiftui-background-position" &&
                              !issue.informational;
                      }));

    // Even default size/position cannot make a second actual CSS image layer
    // representable by one SwiftUI LinearGradient.
    full_box_ir.root.children.front().style.background_size = "auto, auto";
    full_box_ir.root.children.front().style.background_position = "0% 0%, 0% 0%";
    full_box_ir.root.children.front().style.background_gradient =
        "linear-gradient(to right, #123456, #654321), "
        "linear-gradient(to bottom, #abcdef, #fedcba)";
    std::vector<FidelityIssue> multiple_layer_issues;
    SwiftExportOptions multiple_layer_options;
    multiple_layer_options.fidelity_report = &multiple_layer_issues;
    (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest, multiple_layer_options);
    CHECK(std::any_of(multiple_layer_issues.begin(), multiple_layer_issues.end(),
                      [](const FidelityIssue& issue) {
                          return issue.kind == "swiftui-background-layers" &&
                              !issue.informational;
                      }));

    // The image-layer count is not a gradient-only count: a URL under/over a
    // linear gradient is still an independently painted CSS layer that Swift
    // cannot silently drop.
    full_box_ir.root.children.front().style.background_gradient =
        "url(texture.png), linear-gradient(to right, #123456, #654321)";
    full_box_ir.root.children.front().style.background_size = "auto, auto";
    full_box_ir.root.children.front().style.background_position = "0% 0%, 0% 0%";
    std::vector<FidelityIssue> mixed_layer_issues;
    SwiftExportOptions mixed_layer_options;
    mixed_layer_options.fidelity_report = &mixed_layer_issues;
    (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest, mixed_layer_options);
    CHECK(std::any_of(mixed_layer_issues.begin(), mixed_layer_issues.end(),
                      [](const FidelityIssue& issue) {
                          return issue.kind == "swiftui-background-layers" &&
                              !issue.informational;
                      }));

    // A single unsupported CSS gradient is also a hard divergence: accepting
    // it only as an informational note would export a flat fill in strict mode.
    for (const char* gradient : {
             "radial-gradient(circle, #123456, #654321)",
             "repeating-linear-gradient(to right, #123456 0 2px, #654321 2px 4px)",
             "cross-fade(linear-gradient(to right, #123456, #654321), #000000)",
             "linear-gradient(#123456, 50%, #654321)",
         }) {
        full_box_ir.root.children.front().style.background_gradient = gradient;
        full_box_ir.root.children.front().style.background_size.reset();
        full_box_ir.root.children.front().style.background_position.reset();
        std::vector<FidelityIssue> unsupported_gradient_issues;
        SwiftExportOptions unsupported_gradient_options;
        unsupported_gradient_options.fidelity_report = &unsupported_gradient_issues;
        (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest,
                                  unsupported_gradient_options);
        INFO("unsupported Swift gradient: " << gradient);
        CHECK(std::any_of(unsupported_gradient_issues.begin(),
                          unsupported_gradient_issues.end(),
                          [](const FidelityIssue& issue) {
                              return issue.kind == "swiftui-gradient" &&
                                  !issue.informational;
                          }));
    }

    // `none` has no pixels but still occupies its declared CSS image-list
    // index. The second image must consult size/position entry 1, not entry 0.
    full_box_ir.root.children.front().style.background_gradient =
        "none, linear-gradient(to right, #123456, #654321)";
    full_box_ir.root.children.front().style.background_size = "12px 100%, auto";
    full_box_ir.root.children.front().style.background_position = "50% 0%, 0% 0%";
    std::vector<FidelityIssue> none_then_linear_inert_issues;
    SwiftExportOptions none_then_linear_inert_options;
    none_then_linear_inert_options.fidelity_report = &none_then_linear_inert_issues;
    (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest,
                              none_then_linear_inert_options);
    CHECK_FALSE(std::any_of(none_then_linear_inert_issues.begin(),
                            none_then_linear_inert_issues.end(),
                            [](const FidelityIssue& issue) {
                                return (issue.kind == "swiftui-background-size" ||
                                        issue.kind == "swiftui-background-position") &&
                                    !issue.informational;
                            }));
    full_box_ir.root.children.front().style.background_size = "auto, 12px 100%";
    full_box_ir.root.children.front().style.background_position = "0% 0%, 50% 0%";
    std::vector<FidelityIssue> none_then_linear_material_issues;
    SwiftExportOptions none_then_linear_material_options;
    none_then_linear_material_options.fidelity_report = &none_then_linear_material_issues;
    (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest,
                              none_then_linear_material_options);
    CHECK(std::any_of(none_then_linear_material_issues.begin(),
                      none_then_linear_material_issues.end(),
                      [](const FidelityIssue& issue) {
                          return issue.kind == "swiftui-background-size" &&
                              !issue.informational;
                      }));
    CHECK(std::any_of(none_then_linear_material_issues.begin(),
                      none_then_linear_material_issues.end(),
                      [](const FidelityIssue& issue) {
                          return issue.kind == "swiftui-background-position" &&
                              !issue.informational;
                      }));

    // A non-default size is material even without a position: SwiftUI's
    // full-box LinearGradient cannot reproduce CSS's gradient tile geometry.
    full_box_ir.root.children.front().style.background_position.reset();
    full_box_ir.root.children.front().style.background_size = "12.5% 100%";
    std::vector<FidelityIssue> size_only_issues;
    SwiftExportOptions size_only_options;
    size_only_options.fidelity_report = &size_only_issues;
    (void)generate_pulp_swift(full_box_ir, full_box_ir.asset_manifest,
                              size_only_options);
    CHECK(std::any_of(size_only_issues.begin(), size_only_issues.end(),
                      [](const FidelityIssue& issue) {
                          return issue.kind == "swiftui-background-size" &&
                              !issue.informational;
                      }));

    // Unlike the unsized full-box controls, this is a real two-layer tiled
    // checkerboard phase. SwiftUI must disclose that it cannot represent it.

    // web_compat, not bridge_native_js: this is the document.createElement +
    // el.style lane, which is what a generated panel's ui.js actually uses.
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);

    const auto gradient_at = js.find(".style.background = ");
    const auto size_at = js.find(".style.backgroundSize = ");
    const auto position_at = js.find(".style.backgroundPosition = ");
    REQUIRE(gradient_at != std::string::npos);
    REQUIRE(size_at != std::string::npos);
    REQUIRE(position_at != std::string::npos);
    CHECK(js.find("12.5% 100%, 100% 12.5%") != std::string::npos);
    // The shorthand resets the size, so anything else here ships a tile that
    // never tiles.
    CHECK(gradient_at < size_at);
    CHECK(size_at < position_at);

    const auto cpp = generate_pulp_cpp(ir, ir.asset_manifest, {});
    CHECK(cpp.source.find("set_background_position(\"0 0, 2px 2px\")") !=
          std::string::npos);
    CHECK(cpp.source.find("set_captured_box_paint_rect(5.0f, 6.0f, 105.0f, 56.0f, 2.0f)") !=
          std::string::npos);

    std::vector<FidelityIssue> painted_box_swift_issues;
    SwiftExportOptions painted_box_swift_options;
    painted_box_swift_options.fidelity_report = &painted_box_swift_issues;
    (void)generate_pulp_swift(ir, ir.asset_manifest, painted_box_swift_options);
    CHECK(std::any_of(painted_box_swift_issues.begin(), painted_box_swift_issues.end(),
                      [](const FidelityIssue& issue) {
                          return issue.kind == "swiftui-browser-box-paint-rect" &&
                              !issue.informational;
                      }));

    // Browser capture records the same tuple on transparent layout/text
    // carriers. Native View never consumes it without a background, gradient,
    // or painted border, so it cannot change exported SwiftUI pixels.
    DesignIR inert_box_ir;
    inert_box_ir.root.type = "frame";
    IRNode inert_box;
    inert_box.type = "frame";
    inert_box.attributes["browser_box_paint_dpr"] = "2";
    inert_box.attributes["browser_box_paint_left_px"] = "10";
    inert_box.attributes["browser_box_paint_top_px"] = "12";
    inert_box.attributes["browser_box_paint_right_px"] = "210";
    inert_box.attributes["browser_box_paint_bottom_px"] = "112";
    inert_box_ir.root.children.push_back(std::move(inert_box));
    std::vector<FidelityIssue> inert_box_swift_issues;
    SwiftExportOptions inert_box_swift_options;
    inert_box_swift_options.fidelity_report = &inert_box_swift_issues;
    (void)generate_pulp_swift(inert_box_ir, inert_box_ir.asset_manifest,
                              inert_box_swift_options);
    CHECK_FALSE(std::any_of(inert_box_swift_issues.begin(), inert_box_swift_issues.end(),
                            [](const FidelityIssue& issue) {
                                return issue.kind == "swiftui-browser-box-paint-rect" &&
                                    !issue.informational;
                            }));

    DesignIR raster_ir;
    raster_ir.root.type = "frame";
    IRNode captured_knob;
    captured_knob.type = "knob";
    captured_knob.name = "Captured knob";
    captured_knob.audio_widget = AudioWidgetType::knob;
    captured_knob.attributes["captured_raster_dpr"] = "2";
    captured_knob.attributes["captured_raster_origin_x_px"] = "12";
    captured_knob.attributes["captured_raster_origin_y_px"] = "18";
    captured_knob.attributes["browser_box_paint_dpr"] = "2";
    captured_knob.attributes["browser_box_paint_left_px"] = "12";
    captured_knob.attributes["browser_box_paint_top_px"] = "18";
    captured_knob.attributes["browser_box_paint_right_px"] = "112";
    captured_knob.attributes["browser_box_paint_bottom_px"] = "118";
    captured_knob.attributes["knob_ind_r_in"] = "0.2";
    captured_knob.attributes["knob_ind_r_out"] = "0.8";
    captured_knob.attributes["knob_ind_w"] = "0.04";
    captured_knob.attributes["knob_ind_color"] = "#ffffff";
    captured_knob.attributes["knob_ind_outline_color"] = "#000000";
    captured_knob.attributes["knob_ind_outline_w"] = "1";
    raster_ir.root.children.push_back(std::move(captured_knob));
    const auto raster_cpp = generate_pulp_cpp(raster_ir, raster_ir.asset_manifest, {});
    CHECK(raster_cpp.source.find("set_captured_raster_origin(6.0f, 9.0f, 2.0f)") !=
          std::string::npos);
    CHECK(raster_cpp.source.find("set_captured_indicator(") != std::string::npos);
    CHECK(raster_cpp.source.find("set_captured_indicator_outline(") != std::string::npos);

    // A recovered pointer can be geometric evidence without an authored paint
    // color. Generated C++ must leave that fallback theme-resolvable rather
    // than introducing a second hardcoded widget color.
    raster_ir.root.children[0].attributes.erase("knob_ind_color");
    const auto tokenized_pointer_cpp =
        generate_pulp_cpp(raster_ir, raster_ir.asset_manifest, {});
    CHECK(tokenized_pointer_cpp.source.find(
              "resolve_color(\"knob.thumb\", pulp::canvas::Color::rgba8(235, 235, 235))") !=
          std::string::npos);
    std::vector<FidelityIssue> raster_swift_issues;
    SwiftExportOptions raster_swift_options;
    raster_swift_options.fidelity_report = &raster_swift_issues;
    (void)generate_pulp_swift(raster_ir, raster_ir.asset_manifest, raster_swift_options);
    for (const char* kind : {"swiftui-captured-raster-origin",
                             "swiftui-knob-captured-indicator"}) {
        CHECK(std::any_of(raster_swift_issues.begin(), raster_swift_issues.end(),
                          [kind](const FidelityIssue& issue) {
                              return issue.kind == kind && !issue.informational;
                          }));
    }

    std::vector<FidelityIssue> swift_issues;
    SwiftExportOptions swift_options;
    swift_options.fidelity_report = &swift_issues;
    (void)generate_pulp_swift(ir, ir.asset_manifest, swift_options);
    REQUIRE_FALSE(swift_issues.empty());
    CHECK(std::any_of(swift_issues.begin(), swift_issues.end(),
                      [](const FidelityIssue& issue) {
                          return issue.kind == "swiftui-background-position" &&
                              issue.detail.find("0 0, 2px 2px") != std::string::npos;
                      }));
}

TEST_CASE("the web-compat JS emitter carries white-space nowrap",
          "[view][import][codegen][white-space]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRNode label;
    label.type = "text";
    label.name = "single-line";
    label.text_content = "never wrap this";
    label.style.white_space = "nowrap";
    ir.root.children.push_back(std::move(label));

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);

    CHECK(js.find(".style.whiteSpace = 'nowrap'") != std::string::npos);

    opts.mode = CodeGenMode::bridge_native_js;
    const auto native_js = generate_pulp_js(ir, opts);
    const auto native_white_space = native_js.find("setWhiteSpace(");
    REQUIRE(native_white_space != std::string::npos);
    CHECK(native_js.substr(native_white_space, 120).find("'nowrap'") !=
          std::string::npos);
}

TEST_CASE("web-compat attributed text keeps responsive paragraph wrapping",
          "[view][import][codegen][attributed][web-compat][wrapping]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRNode paragraph;
    paragraph.type = "text";
    paragraph.name = "paragraph";
    paragraph.text_content = "mixed style paragraph";
    IRTextRun emphasized;
    emphasized.start = 0;
    emphasized.end = 5;
    emphasized.font_weight = 700;
    paragraph.text_runs.push_back(std::move(emphasized));
    ir.root.children.push_back(paragraph);

    paragraph.name = "nowrap";
    paragraph.style.white_space = "nowrap";
    ir.root.children.push_back(std::move(paragraph));

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);

    const auto paragraph_at = js.find("setTextRuns(paragraph0._id");
    REQUIRE(paragraph_at != std::string::npos);
    CHECK(js.find("setMultiLine(paragraph0._id, true)", paragraph_at) !=
          std::string::npos);
    const auto nowrap_at = js.find("setTextRuns(nowrap1._id");
    REQUIRE(nowrap_at != std::string::npos);
    CHECK(js.find("setMultiLine(nowrap1._id, true)", nowrap_at) ==
          std::string::npos);
}

// The colour the DESIGN drew a control in. Without it the widget falls back to
// the host theme's accent, so a panel whose author chose lilac renders its knobs
// in whatever the surrounding app uses — the palette reaches the panel and stops
// at its controls. Observed as the same ui.js rendering periwinkle in one host
// and mint in another.
//
// The IR carries this per control and the native lane already consumed it; only
// the web-compat lane was silent.
TEST_CASE("the web-compat lane emits the design's accent for a control",
          "[view][import][codegen][design-accent]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRNode knob;
    knob.type = "frame";
    knob.name = "CUTOFF";
    knob.audio_widget = AudioWidgetType::knob;
    knob.attributes["binding"] = "param_1";
    knob.attributes["design_accent"] = "#d6b8ff";
    ir.root.children.push_back(std::move(knob));

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);

    CHECK(js.find("setAccentColor(") != std::string::npos);
    CHECK(js.find("#d6b8ff") != std::string::npos);
}

// A control with no design accent must NOT be forced to one: the host theme is
// the correct fallback, and emitting an empty colour would blank the widget.
TEST_CASE("a control with no design accent is left to the theme",
          "[view][import][codegen][design-accent]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRNode knob;
    knob.type = "frame";
    knob.audio_widget = AudioWidgetType::knob;
    knob.attributes["binding"] = "param_1";
    ir.root.children.push_back(std::move(knob));

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);

    CHECK(js.find("setAccentColor(") == std::string::npos);
}

TEST_CASE("generated JS keeps durable per-control indicator colours without sprites",
          "[view][import][codegen][indicator-precedence]") {
    const auto make_ir = [](bool per_control, bool panel_fallback) {
        DesignIR ir;
        ir.root.type = "frame";
        ir.root.name = "Root";

        IRNode knob;
        knob.type = "frame";
        knob.name = "DillaKnob";
        knob.audio_widget = AudioWidgetType::knob;
        knob.style.width = 86.0f;
        knob.style.height = 86.0f;
        knob.attributes["knob_ind_r_in"] = "0.55";
        knob.attributes["knob_ind_r_out"] = "0.91";
        knob.attributes["knob_ind_w"] = "0.04";
        if (per_control)
            knob.attributes["knob_ind_color"] = "#ffffffff";
        if (per_control) {
            knob.attributes["knob_ind_outline_color"] = "#000000ff";
            knob.attributes["knob_ind_outline_w"] = "1";
        }
        if (panel_fallback)
            knob.attributes["design_indicator"] = "#ff0000ff";
        // Deliberately no asset_path: portable/self-contained persistence may
        // prune importer-time sprite paths while retaining the authored pointer.
        ir.root.children.push_back(std::move(knob));

        IRNode fader;
        fader.type = "frame";
        fader.name = "DillaFader";
        fader.audio_widget = AudioWidgetType::fader;
        fader.style.width = 180.0f;
        fader.style.height = 24.0f;
        if (per_control)
            fader.attributes["fader_ind_color"] = "#28dcf0ff";
        if (panel_fallback)
            fader.attributes["design_indicator"] = "#ff0000ff";
        // Deliberately no fader body/indicator asset paths for the same reason.
        ir.root.children.push_back(std::move(fader));
        return ir;
    };

    for (const auto mode : {CodeGenMode::web_compat,
                            CodeGenMode::bridge_native_js}) {
        CodeGenOptions opts;
        opts.mode = mode;
        opts.include_comments = false;

        const auto authored = generate_pulp_js(make_ir(true, true), opts);
        INFO(authored);
        const auto knob_at = authored.find("setKnobCapturedIndicator(");
        const auto fader_at = authored.rfind("setFaderSkin(");
        REQUIRE(knob_at != std::string::npos);
        REQUIRE(fader_at != std::string::npos);
        const auto knob_stmt = authored.substr(
            knob_at, authored.find('\n', knob_at) - knob_at);
        const auto fader_stmt = authored.substr(
            fader_at, authored.find('\n', fader_at) - fader_at);
        CHECK(knob_stmt.find("#ffffffff") != std::string::npos);
        CHECK(knob_stmt.find("#000000ff") != std::string::npos);
        CHECK(knob_stmt.find(", 1)") != std::string::npos);
        CHECK(fader_stmt.find("#28dcf0ff") != std::string::npos);
        CHECK(knob_stmt.find(", true,") != std::string::npos);
        CHECK(fader_stmt.find(", true)") != std::string::npos);
        CHECK(knob_stmt.find("#ff0000ff") == std::string::npos);
        CHECK(fader_stmt.find("#ff0000ff") == std::string::npos);
        CHECK(authored.find("setKnobSpriteStrip(") == std::string::npos);
        CHECK(authored.find("setFaderCapturedArt(") == std::string::npos);

        auto invalid_ir = make_ir(true, true);
        invalid_ir.root.children[0].attributes["knob_ind_color"] = "rgb(nope)";
        invalid_ir.root.children[1].attributes["fader_ind_color"] = "rgb(255)";
        const auto invalid = generate_pulp_js(invalid_ir, opts);
        INFO(invalid);
        const auto invalid_knob_at = invalid.find("setKnobCapturedIndicator(");
        const auto invalid_fader_at = invalid.find("setFaderSkin(");
        REQUIRE(invalid_knob_at != std::string::npos);
        REQUIRE(invalid_fader_at != std::string::npos);
        const auto invalid_knob_stmt = invalid.substr(
            invalid_knob_at, invalid.find('\n', invalid_knob_at) - invalid_knob_at);
        const auto invalid_fader_stmt = invalid.substr(
            invalid_fader_at, invalid.find('\n', invalid_fader_at) - invalid_fader_at);
        CHECK(invalid_knob_stmt.find("#ff0000ff") != std::string::npos);
        CHECK(invalid_fader_stmt.find("#ff0000ff") != std::string::npos);
        CHECK(invalid_knob_stmt.find(", false,") != std::string::npos);
        CHECK(invalid_fader_stmt.find(", false)") != std::string::npos);

        const auto panel = generate_pulp_js(make_ir(false, true), opts);
        INFO(panel);
        const auto panel_knob_at = panel.find("setKnobCapturedIndicator(");
        const auto panel_fader_at = panel.find("setFaderSkin(");
        REQUIRE(panel_knob_at != std::string::npos);
        REQUIRE(panel_fader_at != std::string::npos);
        CHECK(panel.substr(panel_knob_at,
                           panel.find('\n', panel_knob_at) - panel_knob_at)
                  .find("#ff0000ff") != std::string::npos);
        const auto panel_knob_stmt = panel.substr(
            panel_knob_at, panel.find('\n', panel_knob_at) - panel_knob_at);
        const auto panel_fader_stmt = panel.substr(
            panel_fader_at, panel.find('\n', panel_fader_at) - panel_fader_at);
        CHECK(panel_knob_stmt.find(", false,") != std::string::npos);
        CHECK(panel_fader_stmt.find("#ff0000ff") != std::string::npos);
        CHECK(panel_fader_stmt.find(", false)") != std::string::npos);

        const auto themed = generate_pulp_js(make_ir(false, false), opts);
        INFO(themed);
        const auto themed_knob_at = themed.find("setKnobCapturedIndicator(");
        REQUIRE(themed_knob_at != std::string::npos);
        CHECK(themed.substr(themed_knob_at,
                            themed.find('\n', themed_knob_at) - themed_knob_at)
                  .find(", '', ") != std::string::npos);
        CHECK(themed.substr(themed_knob_at,
                            themed.find('\n', themed_knob_at) - themed_knob_at)
                  .find(", false,") != std::string::npos);
        // No authored track/fill/thumb means no synthetic fader skin: the
        // runtime's control.thumb/theme remains the authority.
        CHECK(themed.find("setFaderSkin(") == std::string::npos);
    }
}

TEST_CASE("generated fader skin keeps sampled and isolated control colors above panel fallbacks",
          "[view][import][codegen][indicator-precedence]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRNode fader;
    fader.type = "frame";
    fader.name = "LayeredFader";
    fader.audio_widget = AudioWidgetType::fader;
    fader.style.width = 24.0f;
    fader.style.height = 120.0f;
    fader.attributes["design_track"] = "#110000ff";
    fader.attributes["design_accent"] = "#220000ff";
    fader.attributes["design_indicator"] = "#330000ff";
    fader.attributes["skin_track_color"] = "#001100ff";
    fader.attributes["skin_fill_color"] = "#002200ff";
    fader.attributes["skin_thumb_color"] = "#003300ff";
    fader.attributes["fader_ind_color"] = "#28dcf0ff";
    ir.root.children.push_back(std::move(fader));

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    opts.include_comments = false;
    opts.skin_faders = true;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);

    const auto panel = js.find("#330000ff");
    const auto sampled = js.find("#003300ff");
    const auto isolated = js.find("#28dcf0ff");
    REQUIRE(panel != std::string::npos);
    REQUIRE(sampled != std::string::npos);
    REQUIRE(isolated != std::string::npos);
    CHECK(panel < sampled);
    CHECK(sampled < isolated);

    const auto statement_at = [&](std::size_t color_at) {
        const auto begin = js.rfind("setFaderSkin(", color_at);
        REQUIRE(begin != std::string::npos);
        const auto end = js.find('\n', color_at);
        REQUIRE(end != std::string::npos);
        return js.substr(begin, end - begin);
    };
    const auto panel_statement = statement_at(panel);
    const auto sampled_statement = statement_at(sampled);
    const auto isolated_statement = statement_at(isolated);
    CHECK(panel_statement.find(", false)") != std::string::npos);
    CHECK(sampled_statement.find(", true)") != std::string::npos);
    CHECK(isolated_statement.find(", true)") != std::string::npos);
}

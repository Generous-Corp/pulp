#include "test_design_import_shared.hpp"
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/view/pointer_dispatch.hpp>

// Yoga-constrained audio widgets, container padding, multi-line text and font
// lowering, knob tapers, and the web-compat parameter-gesture path.

TEST_CASE("generate_pulp_js bridge_native_js mode handles audio widgets with Yoga constraints", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";
    ir.root.style.width = 300.0f;

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.audio_default = 0.75f;
    ir.root.children.push_back(knob);

    IRNode fader;
    fader.type = "fader";
    fader.name = "MixFader";
    fader.audio_widget = AudioWidgetType::fader;
    fader.audio_label = "Mix";
    fader.audio_default = 0.5f;
    ir.root.children.push_back(fader);

    IRNode meter;
    meter.type = "meter";
    meter.name = "OutputMeter";
    meter.audio_widget = AudioWidgetType::meter;
    meter.audio_label = "Out";
    ir.root.children.push_back(meter);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    auto js = generate_pulp_js(ir, opts);

    // Knob with wrapper column and proper sizing (IDs get numeric suffixes)
    REQUIRE(js.find("createKnob('GainKnob") != std::string::npos);
    REQUIRE(js.find("setLabel('GainKnob") != std::string::npos);
    REQUIRE(js.find("'Gain'") != std::string::npos);
    REQUIRE(js.find("setValue('GainKnob") != std::string::npos);
    // Knob size >= 56 (minimum)
    REQUIRE(js.find("'width', 56)") != std::string::npos);

    // Fader with min width >= 40, label as separate element
    REQUIRE(js.find("createFader('MixFader") != std::string::npos);
    REQUIRE(js.find("createLabel('MixFader") != std::string::npos);  // Separate label
    REQUIRE(js.find("'Mix'") != std::string::npos);
    REQUIRE(js.find("'width', 40)") != std::string::npos);

    // Meter with separate label (no built-in setLabel for Meter)
    REQUIRE(js.find("createMeter('OutputMeter") != std::string::npos);
    REQUIRE(js.find("'Out'") != std::string::npos);
    REQUIRE(js.find("setMeterLevel") != std::string::npos);
}

// ── Figma-plugin fidelity: padding, wrapped text, font lowering, knob tapers ──

TEST_CASE("parse_design_ir_json parses nested padding object", "[view][import][issue-3192]") {
    // The figma-plugin export ships container padding as a nested
    // {top,right,bottom,left} object. The parser previously only understood a
    // uniform float or camelCase per-side keys, so the nested form was dropped
    // (parsed as 0) and content hugged the panel edge.
    const auto json = std::string{R"json({
        "type": "frame",
        "name": "Panel",
        "layout": { "padding": { "top": 24, "right": 32, "bottom": 24, "left": 32 } }
    })json"};
    const auto parsed = parse_design_ir_json(json);
    REQUIRE(parsed.root.layout.padding_top == Catch::Approx(24.0f));
    REQUIRE(parsed.root.layout.padding_right == Catch::Approx(32.0f));
    REQUIRE(parsed.root.layout.padding_bottom == Catch::Approx(24.0f));
    REQUIRE(parsed.root.layout.padding_left == Catch::Approx(32.0f));
}

TEST_CASE("parse_design_ir_json keeps uniform-float and per-side padding forms",
          "[view][import][issue-3192]") {
    // Back-compat: the legacy float and camelCase per-side forms must keep
    // working alongside the new nested-object form.
    const auto uniform = parse_design_ir_json(
        R"json({ "type": "frame", "layout": { "padding": 10 } })json");
    REQUIRE(uniform.root.layout.padding_top == Catch::Approx(10.0f));
    REQUIRE(uniform.root.layout.padding_left == Catch::Approx(10.0f));

    const auto per_side = parse_design_ir_json(
        R"json({ "type": "frame", "layout": { "paddingTop": 5, "paddingLeft": 7 } })json");
    REQUIRE(per_side.root.layout.padding_top == Catch::Approx(5.0f));
    REQUIRE(per_side.root.layout.padding_left == Catch::Approx(7.0f));
    REQUIRE(per_side.root.layout.padding_right == Catch::Approx(0.0f));
}

TEST_CASE("native codegen emits container padding (issue-3192)", "[view][import][issue-3192]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 300.0f;
    ir.root.layout.padding_top = 24.0f;
    ir.root.layout.padding_right = 32.0f;
    ir.root.layout.padding_bottom = 24.0f;
    ir.root.layout.padding_left = 32.0f;

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    // Non-uniform padding → per-side setFlex calls with the exact values.
    REQUIRE(js.find("'padding_top', 24") != std::string::npos);
    REQUIRE(js.find("'padding_right', 32") != std::string::npos);
    REQUIRE(js.find("'padding_bottom', 24") != std::string::npos);
    REQUIRE(js.find("'padding_left', 32") != std::string::npos);
}

TEST_CASE("native codegen wraps multi-line text at its design width (issue-3192)",
          "[view][import][issue-3192]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.style.width = 800.0f;

    // A subtitle paragraph: design box is wider AND taller than one line, so it
    // should wrap inside its width instead of overflowing.
    IRNode subtitle;
    subtitle.type = "text";
    subtitle.name = "Subtitle";
    subtitle.text_content = "A long subtitle that should soft-wrap inside its box";
    subtitle.style.width = 720.0f;
    subtitle.style.height = 26.0f;     // two lines at 11px
    subtitle.style.font_size = 11.0f;
    ir.root.children.push_back(subtitle);

    // A title: same font but a one-line-high box → must stay single line
    // (no forced wrap box) so it doesn't wrap when Pulp's metrics run wide.
    IRNode title;
    title.type = "text";
    title.name = "Title";
    title.text_content = "Title that fits one line";
    title.style.width = 284.0f;
    title.style.height = 22.0f;        // one line at 18px
    title.style.font_size = 18.0f;
    title.style.font_weight = 600;
    ir.root.children.push_back(title);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    // Subtitle: bounded width + multi-line so it wraps.
    REQUIRE(js.find("setFlex('Subtitle") != std::string::npos);
    REQUIRE(js.find("'width', 720") != std::string::npos);
    REQUIRE(js.find("setMultiLine('Subtitle") != std::string::npos);

    // Title: NO hard width / multi-line box (stays single line). It still gets a
    // min_width, but must not be forced into a wrap box.
    REQUIRE(js.find("setMultiLine('Title") == std::string::npos);
    REQUIRE(js.find("'width', 284") == std::string::npos);
}

TEST_CASE("native codegen preserves the browser's captured line-breaking decision",
          "[view][import][browser-capture][text]") {
    if (pulp::canvas::resolved_face_identity("Inter", 400.0f).empty())
        SKIP("captured line-breaking requires a text-shaping backend");
    DesignIR ir;
    ir.source = DesignSource::html;
    ir.root.type = "frame";
    ir.root.name = "Panel";

    IRNode single;
    single.type = "text";
    single.name = "Single";
    single.text_content = "01  RATE";
    single.style.width = 53.6719f;
    single.style.height = 40.0f; // tall padding must not override captured one-line layout
    single.style.font_size = 12.0f;
    single.style.font_family = "Inter";
    single.text_line_boxes.push_back({0.0f, 0.0f, 53.6719f, 15.0f, 0, 8});
    single.text_layout_basis = IRTextLayoutBasis{
        53.6719f, pulp::canvas::resolved_face_identity("Inter", 400.0f)};
    ir.root.children.push_back(single);

    IRNode wrapped;
    wrapped.type = "text";
    wrapped.name = "Wrapped";
    wrapped.text_content = "alpha beta";
    wrapped.style.width = 40.0f;
    wrapped.style.height = 15.0f; // geometry alone would not infer wrapping
    wrapped.style.font_size = 12.0f;
    wrapped.style.font_family = "Inter";
    wrapped.style.font_style = "italic";
    wrapped.style.text_decoration = "underline";
    wrapped.style.text_align = "center";
    wrapped.style.line_height = 18.0f;
    wrapped.text_line_boxes.push_back({5.0f, 0.0f, 30.0f, 15.0f, 0, 5});
    wrapped.text_line_boxes.push_back({8.0f, 15.0f, 24.0f, 15.0f, 6, 4});
    wrapped.text_layout_basis = IRTextLayoutBasis{
        40.0f, pulp::canvas::resolved_face_identity(
                   "Inter", 400.0f, pulp::canvas::FontSlant::Italic)};
    IRTextRun wrapped_red;
    wrapped_red.start = 0;
    wrapped_red.end = 5;
    wrapped_red.color = "#ff0000";
    wrapped.text_runs.push_back(wrapped_red);
    IRTextRun wrapped_green;
    wrapped_green.start = 6;
    wrapped_green.end = 10;
    wrapped_green.color = "#00ff00";
    wrapped_green.font_weight = 700;
    wrapped_green.font_family = "Courier";
    wrapped_green.font_style = "oblique 12deg";
    wrapped_green.text_decoration = "none";
    wrapped.text_runs.push_back(wrapped_green);
    ir.root.children.push_back(wrapped);

    IRNode stale;
    stale.type = "text";
    stale.name = "Stale";
    stale.text_content = "stale basis";
    stale.style.width = 80.0f;
    stale.style.height = 15.0f;
    stale.style.font_size = 12.0f;
    stale.text_line_boxes.push_back({0.0f, 0.0f, 80.0f, 15.0f, 0, 11});
    stale.text_layout_basis = IRTextLayoutBasis{80.0f, ""};
    ir.root.children.push_back(stale);

    IRNode deferred_face;
    deferred_face.type = "text";
    deferred_face.name = "DeferredFace";
    deferred_face.text_content = "bundled face";
    deferred_face.style.width = 72.0f;
    deferred_face.style.height = 15.0f;
    deferred_face.style.font_size = 12.0f;
    deferred_face.style.font_family = "Not Registered Until Runtime";
    deferred_face.text_line_boxes.push_back({0.0f, 0.0f, 72.0f, 15.0f, 0, 12});
    deferred_face.text_layout_basis = IRTextLayoutBasis{
        72.0f, "captured-face-id-not-known-to-generator"};
    ir.root.children.push_back(deferred_face);

    IRNode nowrap;
    nowrap.type = "text";
    nowrap.name = "NoWrap";
    nowrap.text_content = "alpha beta";
    nowrap.style.width = 40.0f;
    nowrap.style.height = 40.0f;
    nowrap.style.font_size = 12.0f;
    nowrap.style.font_family = "Inter";
    nowrap.style.white_space = "nowrap";
    nowrap.text_line_boxes.push_back({0.0f, 0.0f, 40.0f, 15.0f, 0, 10});
    nowrap.text_layout_basis = IRTextLayoutBasis{
        40.0f, pulp::canvas::resolved_face_identity("Inter", 400.0f)};
    ir.root.children.push_back(nowrap);

    IRNode ellipsis;
    ellipsis.type = "text";
    ellipsis.name = "Ellipsis";
    ellipsis.text_content = "long label";
    ellipsis.style.width = 30.0f;
    ellipsis.style.height = 20.0f;
    ellipsis.style.font_size = 12.0f;
    ellipsis.style.font_family = "Inter";
    ellipsis.style.white_space = "nowrap";
    ellipsis.style.text_overflow = "ellipsis";
    ellipsis.text_line_boxes.push_back({0.0f, 0.0f, 30.0f, 15.0f, 0, 10});
    ellipsis.text_layout_basis = IRTextLayoutBasis{
        30.0f, pulp::canvas::resolved_face_identity("Inter", 400.0f)};
    ir.root.children.push_back(ellipsis);

    IRNode uncached_ellipsis = ellipsis;
    uncached_ellipsis.name = "UncachedEllipsis";
    uncached_ellipsis.text_content = "uncached label";
    uncached_ellipsis.style.text_align = "right";
    uncached_ellipsis.text_line_boxes.clear();
    uncached_ellipsis.text_layout_basis.reset();
    ir.root.children.push_back(uncached_ellipsis);

    IRNode styled_single;
    styled_single.type = "text";
    styled_single.name = "StyledSingle";
    styled_single.text_content = "abcd";
    styled_single.style.width = 25.0f;
    styled_single.style.height = 20.0f;
    styled_single.style.font_size = 12.0f;
    styled_single.style.font_family = "Inter";
    styled_single.style.white_space = "nowrap";
    styled_single.style.text_transform = "uppercase";
    styled_single.style.text_overflow = "ellipsis";
    styled_single.text_line_boxes.push_back({0.0f, 0.0f, 60.0f, 15.0f, 0, 4});
    styled_single.text_layout_basis = IRTextLayoutBasis{
        25.0f, pulp::canvas::resolved_face_identity("Inter", 400.0f)};
    IRTextRun red_run;
    red_run.start = 0;
    red_run.end = 2;
    red_run.color = "#ff0000";
    styled_single.text_runs.push_back(red_run);
    IRTextRun green_run;
    green_run.start = 2;
    green_run.end = 4;
    green_run.color = "#00ff00";
    styled_single.text_runs.push_back(green_run);
    ir.root.children.push_back(styled_single);

    IRNode oblique = single;
    oblique.name = "Oblique";
    oblique.text_content = "slanted";
    oblique.style.width = 50.0f;
    oblique.style.font_style = "oblique 12deg";
    oblique.text_line_boxes = {{0.0f, 0.0f, 50.0f, 15.0f, 0, 7}};
    oblique.text_layout_basis = IRTextLayoutBasis{
        50.0f, pulp::canvas::resolved_face_identity(
                   "Inter", 400.0f, pulp::canvas::FontSlant::Oblique)};
    ir.root.children.push_back(oblique);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    const auto single_ws = js.find("setWhiteSpace('Single");
    CHECK(single_ws == std::string::npos);
    CHECK(js.find("setCapturedLineBoxes('Single") != std::string::npos);
    CHECK(js.find("setMultiLine('Single") == std::string::npos);
    const auto wrapped_cache = js.find("setCapturedLineBoxes('Wrapped");
    const auto wrapped_runs = js.find("setTextRuns('Wrapped");
    REQUIRE(wrapped_cache != std::string::npos);
    REQUIRE(wrapped_runs != std::string::npos);
    CHECK(wrapped_runs < wrapped_cache);
    const auto wrapped_style = js.find("setFontStyle('Wrapped");
    const auto wrapped_line_height = js.find("setLineHeight('Wrapped");
    REQUIRE(wrapped_style != std::string::npos);
    REQUIRE(wrapped_line_height != std::string::npos);
    CHECK(wrapped_style < wrapped_cache);
    CHECK(wrapped_line_height < wrapped_cache);
    CHECK(js.substr(wrapped_cache, 320).find("start: 6, length: 4") !=
          std::string::npos);
    CHECK(js.find("setMultiLine('Wrapped") != std::string::npos);
    CHECK(js.find("setTextAlign('Wrapped") == std::string::npos);
    CHECK(js.find("setCapturedLineBoxes('Stale") == std::string::npos);
    CHECK(js.find("setWhiteSpace('Stale") == std::string::npos);
    CHECK(js.find("setMultiLine('Stale") != std::string::npos);
    CHECK(js.find("setCapturedLineBoxes('DeferredFace") != std::string::npos);
    CHECK(js.find("setCapturedLineBoxes('NoWrap") != std::string::npos);
    CHECK(js.find("setWhiteSpace('NoWrap") != std::string::npos);
    CHECK(js.find("setMultiLine('NoWrap") == std::string::npos);
    CHECK(js.find("setCapturedLineBoxes('Ellipsis") != std::string::npos);
    CHECK(js.find("setMultiLine('Ellipsis") == std::string::npos);
    CHECK(js.find("setTextOverflow('Ellipsis") != std::string::npos);
    const auto ellipsis_width = js.find("setFlex('Ellipsis");
    REQUIRE(ellipsis_width != std::string::npos);
    CHECK(js.find("'width', 30", ellipsis_width) != std::string::npos);
    CHECK(js.find("setCapturedLineBoxes('UncachedEllipsis") == std::string::npos);
    CHECK(js.find("setMultiLine('UncachedEllipsis") == std::string::npos);
    CHECK(js.find("setTextAlign('UncachedEllipsis", 0) != std::string::npos);
    const auto uncached_ellipsis_width = js.find("setFlex('UncachedEllipsis");
    REQUIRE(uncached_ellipsis_width != std::string::npos);
    CHECK(js.find("'width', 30", uncached_ellipsis_width) != std::string::npos);
    CHECK(js.find("setCapturedLineBoxes('StyledSingle") != std::string::npos);
    CHECK(js.find("setMultiLine('StyledSingle") == std::string::npos);
    CHECK(js.find("setTextRuns('StyledSingle") != std::string::npos);
    CHECK(js.find("setTextRuns('StyledSingle") <
          js.find("setCapturedLineBoxes('StyledSingle"));
    CHECK(js.find("setWhiteSpace('StyledSingle") != std::string::npos);
    CHECK(js.find("setFontStyle('Oblique") != std::string::npos);

    const auto cpp = generate_pulp_cpp(ir, ir.asset_manifest, {});
    CHECK(cpp.source.find("pulp::canvas::AttributedString") != std::string::npos);
    CHECK(cpp.source.find("span.font_family = \"Courier\"") !=
          std::string::npos);
    CHECK(cpp.source.find("span.font_weight = 700") != std::string::npos);
    CHECK(cpp.source.find("span.font_slant = 2") != std::string::npos);
    CHECK(cpp.source.find("set_attributed_string(std::move(") !=
          std::string::npos);
    CHECK(cpp.source.find("set_cached_line_boxes(") != std::string::npos);

    ScriptEngine engine;
    View host;
    host.set_bounds({0, 0, 320, 200});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, host, store);
    bridge.load_script(js);

    Label* live_single = nullptr;
    Label* live_wrapped = nullptr;
    Label* live_nowrap = nullptr;
    Label* live_stale = nullptr;
    Label* live_ellipsis = nullptr;
    Label* live_styled_single = nullptr;
    Label* live_oblique = nullptr;
    const auto find_wrapped = [&](auto&& self, View& view) -> void {
        if (auto* label = dynamic_cast<Label*>(&view)) {
            if (label->text() == "01  RATE") live_single = label;
            if (label->text() == "alpha beta" && label->multi_line())
                live_wrapped = label;
            if (label->text() == "alpha beta" && !label->multi_line())
                live_nowrap = label;
            if (label->text() == "stale basis") live_stale = label;
            if (label->text() == "long label") live_ellipsis = label;
            if (label->text() == "abcd") live_styled_single = label;
            if (label->text() == "slanted") live_oblique = label;
        }
        for (size_t i = 0; i < view.child_count(); ++i)
            if (auto* child = view.child_at(i)) self(self, *child);
    };
    find_wrapped(find_wrapped, host);
    REQUIRE(live_single != nullptr);
    REQUIRE_FALSE(live_single->multi_line());
    REQUIRE(live_single->captured_wrap_fallback());
    REQUIRE(live_single->cached_line_boxes().size() == 1);
    REQUIRE(live_wrapped != nullptr);
    CHECK(live_wrapped->font_style() == 1);
    CHECK(live_wrapped->line_height() == Catch::Approx(18.0f));
    REQUIRE(live_wrapped->cached_line_boxes().size() == 2);
    CHECK(live_wrapped->cached_line_boxes()[0].start == 0);
    CHECK(live_wrapped->cached_line_boxes()[0].length == 5);
    CHECK(live_wrapped->cached_line_boxes()[1].start == 6);
    CHECK(live_wrapped->cached_line_boxes()[1].length == 4);
    live_wrapped->set_bounds({0, 0, 40.0f, 36.0f});
    Label::reset_line_break_path_counts();
    pulp::canvas::RecordingCanvas italic_cached_canvas;
    live_wrapped->paint(italic_cached_canvas);
    CHECK(Label::line_break_path_counts().reflowed == 1);
    const auto wrapped_fills = [&] {
        std::vector<pulp::canvas::DrawCommand> result;
        for (const auto& command : italic_cached_canvas.commands())
            if (command.type == pulp::canvas::DrawCommand::Type::fill_text)
                result.push_back(command);
        return result;
    }();
    REQUIRE(wrapped_fills.size() == 2);
    CHECK(wrapped_fills[0].text == "alpha");
    CHECK(wrapped_fills[1].text == "beta");
    CHECK(wrapped_fills[0].f[0] >= 0.0f);
    CHECK(wrapped_fills[1].f[0] >= 0.0f);
    bool saw_courier = false;
    bool saw_oblique_run = false;
    for (const auto& command : italic_cached_canvas.commands())
        if (command.type == pulp::canvas::DrawCommand::Type::set_font_full) {
            if (command.text == "Courier") saw_courier = true;
            if (command.text == "Courier" &&
                command.f[2] == Catch::Approx(2.0f))
                saw_oblique_run = true;
        }
    CHECK(saw_courier);
    CHECK(saw_oblique_run);
    CHECK(italic_cached_canvas.count(
              pulp::canvas::DrawCommand::Type::stroke_line) == 1);
    REQUIRE(live_stale != nullptr);
    CHECK(live_stale->multi_line());

    REQUIRE(live_ellipsis != nullptr);
    REQUIRE_FALSE(live_ellipsis->multi_line());
    REQUIRE(live_ellipsis->cached_line_boxes().size() == 1);
    host.layout_children();
    CHECK(live_ellipsis->bounds().width == Catch::Approx(30.0f));
    live_ellipsis->set_bounds({0, 0, 30.0f, 20.0f});
    pulp::canvas::RecordingCanvas ellipsis_canvas;
    live_ellipsis->paint(ellipsis_canvas);
    const auto ellipsis_text = std::find_if(
        ellipsis_canvas.commands().begin(), ellipsis_canvas.commands().end(),
        [](const auto& command) {
            return command.type == pulp::canvas::DrawCommand::Type::fill_text;
        });
    REQUIRE(ellipsis_text != ellipsis_canvas.commands().end());
    REQUIRE(ellipsis_text->text.size() >= 3);
    CHECK(ellipsis_text->text.ends_with("\xe2\x80\xa6"));
    CHECK(ellipsis_text->text != "long label");

    REQUIRE(live_styled_single != nullptr);
    REQUIRE_FALSE(live_styled_single->multi_line());
    REQUIRE_FALSE(live_styled_single->captured_wrap_fallback());
    live_styled_single->set_bounds({0, 0, 25.0f, 20.0f});
    pulp::canvas::RecordingCanvas styled_single_canvas;
    live_styled_single->paint(styled_single_canvas);
    std::string styled_single_text;
    for (const auto& command : styled_single_canvas.commands())
        if (command.type == pulp::canvas::DrawCommand::Type::fill_text)
            styled_single_text += command.text;
    CHECK(styled_single_text.ends_with("\xe2\x80\xa6"));
    CHECK(styled_single_text.find_first_of("abcd") == std::string::npos);

    REQUIRE(live_oblique != nullptr);
    CHECK(live_oblique->font_style() == 2);
    live_oblique->set_bounds({0, 0, 50.0f, 20.0f});
    Label::reset_line_break_path_counts();
    pulp::canvas::RecordingCanvas oblique_canvas;
    live_oblique->paint(oblique_canvas);
    CHECK(Label::line_break_path_counts().cached == 1);

    Label::reset_line_break_path_counts();
    live_single->set_bounds({0, 0, 53.6719f, 40.0f});
    pulp::canvas::RecordingCanvas cached_canvas;
    live_single->paint(cached_canvas);
    CHECK(Label::line_break_path_counts().cached == 1);
    const auto cached_text = std::find_if(
        cached_canvas.commands().begin(), cached_canvas.commands().end(),
        [](const auto& command) {
            return command.type == pulp::canvas::DrawCommand::Type::fill_text;
        });
    REQUIRE(cached_text != cached_canvas.commands().end());
    // CSS half-leading against the captured line box, using the same ink the
    // baseline then descends by: (15 - (ascent + descent)) / 2 + ascent. Inter
    // at 12px carries ~17.16px of ink, so a 15px box has NEGATIVE half-leading
    // (~-1.08) and the glyphs overflow it evenly, which CSS permits. The
    // earlier 11.7 encoded a rule of thumb -- 1.5 + 0.85 * 12 -- that measured
    // the surplus against the em box while descending by a real face ascent,
    // double-counting the gap between the two references.
    CHECK(cached_text->f[1] == Catch::Approx(12.2139f).margin(0.01f));

    live_single->set_text("01  SLOW");
    pulp::canvas::RecordingCanvas changed_text_canvas;
    live_single->paint(changed_text_canvas);
    CHECK(Label::line_break_path_counts().reflowed == 1);
    live_single->set_text("01  RATE");
    live_single->set_font_size(13.0f);
    pulp::canvas::RecordingCanvas changed_size_canvas;
    live_single->paint(changed_size_canvas);
    CHECK(Label::line_break_path_counts().reflowed == 2);
    live_single->set_font_size(12.0f);
    live_single->set_letter_spacing(1.0f);
    pulp::canvas::RecordingCanvas changed_tracking_canvas;
    live_single->paint(changed_tracking_canvas);
    CHECK(Label::line_break_path_counts().reflowed == 3);
    live_single->set_letter_spacing(0.0f);
    live_single->set_bounds({0, 0, 30.0f, 40.0f});
    pulp::canvas::RecordingCanvas reflow_canvas;
    live_single->paint(reflow_canvas);
    CHECK(Label::line_break_path_counts().reflowed == 4);

    // A stale browser cache must reflow with the typography that will be
    // painted. Choose a width between the plain and tracked advances so the
    // observable line count proves letter-spacing reached TextShaper rather
    // than merely invalidating the captured cache.
    constexpr std::string_view tracked_text = "alpha beta";
    auto& shaper = pulp::canvas::global_text_shaper();
    const auto plain_prepared = shaper.prepare(
        tracked_text, "Inter", 12.0f, 400, 0, 0.0f);
    const auto tracked_prepared = shaper.prepare(
        tracked_text, "Inter", 12.0f, 400, 0, 4.0f);
    REQUIRE(tracked_prepared.total_width() > plain_prepared.total_width());
    const float tracked_wrap_width =
        (plain_prepared.total_width() + tracked_prepared.total_width()) * 0.5f;
    live_single->set_text(std::string(tracked_text));
    live_single->set_letter_spacing(0.0f);
    live_single->set_bounds({0, 0, tracked_wrap_width, 40.0f});
    pulp::canvas::RecordingCanvas plain_wrap_canvas;
    live_single->paint(plain_wrap_canvas);
    CHECK(plain_wrap_canvas.count(
              pulp::canvas::DrawCommand::Type::fill_text) == 1);
    live_single->set_letter_spacing(4.0f);
    pulp::canvas::RecordingCanvas tracked_wrap_canvas;
    live_single->paint(tracked_wrap_canvas);
    CHECK(tracked_wrap_canvas.count(
              pulp::canvas::DrawCommand::Type::fill_text) == 2);

    REQUIRE(live_nowrap != nullptr);
    REQUIRE_FALSE(live_nowrap->multi_line());
    live_nowrap->set_bounds({0, 0, 40.0f, 40.0f});
    Label::reset_line_break_path_counts();
    pulp::canvas::RecordingCanvas nowrap_cached_canvas;
    live_nowrap->paint(nowrap_cached_canvas);
    CHECK(Label::line_break_path_counts().cached == 1);
    live_nowrap->set_text("gamma delta");
    pulp::canvas::RecordingCanvas nowrap_stale_canvas;
    live_nowrap->paint(nowrap_stale_canvas);
    CHECK(nowrap_stale_canvas.count(
              pulp::canvas::DrawCommand::Type::fill_text) == 1);

    auto direct = build_native_view_tree(ir, {}, {});
    REQUIRE(direct != nullptr);
    Label* direct_nowrap = nullptr;
    Label* direct_wrapped = nullptr;
    Label* direct_ellipsis = nullptr;
    Label* direct_styled = nullptr;
    const auto find_direct_nowrap = [&](auto&& self, View& view) -> void {
        if (auto* label = dynamic_cast<Label*>(&view)) {
            if (label->text() == "alpha beta" && !label->multi_line())
                direct_nowrap = label;
            if (label->text() == "alpha beta" && label->multi_line())
                direct_wrapped = label;
            if (label->text() == "long label") direct_ellipsis = label;
            if (label->text() == "abcd") direct_styled = label;
        }
        for (size_t i = 0; i < view.child_count(); ++i)
            if (auto* child = view.child_at(i)) self(self, *child);
    };
    find_direct_nowrap(find_direct_nowrap, *direct);
    REQUIRE(direct_nowrap != nullptr);
    REQUIRE(direct_wrapped != nullptr);
    direct_wrapped->set_bounds({0, 0, 40.0f, 36.0f});
    pulp::canvas::RecordingCanvas direct_wrapped_canvas;
    direct_wrapped->paint(direct_wrapped_canvas);
    CHECK(direct_wrapped_canvas.count(
              pulp::canvas::DrawCommand::Type::stroke_line) == 1);
    direct_nowrap->set_bounds({0, 0, 40.0f, 40.0f});
    Label::reset_line_break_path_counts();
    pulp::canvas::RecordingCanvas direct_cached_canvas;
    direct_nowrap->paint(direct_cached_canvas);
    CHECK(Label::line_break_path_counts().cached == 1);
    direct_nowrap->set_text("gamma delta");
    pulp::canvas::RecordingCanvas direct_stale_canvas;
    direct_nowrap->paint(direct_stale_canvas);
    CHECK(direct_stale_canvas.count(
              pulp::canvas::DrawCommand::Type::fill_text) == 1);

    REQUIRE(direct_ellipsis != nullptr);
    direct_ellipsis->set_bounds({0, 0, 30.0f, 20.0f});
    pulp::canvas::RecordingCanvas direct_ellipsis_canvas;
    direct_ellipsis->paint(direct_ellipsis_canvas);
    const auto direct_ellipsis_text = std::find_if(
        direct_ellipsis_canvas.commands().begin(),
        direct_ellipsis_canvas.commands().end(), [](const auto& command) {
            return command.type == pulp::canvas::DrawCommand::Type::fill_text;
        });
    REQUIRE(direct_ellipsis_text != direct_ellipsis_canvas.commands().end());
    CHECK(direct_ellipsis_text->text.ends_with("\xe2\x80\xa6"));

    REQUIRE(direct_styled != nullptr);
    REQUIRE(direct_styled->has_attributed_string());
    REQUIRE(direct_styled->cached_line_boxes().size() == 1);
    direct_styled->set_bounds({0, 0, 25.0f, 20.0f});
    pulp::canvas::RecordingCanvas direct_styled_canvas;
    direct_styled->paint(direct_styled_canvas);
    std::string direct_styled_text;
    for (const auto& command : direct_styled_canvas.commands())
        if (command.type == pulp::canvas::DrawCommand::Type::fill_text)
            direct_styled_text += command.text;
    CHECK(direct_styled_text.ends_with("\xe2\x80\xa6"));
    CHECK(direct_styled_text.find_first_of("abcd") == std::string::npos);

    CodeGenOptions web_opts;
    web_opts.mode = CodeGenMode::web_compat;
    const auto web_js = generate_pulp_js(ir, web_opts);
    CHECK(web_js.find(".style.textDecoration = 'underline';") !=
          std::string::npos);
    CHECK(web_js.find("textDecoration: 'none'") !=
          std::string::npos);
    CHECK(count_occurrences(
              web_js, ".style.textDecoration = 'underline';") == 1);

}

TEST_CASE("direct native text runs snap malformed byte offsets to UTF-8 boundaries",
          "[view][import][text-runs][utf8]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    IRNode label_node;
    label_node.type = "text";
    label_node.name = "Mixed";
    label_node.text_content = std::string("A") + "\xc3\xa9" + "B";
    label_node.style.width = 100.0f;
    label_node.style.height = 24.0f;
    label_node.style.font_size = 14.0f;
    IRTextRun malformed;
    malformed.start = 1;
    malformed.end = 2;
    malformed.font_weight = 700;
    label_node.text_runs.push_back(malformed);
    ir.root.children.push_back(std::move(label_node));

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    Label* label = nullptr;
    const auto find_label = [&](auto&& self, View& view) -> void {
        if (auto* candidate = dynamic_cast<Label*>(&view)) label = candidate;
        for (size_t i = 0; i < view.child_count(); ++i)
            if (auto* child = view.child_at(i)) self(self, *child);
    };
    find_label(find_label, *root);
    REQUIRE(label != nullptr);
    REQUIRE(label->attributed_span_count() == 3);
    label->set_bounds({0, 0, 100, 24});
    pulp::canvas::RecordingCanvas canvas;
    label->paint(canvas);
    std::string painted;
    for (const auto& command : canvas.commands())
        if (command.type == pulp::canvas::DrawCommand::Type::fill_text)
            painted += command.text;
    CHECK(painted == std::string("A") + "\xc3\xa9" + "B");
}

TEST_CASE("direct native text-run gaps retain inherited dominant typography",
          "[view][import][text-runs][inheritance]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.font_family = "Courier";
    ir.root.style.font_size = 18.0f;
    ir.root.style.font_weight = 500;
    ir.root.style.letter_spacing = 1.5f;
    ir.root.style.color = "#223344";
    IRNode label_node;
    label_node.type = "text";
    label_node.name = "Mixed";
    label_node.text_content = "base bold tail";
    label_node.style.width = 180.0f;
    label_node.style.height = 30.0f;
    IRTextRun run;
    run.start = 5;
    run.end = 9;
    run.font_weight = 700;
    label_node.text_runs.push_back(run);
    ir.root.children.push_back(std::move(label_node));

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    auto* label = dynamic_cast<Label*>(root->child_at(0));
    REQUIRE(label != nullptr);
    label->set_bounds({0, 0, 180, 30});
    pulp::canvas::RecordingCanvas canvas;
    label->paint(canvas);
    bool saw_base = false;
    bool saw_run = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != pulp::canvas::DrawCommand::Type::set_font_full) continue;
        if (command.text == "Courier" &&
            command.f[0] == Catch::Approx(18.0f) &&
            command.f[1] == Catch::Approx(500.0f) &&
            command.f[3] == Catch::Approx(1.5f)) saw_base = true;
        if (command.text == "Courier" &&
            command.f[0] == Catch::Approx(18.0f) &&
            command.f[1] == Catch::Approx(700.0f) &&
            command.f[3] == Catch::Approx(1.5f)) saw_run = true;
    }
    CHECK(saw_base);
    CHECK(saw_run);
}

TEST_CASE("web text-run gaps retain the node dominant typography",
          "[view][import][text-runs][web]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRNode label;
    label.type = "text";
    label.name = "Mixed";
    label.text_content = "base bold tail";
    label.style.font_family = "Inter";
    label.style.font_size = 18.0f;
    label.style.font_weight = 500;
    label.style.font_style = "italic";
    label.style.color = "#223344";
    label.style.letter_spacing = 1.5f;
    IRTextRun run;
    run.start = 5;
    run.end = 9;
    run.font_weight = 700;
    label.text_runs.push_back(run);
    ir.root.children.push_back(std::move(label));

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    const auto js = generate_pulp_js(ir, opts);
    INFO(js);
    CHECK(js.find("Mixed0.style.fontFamily = 'Inter';") != std::string::npos);
    CHECK(js.find("Mixed0.style.fontSize = '18px';") != std::string::npos);
    CHECK(js.find("Mixed0.style.fontWeight = '500';") != std::string::npos);
    CHECK(js.find("Mixed0.style.color = '#223344';") != std::string::npos);
    CHECK(js.find("setTextRuns(Mixed0._id, [{ start: 5, end: 9, fontWeight: 700 }])") != std::string::npos);
    CHECK(js.find("Mixed0_r0") == std::string::npos);
}

TEST_CASE("direct native styled paragraphs use responsive multiline layout",
          "[view][import][text-runs][multiline]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    IRNode paragraph;
    paragraph.type = "text";
    paragraph.name = "Paragraph";
    paragraph.text_content = "alpha beta gamma delta";
    paragraph.style.width = 40.0f;
    paragraph.style.height = 60.0f;
    paragraph.style.font_size = 12.0f;
    IRTextRun run;
    run.start = 6;
    run.end = 10;
    run.font_weight = 700;
    paragraph.text_runs.push_back(run);
    ir.root.children.push_back(std::move(paragraph));

    auto root = build_native_view_tree(ir, {}, {});
    REQUIRE(root != nullptr);
    Label* label = nullptr;
    const auto find_label = [&](auto&& self, View& view) -> void {
        if (auto* candidate = dynamic_cast<Label*>(&view)) label = candidate;
        for (size_t i = 0; i < view.child_count(); ++i)
            if (auto* child = view.child_at(i)) self(self, *child);
    };
    find_label(find_label, *root);
    REQUIRE(label != nullptr);
    CHECK(label->multi_line());
}

TEST_CASE("captured line decisions reject UTF-16 surrogate-pair splits",
          "[view][import][text-cache][utf16]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    IRNode label;
    label.type = "text";
    label.name = "Emoji";
    label.text_content = std::string("A") + "\xf0\x9f\x98\x80" + "B";
    label.style.width = 100.0f;
    label.style.height = 24.0f;
    label.style.font_size = 14.0f;
    label.text_line_boxes.push_back({0, 0, 100, 18, 1, 1});
    label.text_layout_basis = IRTextLayoutBasis{
        100.0f, pulp::canvas::resolved_face_identity("Inter", 400.0f)};
    ir.root.children.push_back(std::move(label));

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);
    CHECK(js.find("setCapturedLineBoxes('Emoji") == std::string::npos);
}

TEST_CASE("captured line decisions reject reordered and overlapping ranges",
          "[view][import][text-cache][ranges]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRNode label;
    label.type = "text";
    label.name = "Overlap";
    label.text_content = "alpha beta";
    label.style.width = 100.0f;
    label.text_line_boxes = {
        {0, 0, 50, 18, 0, 7},
        {0, 18, 50, 18, 6, 4},
    };
    label.text_layout_basis = IRTextLayoutBasis{100.0f, "Inter-Regular"};
    ir.root.children.push_back(std::move(label));

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);
    CHECK(js.find("setCapturedLineBoxes('Overlap") == std::string::npos);
}

TEST_CASE("native codegen emits font weight and family for text (issue-3192)",
          "[view][import][issue-3192]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";

    IRNode title;
    title.type = "text";
    title.name = "Title";
    title.text_content = "Bold Inter Title";
    title.style.font_size = 18.0f;
    title.style.font_weight = 600;
    title.style.font_family = "Inter";
    ir.root.children.push_back(title);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("setFontWeight('Title") != std::string::npos);
    REQUIRE(js.find("'600'") != std::string::npos);
    REQUIRE(js.find("setFontFamily('Title") != std::string::npos);
    REQUIRE(js.find("'Inter'") != std::string::npos);
}

TEST_CASE("native codegen log-tapers a Hz-unit knob's initial value (issue-3192)",
          "[view][import][issue-3192]") {
    // A frequency knob's value→angle map is logarithmic. The native silver knob
    // maps a 0..1 value linearly to its sweep, so the imported value must be the
    // LOG-normalized position — 880 Hz in [20, 20000] lands near center (~0.55),
    // not at the far end (a raw 880 would clamp to 1.0) and not at the linear
    // position (~0.04, indicator pointing the wrong way).
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";

    IRNode knob;
    knob.type = "knob";
    knob.name = "Cutoff";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Cutoff";
    knob.audio_min = 20.0f;
    knob.audio_max = 20000.0f;
    knob.audio_default = 880.0f;
    knob.attributes["units"] = "Hz";
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    // Compute the expected log-normalized value and assert the emitted setValue
    // matches it (and is nowhere near the raw 880 or the linear ~0.04).
    const float expected =
        (std::log(880.0f) - std::log(20.0f)) / (std::log(20000.0f) - std::log(20.0f));
    REQUIRE(expected == Catch::Approx(0.5478f).margin(0.01f));
    // The emitted value should be the log position, not the raw value. Only one
    // knob exists, so a "setValue('Cutoff" prefix uniquely identifies it.
    REQUIRE(js.find("setValue('Cutoff") != std::string::npos);
    REQUIRE(js.find("', 880") == std::string::npos);
    REQUIRE(js.find("', 0.54") != std::string::npos);
}

TEST_CASE("native codegen uses linear taper for non-frequency knobs (issue-3192)",
          "[view][import][issue-3192]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";

    IRNode knob;
    knob.type = "knob";
    knob.name = "Drive";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Drive";
    knob.audio_min = 0.0f;
    knob.audio_max = 10.0f;
    knob.audio_default = 5.0f;        // linear midpoint → 0.5
    knob.attributes["units"] = "dB";
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    const auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("setValue('Drive") != std::string::npos);
    REQUIRE(js.find("', 0.5)") != std::string::npos);
}

TEST_CASE("generate_pulp_js web-compat mode handles audio widgets", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.audio_min = 0.0f;
    knob.audio_max = 1.0f;
    knob.audio_default = 0.75f;
    knob.attributes["binding"] = "gain";
    knob.stable_anchor_id = "figma:1:2";
    ir.root.children.push_back(knob);

    IRNode meter;
    meter.type = "meter";
    meter.name = "OutputMeter";
    meter.audio_widget = AudioWidgetType::meter;
    meter.attributes["binding"] = "output";
    ir.root.children.push_back(meter);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    auto js = generate_pulp_js(ir, opts);

    // This asserted `createKnob({label, min, max, defaultValue})`, which is a
    // shape the bridge it targets cannot run: `createKnob(id, parentId)` reads
    // argument 0 as a string, so an options object lowered to an EMPTY id. The
    // knob registered as widgets_[""], parented at the session root rather than
    // its design parent, carried no anchor, and no binding was emitted at all —
    // the lowering produced a control that drove nothing. The expectation was
    // wrong, not the code.
    REQUIRE(js.find("document.createElement('knob')") != std::string::npos);
    REQUIRE(js.find("setLabel(") != std::string::npos);
    REQUIRE(js.find("'Gain'") != std::string::npos);
    REQUIRE(js.find("setValue(") != std::string::npos);
    REQUIRE(js.find("0.75") != std::string::npos);
    // A declared binding is why an audio widget exists.
    REQUIRE(js.find("bindWidgetToParam(") != std::string::npos);
    REQUIRE(js.find("'gain'") != std::string::npos);
    // The binding API only pushes host state into the widget. Canonical
    // DesignIR lowering must also route a user change back to the parameter.
    // Without this handler the control renders and turns but is acoustically
    // inert.
    REQUIRE(js.find(
        "'change', function (v) { setParam('gain', v); })") !=
        std::string::npos);
    REQUIRE(js.find("bindMeter(") != std::string::npos);
    REQUIRE(js.find("setParam('output'") == std::string::npos);
    REQUIRE(js.find("setAnchor(") != std::string::npos);
    // Appended before configuring: web-compat materializes the native widget on
    // mount, so property calls issued before appendChild are dropped.
    REQUIRE(js.find(".appendChild(") < js.find("setLabel("));
}

TEST_CASE("web-compat DesignIR knob writes user gestures to its parameter",
          "[view][import][runtime]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";
    ir.root.style.width = 240.0f;
    ir.root.style.height = 160.0f;

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.audio_min = 0.0f;
    knob.audio_max = 1.0f;
    knob.audio_default = 0.5f;
    knob.style.width = 80.0f;
    knob.style.height = 80.0f;
    knob.attributes["binding"] = "gain";
    ir.root.children.push_back(knob);

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 240, 160});
    pulp::state::StateStore store;
    pulp::state::ParamInfo gain;
    gain.id = 1;
    gain.name = "gain";
    gain.range = {0.0f, 1.0f, 0.5f};
    store.add_parameter(gain);
    std::vector<std::string> events;
    store.set_gesture_callbacks(
        [&](pulp::state::ParamID id) {
            REQUIRE(id == gain.id);
            events.push_back("begin");
        },
        [&](pulp::state::ParamID id) {
            REQUIRE(id == gain.id);
            events.push_back("end");
        });
    auto listener = store.add_listener(
        [&](pulp::state::ParamID id, float) {
            if (id == gain.id) events.push_back("value");
        },
        pulp::state::ListenerThread::Audio);
    WidgetBridge bridge(engine, root, store);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    bridge.load_script(generate_pulp_js(ir, opts));
    root.layout_children();

    Knob* live_knob = nullptr;
    const auto find_knob = [&](auto&& self, View& view) -> void {
        if (auto* found = dynamic_cast<Knob*>(&view)) {
            live_knob = found;
            return;
        }
        for (std::size_t i = 0; i < view.child_count() && !live_knob; ++i)
            if (auto* child = view.child_at(i)) self(self, *child);
    };
    find_knob(find_knob, root);
    REQUIRE(live_knob != nullptr);

    const float before = store.get_normalized(gain.id);
    REQUIRE(deliver_mouse_down(root, live_knob, {40.0f, 40.0f}, 0));
    deliver_mouse_drag(root, live_knob, {40.0f, 12.0f}, 0);
    deliver_mouse_up(root, live_knob, {40.0f, 12.0f}, 0, 1, {});
    REQUIRE(std::abs(store.get_normalized(gain.id) - before) > 1.0e-6f);
    REQUIRE(events == std::vector<std::string>{"begin", "value", "end"});

    // The real pointer router delivers Knob::on_mouse_event before
    // Knob::on_mouse_down. A reset must still write inside the gesture.
    events.clear();
    REQUIRE(deliver_mouse_down(
        root, live_knob, {40.0f, 40.0f}, 0, /*click_count=*/2));
    deliver_mouse_up(
        root, live_knob, {40.0f, 40.0f}, 0, /*click_count=*/2, {});
    REQUIRE(events == std::vector<std::string>{
                          "begin", "value", "end", "begin", "end"});
}

TEST_CASE("web-compat parameter gesture routing survives deferred creation and rebinding",
          "[view][import][runtime]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 240, 160});
    std::vector<std::string> events;
    Knob* live_knob = nullptr;

    {
        pulp::state::StateStore store;
        store.add_parameter({
            .id = 1,
            .name = "gain",
            .range = {0.0f, 1.0f, 0.5f},
        });
        store.add_parameter({
            .id = 2,
            .name = "tone",
            .range = {0.0f, 1.0f, 0.5f},
        });
        store.set_gesture_callbacks(
            [&](pulp::state::ParamID id) {
                events.push_back("begin:" + std::to_string(id));
            },
            [&](pulp::state::ParamID id) {
                events.push_back("end:" + std::to_string(id));
            });

        {
            WidgetBridge bridge(engine, root, store);
            bridge.load_script(
                "bindWidgetToParam('deferred', 'gain');"
                "createKnob('deferred', '');");
            live_knob = dynamic_cast<Knob*>(bridge.widget("deferred"));
            REQUIRE(live_knob != nullptr);

            live_knob->on_mouse_down({40.0f, 40.0f});
            bridge.load_script("bindWidgetToParam('deferred', 'tone');");
            live_knob->on_mouse_up({40.0f, 40.0f});
            REQUIRE(events == std::vector<std::string>{"begin:1", "end:1"});

            live_knob->on_mouse_down({40.0f, 40.0f});
            live_knob->on_mouse_up({40.0f, 40.0f});
            REQUIRE(events == std::vector<std::string>{
                                  "begin:1", "end:1", "begin:2", "end:2"});

            bridge.load_script(
                "createKnob('second', '');"
                "bindWidgetToParam('second', 'tone');");
            auto* second_knob =
                dynamic_cast<Knob*>(bridge.widget("second"));
            REQUIRE(second_knob != nullptr);
            events.clear();
            live_knob->on_mouse_down({40.0f, 40.0f});
            second_knob->on_mouse_down({40.0f, 40.0f});
            second_knob->on_mouse_up({40.0f, 40.0f});
            REQUIRE(events == std::vector<std::string>{"begin:2"});
            REQUIRE(store.open_gesture_count() == 1);
            live_knob->on_mouse_up({40.0f, 40.0f});
            REQUIRE(events ==
                    std::vector<std::string>{"begin:2", "end:2"});

            // Teardown while pressed must close the host gesture. The root
            // remains externally owned and retains the widget after the bridge.
            live_knob->on_mouse_down({40.0f, 40.0f});
            REQUIRE(store.open_gesture_count() == 1);
        }
        REQUIRE(store.open_gesture_count() == 0);
        REQUIRE(events.back() == "end:2");
    }

    // The retained widget callbacks are lifetime-gated after both bridge and
    // store destruction.
    REQUIRE(live_knob != nullptr);
    live_knob->on_mouse_up({40.0f, 40.0f});
}

TEST_CASE("web-compat parameter gesture teardown tolerates hostile callbacks",
          "[view][import][runtime]") {
    ScriptEngine engine;
    View root;
    pulp::state::StateStore store;
    store.add_parameter({
        .id = 1,
        .name = "gain",
        .range = {0.0f, 1.0f, 0.5f},
    });

    WidgetBridge* live_bridge = nullptr;
    bool reenter = true;
    store.set_gesture_callbacks(
        [](pulp::state::ParamID) {},
        [&](pulp::state::ParamID) {
            if (!reenter) return;
            reenter = false;
            live_bridge->clear();
        });
    WidgetBridge bridge(engine, root, store);
    live_bridge = &bridge;
    bridge.load_script(
        "createKnob('gain', '');"
        "bindWidgetToParam('gain', 'gain');");
    auto* knob = dynamic_cast<Knob*>(bridge.widget("gain"));
    REQUIRE(knob != nullptr);
    knob->on_mouse_down({40.0f, 40.0f});
    REQUIRE_NOTHROW(bridge.clear());
    REQUIRE(store.open_gesture_count() == 0);

    {
        ScriptEngine throwing_engine;
        View throwing_root;
        pulp::state::StateStore throwing_store;
        throwing_store.add_parameter({
            .id = 1,
            .name = "gain",
            .range = {0.0f, 1.0f, 0.5f},
        });
        throwing_store.set_gesture_callbacks(
            [](pulp::state::ParamID) {},
            [](pulp::state::ParamID) {
                throw std::runtime_error("intentional teardown failure");
            });
        WidgetBridge throwing_bridge(
            throwing_engine, throwing_root, throwing_store);
        throwing_bridge.load_script(
            "createKnob('gain', '');"
            "bindWidgetToParam('gain', 'gain');");
        auto* throwing_knob =
            dynamic_cast<Knob*>(throwing_bridge.widget("gain"));
        REQUIRE(throwing_knob != nullptr);
        throwing_knob->on_mouse_down({40.0f, 40.0f});
    }
}

TEST_CASE("web-compat parameter gestures share ownership across bridges",
          "[view][import][runtime]") {
    pulp::state::StateStore store;
    store.add_parameter({
        .id = 1,
        .name = "gain",
        .range = {0.0f, 1.0f, 0.5f},
    });
    std::vector<std::string> events;
    store.set_gesture_callbacks(
        [&](pulp::state::ParamID) { events.push_back("begin"); },
        [&](pulp::state::ParamID) { events.push_back("end"); });

    ScriptEngine first_engine;
    ScriptEngine second_engine;
    View first_root;
    View second_root;
    WidgetBridge first_bridge(first_engine, first_root, store);
    WidgetBridge second_bridge(second_engine, second_root, store);
    first_bridge.load_script(
        "createKnob('gain', '');"
        "bindWidgetToParam('gain', 'gain');");
    second_bridge.load_script(
        "createKnob('gain', '');"
        "bindWidgetToParam('gain', 'gain');");
    auto* first = dynamic_cast<Knob*>(first_bridge.widget("gain"));
    auto* second = dynamic_cast<Knob*>(second_bridge.widget("gain"));
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    first->on_mouse_down({40.0f, 40.0f});
    second->on_mouse_down({40.0f, 40.0f});
    second->on_mouse_up({40.0f, 40.0f});
    REQUIRE(events == std::vector<std::string>{"begin"});
    REQUIRE(store.open_gesture_count() == 1);
    first->on_mouse_up({40.0f, 40.0f});
    REQUIRE(events == std::vector<std::string>{"begin", "end"});
    REQUIRE(store.open_gesture_count() == 0);
}

TEST_CASE("web-compat audio widget escapes an authored label",
          "[view][import]") {
    // The label was interpolated raw into a single-quoted literal, so a design
    // could terminate the string and inject executable bridge JavaScript. Every
    // other emission in this file escapes; this one must too.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "name'); setTheme('light');//";
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = false;   // the live lowering Forge runs
    const auto js = generate_pulp_js(ir, opts);

    CHECK(js.find("setTheme('light');") == std::string::npos);
    CHECK(js.find("\\'") != std::string::npos);
}

TEST_CASE("web-compat audio widget label cannot escape its own comment",
          "[view][import]") {
    // The other half of the same hole. With comments on, the label was written
    // into a `//` line verbatim — and a comment is only a comment until the
    // text ends the line, so a newline in an authored label closed it and the
    // remainder became executable bridge JavaScript.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Controls";

    IRNode knob;
    knob.type = "knob";
    knob.name = "GainKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain\nsetTheme('light');\n";
    ir.root.children.push_back(knob);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = true;
    const auto js = generate_pulp_js(ir, opts);

    CHECK(js.find("\nsetTheme('light');\n") == std::string::npos);
}

TEST_CASE("generate_pulp_js respects CodeGenOptions", "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.source_file = "test.json";
    ir.root.type = "frame";
    ir.root.name = "Root";

    CodeGenOptions opts;
    opts.include_comments = true;
    auto with_comments = generate_pulp_js(ir, opts);
    REQUIRE(with_comments.find("// Generated by Pulp") != std::string::npos);

    opts.include_comments = false;
    auto no_comments = generate_pulp_js(ir, opts);
    REQUIRE(no_comments.find("// Generated") == std::string::npos);
}

TEST_CASE("generate_pulp_js bridge_native_js mode covers layout and audio widget edge branches",
          "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::pencil;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.layout.direction = LayoutDirection::row;
    ir.root.layout.justify = LayoutAlign::space_between;
    ir.root.layout.align = LayoutAlign::center;
    ir.root.layout.gap = 10.0f;
    ir.root.layout.padding_top = 2.0f;
    ir.root.layout.padding_right = 4.0f;
    ir.root.layout.padding_bottom = 6.0f;
    ir.root.layout.padding_left = 8.0f;
    ir.root.attributes["_layoutHeight"] = "180";
    ir.root.attributes["_layoutWidth"] = "420";
    ir.root.style.background_color = "#111111";
    ir.root.style.border_radius = 6.0f;

    IRNode left;
    left.type = "text";
    left.name = "left label";
    left.text_content = "Left";
    left.style.font_size = 12.0f;
    left.style.color = "#ffffff";
    ir.root.children.push_back(left);

    IRNode right;
    right.type = "text";
    right.name = "right.label";
    right.text_content = "Right";
    right.style.font_weight = 600;
    ir.root.children.push_back(right);

    IRNode knob;
    knob.type = "frame";
    knob.name = "ToneKnob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Tone";
    knob.audio_default = 0.33f;
    knob.style.width = 90.0f;
    knob.attributes["shape_width"] = "72";
    knob.attributes["shape_height"] = "72";
    IRNode ring;
    ring.type = "ellipse";
    ring.attributes["stroke_color"] = "#fab387";
    knob.children.push_back(ring);
    IRNode value;
    value.type = "text";
    value.text_content = "-6 dB";
    knob.children.push_back(value);
    ir.root.children.push_back(knob);

    IRNode xy;
    xy.type = "frame";
    xy.name = "FilterXYPad";
    xy.audio_widget = AudioWidgetType::xy_pad;
    xy.style.width = 72.0f;
    ir.root.children.push_back(xy);

    IRNode waveform;
    waveform.type = "frame";
    waveform.name = "MainWaveform";
    waveform.audio_widget = AudioWidgetType::waveform;
    waveform.style.width = 180.0f;
    waveform.style.height = 44.0f;
    ir.root.children.push_back(waveform);

    IRNode spectrum;
    spectrum.type = "frame";
    spectrum.name = "SpectrumAnalyzer";
    spectrum.audio_widget = AudioWidgetType::spectrum;
    spectrum.style.width = 160.0f;
    spectrum.style.height = 48.0f;
    ir.root.children.push_back(spectrum);

    IRNode spacer;
    spacer.type = "rectangle";
    spacer.name = "divider";
    spacer.style.height = 2.0f;
    spacer.style.background_color = "#333333";
    ir.root.children.push_back(spacer);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::bridge_native_js;
    opts.include_comments = false;
    opts.preview_mode = true;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("createRow('root', '')") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'height', 180)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'width', 420)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'padding_top', 2)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'padding_left', 8)") != std::string::npos);
    REQUIRE(js.find("setFlex('root', 'justify_content', 'space-between')") != std::string::npos);
    REQUIRE(js.find("setTextAlign('right_label1', 'right')") != std::string::npos);
    REQUIRE(js.find("setCornerRadius('root', 'All', 6)") != std::string::npos);
    REQUIRE(js.find("setWidgetStyle('ToneKnob2', 'minimal')") != std::string::npos);
    REQUIRE(js.find("setBorder('ToneKnob2', '#fab387', 2.5, 36)") != std::string::npos);
    REQUIRE(js.find("createLabel('ToneKnob2_val', '-6 dB'") != std::string::npos);
    REQUIRE(js.find("createXYPad('FilterXYPad3'") != std::string::npos);
    REQUIRE(js.find("createWaveform('MainWaveform4'") != std::string::npos);
    REQUIRE(js.find("createSpectrum('SpectrumAnalyzer5'") != std::string::npos);
    REQUIRE(js.find("createRow('divider6'") != std::string::npos);
    REQUIRE(js.find("setBackground('divider6', '#333333')") != std::string::npos);
}

TEST_CASE("generate_pulp_js web compat emits extended style and layout properties",
          "[view][import]") {
    DesignIR ir;
    ir.source = DesignSource::v0;
    ir.root.type = "frame";
    ir.root.name = "Root Panel";
    ir.root.layout.direction = LayoutDirection::row;
    ir.root.layout.gap = 2.5f;
    ir.root.layout.padding_top = 1.0f;
    ir.root.layout.padding_right = 2.0f;
    ir.root.layout.padding_bottom = 3.0f;
    ir.root.layout.padding_left = 4.0f;
    ir.root.layout.justify = LayoutAlign::center;
    ir.root.layout.align = LayoutAlign::center;
    ir.root.layout.wrap = true;
    ir.root.layout.width_mode = SizingMode::fill;
    ir.root.layout.height_mode = SizingMode::fill;
    ir.root.style.background_color = "#101010";
    ir.root.style.background_gradient = "linear-gradient(#101010,#202020)";
    ir.root.style.color = "#eeeeee";
    ir.root.style.opacity = 0.5f;
    ir.root.style.border_radius = 3.5f;
    ir.root.style.border = "1px solid #444";
    ir.root.style.box_shadow = parse_css_box_shadow("0 1px 2px #000");
    ir.root.style.filter = "blur(1px)";
    ir.root.style.backdrop_filter = "blur(7px)";
    ir.root.style.font_family = "Inter";
    ir.root.style.font_size = 15.0f;
    ir.root.style.font_weight = 500;
    ir.root.style.font_style = "italic";
    ir.root.style.text_align = "center";
    ir.root.style.letter_spacing = 0.5f;
    ir.root.style.line_height = 1.3f;
    ir.root.style.text_transform = "uppercase";
    ir.root.style.overflow = "hidden";
    ir.root.style.cursor = "grab";
    ir.root.style.position = "absolute";
    ir.root.style.top = 1.0f;
    ir.root.style.left = 2.0f;
    ir.root.style.right = 3.0f;
    ir.root.style.bottom = 4.0f;
    ir.root.style.z_index = 12;
    ir.root.style.transform = "scale(1.1)";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 100.0f;
    ir.root.style.min_width = 80.0f;
    ir.root.style.min_height = 30.0f;
    ir.root.style.max_width = 400.0f;
    ir.root.style.max_height = 160.0f;

    IRNode button;
    button.type = "button";
    button.name = "Send Button";
    button.text_content = "Send";
    ir.root.children.push_back(button);

    IRNode input;
    input.type = "input";
    input.name = "amount-input";
    ir.root.children.push_back(input);

    IRNode image;
    image.type = "image";
    image.name = "logo.png";
    ir.root.children.push_back(image);

    ir.tokens.strings["copy.cta"] = "Send";

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = false;
    opts.root_variable = "panelRoot";
    opts.indent_spaces = 4;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("const panelRoot = document.createElement('div')") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.flexDirection = 'row'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.gap = '2.5px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.paddingTop = '1px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.paddingLeft = '4px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.justifyContent = 'center'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.alignItems = 'center'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.flexWrap = 'wrap'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.flexGrow = '1'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.background = 'linear-gradient(#101010,#202020)'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.opacity = '0.5'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.borderRadius = '3.5px'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.boxShadow = '0 1px 2px #000'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.filter = 'blur(1px)'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.backdropFilter = 'blur(7px)'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.zIndex = '12'") != std::string::npos);
    REQUIRE(js.find("panelRoot.style.maxHeight = '160px'") != std::string::npos);
    REQUIRE(js.find("document.createElement('button')") != std::string::npos);
    REQUIRE(js.find("document.createElement('input')") != std::string::npos);
    REQUIRE(js.find("document.createElement('img')") != std::string::npos);
    REQUIRE(js.find("theme.strings[\"copy.cta\"] = 'Send'") != std::string::npos);
    REQUIRE(js.find("document.body.appendChild(panelRoot)") != std::string::npos);
}

// ─── Visual overrides reach EVERY node kind, not just the branch that had them ───
//
// These all guard one bug shape: a per-View property emitted from one lowering
// branch, so the design looks wrong only for node kinds that take a different
// branch. Nothing errors — the boxes are the right size in the right place, they
// just aren't faded / shadowed / filled. Each case below renders a node kind
// through generate_pulp_js and asserts the property survives the lowering.

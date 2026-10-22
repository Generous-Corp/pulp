// Captured browser line layout reaching a Label through the JS bridge.
//
// Generated JS imports install a capture's line boxes with
// setCapturedLineBoxes. When the capture also measured the primary font's
// ascent and descent, the call carries them, and the Label places its baseline
// from them exactly as the native materializer does.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <cmath>
#include <string>

using namespace pulp::view;
using namespace pulp::state;

namespace {

float painted_baseline(const std::string& metrics_args) {
    const auto face = pulp::canvas::resolved_face_identity("Inter", 400.0f);
    if (face.empty())
        return std::nanf("");
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createLabel('copy', 'Measured', '');\n"
                       "setFontFamily('copy', 'Inter');\n"
                       "setFontSize('copy', 17);\n");
    auto* label = dynamic_cast<Label*>(bridge.widget("copy"));
    REQUIRE(label != nullptr);
    label->set_bounds({0, 0, 200, 40});
    bridge.load_script("setCapturedLineBoxes('copy', [\n"
                       "  { left: 0, top: 2, width: 120, height: 20, start: 0, length: 8 }\n"
                       "], 200, '" +
                       face + "', false" + metrics_args + ");\n");
    REQUIRE(label->cached_line_boxes().size() == 1);
    pulp::canvas::RecordingCanvas canvas;
    label->paint(canvas);
    for (const auto& command : canvas.commands())
        if (command.type == pulp::canvas::DrawCommand::Type::fill_text)
            return command.f[1];
    FAIL("the label painted no text");
    return 0.0f;
}

} // namespace

TEST_CASE("setCapturedLineBoxes carries measured line metrics to the Label",
          "[view][bridge][typography][captured-lines]") {
    const float with_metrics = painted_baseline(", 13, 7");
    if (std::isnan(with_metrics)) {
        SKIP("Inter does not resolve to a real face on this host");
    }
    // A 20px box that is exactly 13 + 7: the baseline is 13 below its top.
    CHECK(with_metrics == Catch::Approx(15.0f).margin(0.001f));
    // Control: without the metrics the Label derives its own, which do not
    // put the baseline there, so the equality above measures the argument.
    const float derived = painted_baseline("");
    CHECK(std::abs(derived - 15.0f) > 0.25f);
    // Non-numeric metrics are ignored rather than misread.
    CHECK(painted_baseline(", 'x', 7") == Catch::Approx(derived).margin(0.001f));
}

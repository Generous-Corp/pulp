// Script text runs with NaN and -0 letterSpacing, painted through the Skia
// screenshot backend, which builds a SkiaCanvas per render the way a host
// builds one per frame. setTextRuns keeps the inherited spacing for a
// non-finite value: a NaN that reached SkParagraph would never find its own
// entry in Skia's paragraph cache, which TextShaper's layout spins on, and in
// this canvas's paragraph cache, whose eviction it would break. -0 reaches the
// canvas and must share +0's entry.

#ifdef PULP_HAS_SKIA

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/paragraph_cache.hpp>
#include <pulp/canvas/resource_counters.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include <string>

using namespace pulp::view;

namespace {

std::uint64_t paragraph_builds() {
    return pulp::canvas::resource_counters().paragraph_builds;
}

bool paint(View& root) {
    return !render_to_png(root, 240, 60, 1.0f, ScreenshotBackend::skia).empty();
}

} // namespace

TEST_CASE("Paragraph cache holds script text runs with NaN and signed-zero spacing",
          "[canvas][paragraph-cache][widget-bridge]") {
    auto& cache = pulp::canvas::ParagraphCache::instance();
    cache.set_enabled(true);
    cache.set_limits(pulp::canvas::ParagraphCache::kDefaultMaxEntries,
                     pulp::canvas::ParagraphCache::kDefaultMaxBytes);
    cache.clear();

    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 240, 60});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createLabel('t', 'Hello world', '');\n"
                       "setFontSize('t', 16);\n"
                       "setTextRuns('t', [{ start: 0, end: 5, letterSpacing: NaN },\n"
                       "                  { start: 6, end: 11, letterSpacing: -0 }]);\n"
                       "createLabel('m', '-60 dB', '');\n");
    auto* label = dynamic_cast<Label*>(bridge.widget("t"));
    auto* meter = dynamic_cast<Label*>(bridge.widget("m"));
    REQUIRE(label != nullptr);
    REQUIRE(meter != nullptr);
    REQUIRE(label->has_attributed_string());
    label->set_bounds({0, 0, 240, 30});
    meter->set_bounds({0, 30, 240, 30});

    REQUIRE(paint(root));
    const auto first = paragraph_builds();
    REQUIRE(paint(root));
    // A repaint of unchanged text shapes nothing: the NaN run found its entry.
    CHECK(paragraph_builds() - first == 0);

    // A cache of one entry, the NaN run repainted while the meter churns:
    // every paint evicts, so a key that could not find itself would be
    // erased through a missing map entry.
    cache.set_limits(1, pulp::canvas::ParagraphCache::kDefaultMaxBytes);
    for (int frame = 0; frame < 20; ++frame) {
        meter->set_text(std::to_string(-60 + frame) + " dB");
        REQUIRE(paint(root));
        CHECK(cache.stats().entries <= 1);
    }

    cache.set_limits(pulp::canvas::ParagraphCache::kDefaultMaxEntries,
                     pulp::canvas::ParagraphCache::kDefaultMaxBytes);
    cache.clear();
}

#endif // PULP_HAS_SKIA

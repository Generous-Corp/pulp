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

#include <cstdint>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

std::uint64_t paragraph_builds() {
    return pulp::canvas::resource_counters().paragraph_builds;
}

bool paint(View& root) {
    return !render_to_png(root, 240, 60, 1.0f, ScreenshotBackend::skia).empty();
}

// The paragraph cache is process-wide: put it back to its defaults on the way
// out, even when a REQUIRE fails, so a later test does not inherit one entry.
struct CacheDefaults {
    CacheDefaults() {
        reset();
    }
    ~CacheDefaults() {
        reset();
    }
    static void reset() {
        auto& cache = pulp::canvas::ParagraphCache::instance();
        cache.set_enabled(true);
        cache.set_limits(pulp::canvas::ParagraphCache::kDefaultMaxEntries,
                         pulp::canvas::ParagraphCache::kDefaultMaxBytes);
        cache.clear();
    }
};

} // namespace

TEST_CASE("Paragraph cache holds script text runs with NaN and signed-zero spacing",
          "[canvas][paragraph-cache][widget-bridge]") {
    CacheDefaults restore;
    auto& cache = pulp::canvas::ParagraphCache::instance();

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
}

namespace {

// One Label with `base` applied and `runs` set as its text runs, painted
// through the Skia backend. Runs that resolve to the same style paint the same
// bytes, so a run can be compared against the style it should inherit.
std::vector<std::uint8_t> paint_runs(const std::string& base, const std::string& runs) {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 240, 40});
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("createLabel('t', 'Hello world', '');\n" + base + "setTextRuns('t', [" +
                       runs + "]);\n");
    auto* label = dynamic_cast<Label*>(bridge.widget("t"));
    if (label == nullptr || !label->has_attributed_string())
        return {};
    label->set_bounds({0, 0, 240, 40});
    return render_to_png(root, 240, 40, 1.0f, ScreenshotBackend::skia);
}

} // namespace

TEST_CASE("setTextRuns keeps the inherited spacing for a non-finite letterSpacing",
          "[canvas][paragraph-cache][widget-bridge]") {
    CacheDefaults restore;
    const std::string base = "setFontSize('t', 16);\nsetLetterSpacing('t', 3);\n";
    const auto inherited = paint_runs(base, "{ start: 0, end: 5, letterSpacing: 3 }");
    REQUIRE_FALSE(inherited.empty());
    // Control: a different spacing paints differently, so equal bytes below
    // mean the same spacing, not an instrument that cannot see spacing.
    REQUIRE(paint_runs(base, "{ start: 0, end: 5, letterSpacing: 0 }") != inherited);
    // NaN keeps the Label's 3. Were it passed on, make_paragraph would fold it
    // to 0 and the run would paint like the control instead.
    CHECK(paint_runs(base, "{ start: 0, end: 5, letterSpacing: NaN }") == inherited);
}

TEST_CASE("setTextRuns keeps the inherited size for a non-finite or non-positive fontSize",
          "[canvas][paragraph-cache][widget-bridge]") {
    CacheDefaults restore;
    const std::string base = "setFontSize('t', 16);\n";
    // The canvas keeps its last usable size when handed one it cannot use, so
    // the run under test follows a run at 8. Painted first, a bad size would
    // ride on the Label's 16 and look inherited whether or not it was.
    const std::string sized = "{ start: 0, end: 6, fontSize: 8 }";
    const auto inherited = paint_runs(base, sized);
    REQUIRE_FALSE(inherited.empty());
    REQUIRE(paint_runs(base, sized + ", { start: 6, end: 11, fontSize: 12 }") != inherited);
    // parseFloat of a CSS keyword gives NaN; 0 and negatives are not sizes.
    // Each keeps the Label's size, as setFontSize does, rather than the 8 the
    // canvas was left at.
    for (const char* size : {"NaN", "0", "-3"}) {
        INFO("fontSize " << size);
        CHECK(paint_runs(base, sized + ", { start: 6, end: 11, fontSize: " + size + " }") ==
              inherited);
    }
}

#endif // PULP_HAS_SKIA

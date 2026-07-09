// The root View paints a small "◉ TRACING" corner pill whenever the binary is
// built with PULP_TRACING=ON, so a developer can never forget that Perfetto
// tracing is compiled in while looking at the plugin UI. In the default OFF
// build the paint site is discarded by `if constexpr (kTracingEnabled)`, so no
// badge is ever emitted. These tests pin both halves with a RecordingCanvas
// (deterministic, no raster backend needed) and cover the runtime suppression
// hook golden-screenshot harnesses use.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/trace.hpp>  // kTracingEnabled
#include <pulp/view/tracing_badge.hpp>
#include <pulp/view/view.hpp>

#include <memory>
#include <string>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

namespace {

// Number of fill_text commands whose text carries the badge label. The label is
// distinctive ("TRACING"), so a match is unambiguously the tracing badge and
// never incidental widget text.
int badge_label_draws(const RecordingCanvas& rec) {
    int n = 0;
    for (const auto& c : rec.commands())
        if (c.type == DrawCommand::Type::fill_text &&
            c.text.find("TRACING") != std::string::npos)
            ++n;
    return n;
}

// A rounded-rect fill sits behind the badge label as its pill background.
bool has_rounded_rect(const RecordingCanvas& rec) {
    for (const auto& c : rec.commands())
        if (c.type == DrawCommand::Type::fill_rounded_rect) return true;
    return false;
}

}  // namespace

TEST_CASE("tracing badge predicate tracks the compile-time flag", "[view][tracing]") {
    // Default state: the badge is visible whenever tracing is compiled in.
    set_tracing_badge_visible(true);
    REQUIRE(tracing_badge_should_paint() == pulp::runtime::kTracingEnabled);
}

TEST_CASE("tracing badge suppression hook", "[view][tracing]") {
    set_tracing_badge_visible(false);
    // Suppressed: never paints, regardless of build configuration.
    REQUIRE_FALSE(tracing_badge_should_paint());
    // Restore the default so it cannot leak into other tests.
    set_tracing_badge_visible(true);
    REQUIRE(tracing_badge_should_paint() == pulp::runtime::kTracingEnabled);
}

TEST_CASE("root View paints the tracing badge only when compiled in", "[view][tracing]") {
    set_tracing_badge_visible(true);

    View root;
    root.set_bounds({0, 0, 400, 300});

    RecordingCanvas rec;
    root.paint_all(rec);

    if constexpr (pulp::runtime::kTracingEnabled) {
        // ON build: exactly one badge (the root paints it) with its pill.
        REQUIRE(badge_label_draws(rec) == 1);
        REQUIRE(has_rounded_rect(rec));
    } else {
        // OFF build: the paint site is discarded — nothing is emitted.
        REQUIRE(badge_label_draws(rec) == 0);
    }
}

TEST_CASE("suppressed tracing badge is absent even in an ON build", "[view][tracing]") {
    set_tracing_badge_visible(false);

    View root;
    root.set_bounds({0, 0, 400, 300});

    RecordingCanvas rec;
    root.paint_all(rec);
    REQUIRE(badge_label_draws(rec) == 0);

    // Re-enable and confirm it comes back only when tracing is compiled in.
    set_tracing_badge_visible(true);
    RecordingCanvas rec2;
    root.paint_all(rec2);
    if constexpr (pulp::runtime::kTracingEnabled)
        REQUIRE(badge_label_draws(rec2) == 1);
    else
        REQUIRE(badge_label_draws(rec2) == 0);
}

TEST_CASE("child View never paints the tracing badge", "[view][tracing]") {
    set_tracing_badge_visible(true);

    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 100, 80});
    View* child_ptr = child.get();
    root.add_child(std::move(child));

    // Painting the child directly (it has a parent) must not stamp a badge.
    RecordingCanvas rec;
    child_ptr->paint_all(rec);
    REQUIRE(badge_label_draws(rec) == 0);
}

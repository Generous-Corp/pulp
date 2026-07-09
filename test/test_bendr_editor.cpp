// Bendr editor contracts that only a host exercises, pinned headlessly.
//
// Both cases regressed silently: the editor compiled, opened, and painted, while
// its type-in never saw a keystroke in a DAW and its latency readout showed a
// number unrelated to the processor.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "reference_processor.hpp"
#include "reference_ui.hpp"

#include <pulp/state/store.hpp>

#include <memory>

using Catch::Matchers::WithinAbs;

namespace {

// Bring up the processor the way a host does, then its editor.
struct Fixture {
    pulp::state::StateStore store;
    bendr::ReferenceProcessor proc;
    std::unique_ptr<pulp::view::View> view;
    bendr::ReferenceUi* ui = nullptr;

    Fixture() {
        proc.set_state_store(&store);
        proc.define_parameters(store);
        pulp::format::PrepareContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.max_buffer_size = 512;
        proc.prepare(ctx);

        view = proc.create_view();
        ui = static_cast<bendr::ReferenceUi*>(view.get());
        ui->set_bounds({0.0f, 0.0f, 760.0f, 560.0f});
        ui->layout_for_test();
    }
};

}  // namespace

TEST_CASE("bendr editor takes the keyboard only while a type-in is open", "[bendr][view]") {
    Fixture f;

    // The macOS plugin host gates acceptsFirstResponder on this. Reporting true
    // when no field is open would hold the DAW's keyboard and silence Logic's
    // Musical Typing; reporting false while typing means no keystroke arrives.
    REQUIRE(f.ui->edit_ctl_for_test() < 0);
    CHECK_FALSE(f.ui->accepts_text_input());

    // Click a numeric field without dragging: that opens its type-in.
    const auto field = f.ui->ctl_field_center_for_test(0);
    f.ui->on_mouse_down(field);
    f.ui->on_mouse_up(field);

    REQUIRE(f.ui->edit_ctl_for_test() == 0);
    CHECK(f.ui->accepts_text_input());

    // Escape closes it, handing the keyboard back.
    pulp::view::KeyEvent esc;
    esc.key = pulp::view::KeyCode::escape;
    esc.is_down = true;
    REQUIRE(f.ui->on_key_event(esc));

    REQUIRE(f.ui->edit_ctl_for_test() < 0);
    CHECK_FALSE(f.ui->accepts_text_input());
}

TEST_CASE("bendr publishes the processor's real latency", "[bendr][view]") {
    Fixture f;

    const float ms = f.proc.latency_ms().load(std::memory_order_relaxed);
    const int samples = f.proc.latency_samples();

    REQUIRE(samples > 0);
    CHECK_THAT(ms, WithinAbs(static_cast<float>(samples) / 48000.0f * 1000.0f, 1e-3));

    // Re-preparing at another rate must move the published value, which a
    // hardcoded constant would not.
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = 96000.0;
    ctx.max_buffer_size = 512;
    f.proc.prepare(ctx);

    const float ms96 = f.proc.latency_ms().load(std::memory_order_relaxed);
    CHECK_THAT(ms96, WithinAbs(static_cast<float>(f.proc.latency_samples()) / 96000.0f * 1000.0f,
                               1e-3));
}

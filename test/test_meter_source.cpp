// Meter/scalar host→view source — the paint-safe lock-free channel a Meter
// self-updates from on the FrameClock. Covers the source primitives and the
// subscription lifecycle across the construction orders real hosts use
// (build-then-clock, clock-then-bind, graft-under-clock, detach).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <memory>
#include <thread>

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/value_source.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;

namespace {
MeterFrame stereo(float rms, float peak) {
    MeterFrame f;
    f.channels = 2;
    f.rms[0] = f.rms[1] = rms;
    f.peak[0] = f.peak[1] = peak;
    return f;
}
}  // namespace

TEST_CASE("MeterSource publishes the latest frame", "[view][meter-source]") {
    MeterSource src;
    src.publish(stereo(0.3f, 0.6f));
    const MeterFrame got = src.read();
    CHECK(got.channels == 2);
    CHECK(got.rms[0] == Catch::Approx(0.3f));
    CHECK(got.peak[1] == Catch::Approx(0.6f));

    src.publish(stereo(0.1f, 0.9f));  // latest wins
    CHECK(src.read().peak[0] == Catch::Approx(0.9f));
}

TEST_CASE("ScalarSource publishes the latest value", "[view][meter-source]") {
    ScalarSource s;
    CHECK(s.read() == Catch::Approx(0.0f));  // initial
    s.publish(0.42f);
    CHECK(s.read() == Catch::Approx(0.42f));
    s.publish(-1.0f);
    CHECK(s.read() == Catch::Approx(-1.0f));
}

TEST_CASE("a source-bound Meter subscribes when the clock is installed AFTER build",
          "[view][meter-source][lifecycle]") {
    // The order real Pulp hosts use: build the tree + bind the source, THEN
    // install the frame clock on the root. The propagation hook must reach the
    // meter or it silently never updates.
    auto root = std::make_unique<View>();
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));  // on_attached fires now — no clock yet

    auto src = std::make_shared<MeterSource>();
    m->set_source(src, 0);

    FrameClock clock;
    REQUIRE_FALSE(clock.has_active_subscribers());
    root->set_frame_clock(&clock);  // propagates → the meter subscribes
    REQUIRE(clock.has_active_subscribers());

    src->publish(stereo(0.8f, 0.9f));
    clock.tick(0.016f);
    CHECK(m->display_peak() > 0.0f);  // ballistic path moved toward the level
}

TEST_CASE("a source bound AFTER the clock exists still subscribes",
          "[view][meter-source][lifecycle]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    REQUIRE_FALSE(clock.has_active_subscribers());  // no source yet
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock.has_active_subscribers());
}

TEST_CASE("grafting a pre-built source-bound subtree under a clocked root subscribes",
          "[view][meter-source][lifecycle]") {
    // Build container + meter + source entirely offline, then graft into an
    // already-clocked root. add_child must notify the grafted subtree.
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);

    auto container = std::make_unique<View>();
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    container->add_child(std::move(meter));
    m->set_source(std::make_shared<MeterSource>(), 0);  // no clock reachable yet
    REQUIRE_FALSE(clock.has_active_subscribers());

    root->add_child(std::move(container));  // graft under the clock
    REQUIRE(clock.has_active_subscribers());
}

TEST_CASE("unbinding a Meter's source unsubscribes", "[view][meter-source][lifecycle]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock.has_active_subscribers());
    m->set_source(nullptr);
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("detaching a descendant Meter drops its subscription",
          "[view][meter-source][lifecycle]") {
    // The meter is a DESCENDANT of the removed node, so remove_child never fires
    // on_detached on it — the clock-change notification must still unsubscribe it.
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto container = std::make_unique<View>();
    View* c = container.get();
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    container->add_child(std::move(meter));
    root->add_child(std::move(container));
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock.has_active_subscribers());

    auto removed = root->remove_child(c);  // detaches container + descendant meter
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("a Meter ignores an out-of-range source channel", "[view][meter-source]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    auto src = std::make_shared<MeterSource>();
    m->set_source(src, 5);  // channel 5, frames only carry 2
    src->publish(stereo(0.8f, 0.9f));
    clock.tick(0.016f);
    CHECK(m->display_peak() == Catch::Approx(0.0f));  // untouched
}

TEST_CASE("a destroyed source-bound Meter leaves no dangling FrameClock subscriber",
          "[view][meter-source][lifecycle]") {
    FrameClock clock;
    auto root = std::make_unique<View>();
    root->set_frame_clock(&clock);
    {
        auto meter = std::make_unique<Meter>();
        meter->set_source(std::make_shared<MeterSource>(), 0);
        Meter* m = meter.get();
        root->add_child(std::move(meter));
        REQUIRE(clock.has_active_subscribers());
        root->remove_child(m);  // returns the owner; dropped at end of scope → ~Meter
    }
    // ~Meter must have unsubscribed; ticking must not touch freed memory.
    clock.tick(0.016f);
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("a Meter never reads out of bounds on a malformed channel count",
          "[view][meter-source]") {
    auto root = std::make_unique<View>();
    FrameClock clock;
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    auto src = std::make_shared<MeterSource>();
    m->set_source(src, MeterFrame::kMaxChannels);  // bind a channel at capacity

    MeterFrame bad;             // malformed: claims far more channels than exist
    bad.channels = 999;
    bad.peak[0] = 0.9f;
    bad.rms[0] = 0.8f;
    src->publish(bad);
    clock.tick(0.016f);         // must bound the index by kMaxChannels — no OOB read
    CHECK(m->display_peak() == Catch::Approx(0.0f));  // at-capacity index → ignored
}

TEST_CASE("a live Meter re-points when moved between two FrameClocks",
          "[view][meter-source][lifecycle]") {
    FrameClock clock_a, clock_b;
    auto root_a = std::make_unique<View>();
    root_a->set_frame_clock(&clock_a);
    auto root_b = std::make_unique<View>();
    root_b->set_frame_clock(&clock_b);

    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root_a->add_child(std::move(meter));
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock_a.has_active_subscribers());
    REQUIRE_FALSE(clock_b.has_active_subscribers());

    auto moved = root_a->remove_child(m);   // detaches from A → unsubscribes from A
    REQUIRE_FALSE(clock_a.has_active_subscribers());
    root_b->add_child(std::move(moved));    // graft under B → subscribes to B
    REQUIRE(clock_b.has_active_subscribers());
}

TEST_CASE("clearing the root clock unsubscribes a live Meter before teardown",
          "[view][meter-source][lifecycle]") {
    FrameClock clock;
    auto root = std::make_unique<View>();
    root->set_frame_clock(&clock);
    auto meter = std::make_unique<Meter>();
    Meter* m = meter.get();
    root->add_child(std::move(meter));
    m->set_source(std::make_shared<MeterSource>(), 0);
    REQUIRE(clock.has_active_subscribers());

    // The host clears the clock before destroying it (Pulp's GPU hosts do this).
    // The meter must drop its subscription while the clock is still alive.
    root->set_frame_clock(nullptr);
    REQUIRE_FALSE(clock.has_active_subscribers());
}

TEST_CASE("TripleBuffer<MeterFrame> never tears a frame under a 2-thread hammer",
          "[view][meter-source][rt]") {
    MeterSource src;
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};

    std::thread writer([&] {
        for (int k = 1; k < 200000 && !stop.load(); ++k) {
            const float v = static_cast<float>(k % 1000) / 1000.0f;
            src.publish(stereo(v, v));  // every field equals v — a coherent frame
        }
        stop.store(true);
    });

    while (!stop.load()) {
        const MeterFrame f = src.read();
        // A coherent frame has all fields equal; a torn read would mix two writes.
        if (f.channels == 2 &&
            (f.rms[0] != f.rms[1] || f.rms[0] != f.peak[0] || f.peak[0] != f.peak[1])) {
            torn.fetch_add(1);
        }
    }
    writer.join();
    CHECK(torn.load() == 0);
}

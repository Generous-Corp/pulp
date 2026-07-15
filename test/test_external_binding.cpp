#include <catch2/catch_test_macros.hpp>

#include <pulp/state/external_binding.hpp>

#include <stdexcept>

using namespace pulp::state;

namespace {

// A stand-in for a value owned by something OTHER than a Pulp StateStore — a
// host's parameter object, another framework's parameter tree, a plain member
// of the plugin. ExternalBinding drives it through get/set lambdas, so the
// widget never learns where the value actually lives.
struct HostValue {
    float v = 0.0f;
    int begins = 0;
    int ends = 0;
};

ExternalBindingConfig config_for(HostValue& h, ParamRange range = {}) {
    ExternalBindingConfig cfg;
    cfg.get = [&h] { return h.v; };
    cfg.set = [&h](float x) { h.v = x; };
    cfg.range = range;
    cfg.begin_gesture = [&h] { ++h.begins; };
    cfg.end_gesture = [&h] { ++h.ends; };
    return cfg;
}

} // namespace

TEST_CASE("ExternalBinding round-trips get/set through the lambda", "[state][external]") {
    HostValue h;
    ExternalBinding b(config_for(h));
    REQUIRE(b.is_bound());
    REQUIRE(b.get() == 0.0f);

    b.set(0.42f);
    REQUIRE(h.v == 0.42f);   // wrote to the external source
    REQUIRE(b.get() == 0.42f);
}

TEST_CASE("ExternalBinding maps normalized through an explicit range", "[state][external]") {
    HostValue h;
    // 0..100 range: normalized 0.5 -> plain 50.
    ExternalBinding b(config_for(h, ParamRange{.min = 0.0f, .max = 100.0f, .default_value = 25.0f}));

    b.set_normalized(0.5f);
    REQUIRE(h.v == 50.0f);
    REQUIRE(b.get_normalized() == 0.5f);

    b.set(75.0f);
    REQUIRE(b.get_normalized() == 0.75f);
}

TEST_CASE("ExternalBinding fires on_change only on an actual change", "[state][external]") {
    HostValue h;
    ExternalBinding b(config_for(h));
    int fires = 0;
    float last = -1.0f;
    b.on_change([&](float v) { ++fires; last = v; });

    b.set(0.3f);
    REQUIRE(fires == 1);
    REQUIRE(last == 0.3f);

    b.set(0.3f);          // idempotent write -> no notification
    REQUIRE(fires == 1);

    b.set(0.6f);
    REQUIRE(fires == 2);
}

TEST_CASE("ExternalBinding gestures forward to the host callbacks", "[state][external]") {
    HostValue h;
    ExternalBinding b(config_for(h));

    b.begin_gesture();
    b.set_normalized(0.8f);
    b.end_gesture();

    REQUIRE(h.begins == 1);
    REQUIRE(h.ends == 1);
}

TEST_CASE("ExternalBinding poll() detects an out-of-band external change", "[state][external]") {
    HostValue h;
    ExternalBinding b(config_for(h));
    int fires = 0;
    b.on_change([&](float) { ++fires; });

    // Host mutates the value behind our back (e.g. automation).
    h.v = 0.9f;
    REQUIRE(b.poll());          // detected + fired
    REQUIRE(fires == 1);
    REQUIRE_FALSE(b.poll());    // unchanged since -> no fire
    REQUIRE(fires == 1);
}

TEST_CASE("ExternalBinding notify() fires immediately without waiting for poll", "[state][external]") {
    HostValue h;
    ExternalBinding b(config_for(h));
    int fires = 0;
    float seen = -1.0f;
    b.on_change([&](float v) { ++fires; seen = v; });

    h.v = 0.55f;                // host changed it and tells us right away
    b.notify();
    REQUIRE(fires == 1);
    REQUIRE(seen == 0.55f);
    REQUIRE_FALSE(b.poll());    // notify() already advanced last_polled_
}

TEST_CASE("ExternalBinding reentrant set() converges listeners on the final value",
          "[state][external]") {
    HostValue h;
    ExternalBinding b(config_for(h));
    int fires = 0;
    float last = -1.0f;
    // A pathological callback that writes again from inside the notification.
    b.on_change([&](float v) {
        ++fires;
        last = v;
        if (v < 1.0f) b.set(1.0f);   // reentrant write; the guard blocks recursion
    });

    b.set(0.5f);
    // The nested set() writes through (host ends at 1.0) and the reentrancy
    // guard suppresses its nested notification; bounded reconciliation then
    // re-dispatches so listeners converge on the final value instead of being
    // stranded on the stale 0.5.
    REQUIRE(h.v == 1.0f);
    REQUIRE(last == 1.0f);
    REQUIRE(fires == 2);   // 0.5, then the reconciled 1.0 — never unbounded
}

TEST_CASE("ExternalBinding a throwing callback does not wedge future notifications",
          "[state][external]") {
    HostValue h;
    ExternalBinding b(config_for(h));
    b.on_change([](float) { throw std::runtime_error("boom"); });

    // The throwing callback propagates out of set()...
    REQUIRE_THROWS(b.set(0.3f));
    REQUIRE(h.v == 0.3f);   // the write itself still went through
    // ...and the reentrancy guard must have been reset. If it were left stuck
    // true, this next changing set() would early-return from fire() and NOT
    // throw. It must still throw — proving notifications are not wedged.
    REQUIRE_THROWS(b.set(0.6f));
    REQUIRE(h.v == 0.6f);
}

TEST_CASE("ExternalBinding reset() returns to the range default", "[state][external]") {
    HostValue h;
    ExternalBinding b(config_for(h, ParamRange{.min = 0.0f, .max = 10.0f, .default_value = 4.0f}));
    b.set(9.0f);
    b.reset();
    REQUIRE(h.v == 4.0f);
}

TEST_CASE("ExternalBinding without a getter is unbound and reads zero", "[state][external]") {
    ExternalBinding b;   // default: no get/set
    REQUIRE_FALSE(b.is_bound());
    REQUIRE(b.get() == 0.0f);
    b.set(5.0f);         // no setter -> no-op, no crash
    REQUIRE(b.get() == 0.0f);
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_telemetry_tap.hpp>
#include <pulp/view/value_channel_set.hpp>

#include <array>
#include <chrono>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {
ControlTelemetryAuthority authority(std::string client = "client-a") {
    return {.client_id = std::move(client),
            .registration_id = "registration-1",
            .instance_id = "slot-1",
            .grant_id = "grant-1"};
}
} // namespace

TEST_CASE("canonical telemetry tap is disabled by default and preserves sole-reader ownership",
          "[inspect][control][telemetry][security]") {
    pulp::view::ValueChannelSet channels;
    channels.declare_scalar("gain");
    ControlTelemetryTap disabled({});
    CHECK_FALSE(disabled.attach(channels.attach_telemetry()));
    // The failed explicit activation released the exclusive attachment.
    CHECK(channels.attach_telemetry().valid());
}

TEST_CASE("bounded telemetry fanout downsamples redacts and accounts slow subscribers",
          "[inspect][control][telemetry][loss][redaction][soak]") {
    pulp::view::ValueChannelSet channels;
    auto* gain = channels.declare_scalar("gain");
    auto* secret = channels.declare_scalar("auth-token");
    REQUIRE(gain);
    REQUIRE(secret);
    auto now = std::chrono::steady_clock::now();
    ControlTelemetryTap tap({.enabled = true,
                             .maximum_rate_hz = 10.0,
                             .maximum_subscriptions = 2,
                             .maximum_subscriptions_per_client = 1,
                             .maximum_queued_frames = 2},
                            [&] { return now; });
    REQUIRE(tap.attach(channels.attach_telemetry(), [](std::string_view name) {
        return name == "auth-token" ? ControlTelemetrySensitivity::Sensitive
                                    : ControlTelemetrySensitivity::Observable;
    }));
    const auto subscriber = authority();
    const auto subscription =
        tap.subscribe(subscriber, {.channels = {"gain", "auth-token"}, .rate_hz = 100.0});
    REQUIRE(subscription);
    CHECK_FALSE(tap.subscribe(subscriber, {.channels = {"gain"}, .rate_hz = 1.0}));

    for (int value = 1; value <= 5; ++value) {
        gain->publish(static_cast<float>(value));
        secret->publish(999.0f);
        (void)gain->read();
        (void)secret->read();
        tap.poll();
        now += 101ms;
    }
    CHECK_FALSE(tap.try_pop(*subscription, authority("wrong-client")));
    const auto first = tap.try_pop(*subscription, subscriber);
    REQUIRE(first);
    CHECK(first->dropped_since_previous == 3);
    REQUIRE(first->channels.size() == 2);
    CHECK(first->channels[0].values.front() == Catch::Approx(4.0f));
    CHECK(first->channels[1].redacted);
    CHECK(first->channels[1].channel != "auth-token");
    CHECK(first->channels[1].values.empty());
    const auto second = tap.try_pop(*subscription, subscriber);
    REQUIRE(second);
    CHECK(second->dropped_since_previous == 0);
}

TEST_CASE("telemetry without a classifier fails closed",
          "[inspect][control][telemetry][security][redaction]") {
    pulp::view::ValueChannelSet channels;
    auto* value = channels.declare_scalar("unclassified");
    REQUIRE(value);
    ControlTelemetryTap tap({.enabled = true});
    REQUIRE(tap.attach(channels.attach_telemetry()));
    const auto subscriber = authority();
    CHECK_FALSE(tap.subscribe(subscriber, {.channels = {"unclassified"}, .rate_hz = 1.0e-300}));
    const auto subscription = tap.subscribe(subscriber, {.channels = {"unclassified"}});
    REQUIRE(subscription);
    value->publish(42.0f);
    (void)value->read();
    tap.poll();
    const auto frame = tap.try_pop(*subscription, subscriber);
    REQUIRE(frame);
    REQUIRE(frame->channels.size() == 1);
    CHECK(frame->channels.front().redacted);
    CHECK(frame->channels.front().values.empty());
}

TEST_CASE("sensitive telemetry names and values require explicit authority",
          "[inspect][control][telemetry][security][redaction]") {
    pulp::view::ValueChannelSet channels;
    auto* secret = channels.declare_scalar("secret-level");
    REQUIRE(secret);
    ControlTelemetryTap tap({.enabled = true});
    REQUIRE(tap.attach(channels.attach_telemetry(),
                       [](std::string_view) { return ControlTelemetrySensitivity::Sensitive; }));
    auto subscriber = authority();
    subscriber.allow_sensitive = true;
    const auto subscription = tap.subscribe(subscriber, {.channels = {"secret-level"}});
    REQUIRE(subscription);
    secret->publish(7.0f);
    (void)secret->read();
    tap.poll();
    const auto frame = tap.try_pop(*subscription, subscriber);
    REQUIRE(frame);
    REQUIRE(frame->channels.size() == 1);
    CHECK(frame->channels.front().channel == "secret-level");
    CHECK_FALSE(frame->channels.front().redacted);
    REQUIRE(frame->channels.front().values.size() == 1);
    CHECK(frame->channels.front().values.front() == Catch::Approx(7.0f));
}

TEST_CASE("slow telemetry subscribers retain event batches until their next sample",
          "[inspect][control][telemetry][events][downsampling]") {
    pulp::view::ValueChannelSet channels;
    auto* events = channels.declare_events("hits");
    REQUIRE(events);
    auto now = std::chrono::steady_clock::now();
    ControlTelemetryTap tap({.enabled = true, .maximum_rate_hz = 10.0}, [&] { return now; });
    REQUIRE(tap.attach(channels.attach_telemetry(),
                       [](std::string_view) { return ControlTelemetrySensitivity::Observable; }));
    const auto subscriber = authority();
    const auto subscription = tap.subscribe(
        subscriber, {.channels = {"hits"}, .rate_hz = 1.0, .maximum_vector_values = 8});
    REQUIRE(subscription);
    tap.poll();
    REQUIRE(tap.try_pop(*subscription, subscriber));

    const std::array<pulp::view::ValueEvent, 3> event{{
        {.frame_index = 1, .value = 1.0f},
        {.frame_index = 2, .value = 1.0f},
        {.frame_index = 3, .value = 1.0f},
    }};
    events->publish(event.data(), static_cast<int>(event.size()));
    now += 100ms;
    tap.poll();
    CHECK_FALSE(tap.try_pop(*subscription, subscriber));
    now += 900ms;
    tap.poll();
    const auto frame = tap.try_pop(*subscription, subscriber);
    REQUIRE(frame);
    REQUIRE(frame->channels.size() == 1);
    REQUIRE(frame->channels.front().values.size() == 1);
    CHECK(frame->channels.front().values.front() == Catch::Approx(3.0f));
}

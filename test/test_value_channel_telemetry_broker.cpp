#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/value_channel_telemetry_broker.hpp>
#include <pulp/view/value_channel_set.hpp>

#include <choc/text/choc_JSON.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using pulp::inspect::InspectorMessage;
using pulp::inspect::InspectorRequestContext;
using pulp::inspect::InspectorTargetedEventResult;
using pulp::inspect::ValueChannelTelemetryBroker;
using pulp::view::ValueChannelSet;

InspectorMessage request(std::int64_t id, std::string method, std::string params = "{}") {
    return pulp::inspect::make_request(id, std::move(method), std::move(params));
}

choc::value::Value json(const InspectorMessage& message) {
    return choc::json::parse(message.params_json);
}

choc::value::ValueView channel(const choc::value::ValueView& root, std::string_view name) {
    const auto channels = root["channels"];
    for (std::uint32_t i = 0; i < channels.size(); ++i) {
        if (channels[i]["name"].getString() == name)
            return channels[i];
    }
    return {};
}

/// A ValueView does not own its bytes, so a view taken from a temporary Value
/// dangles the moment the full expression ends. `json()` returns by value, so
/// `channel(json(msg), name)` reads freed memory as soon as the result outlives
/// the statement. Deleting the rvalue overload turns that into a compile error
/// rather than a heap-use-after-free only a sanitizer lane can see: name the
/// Value first, then take the view.
choc::value::ValueView channel(choc::value::Value&&, std::string_view) = delete;

} // namespace

TEST_CASE("telemetry snapshots expose stable schema and UI-consumption staleness",
          "[inspect][telemetry][broker]") {
    auto channels = std::make_unique<ValueChannelSet>();
    auto* scalar = channels->declare_scalar("gain", "dB");
    auto* meter = channels->declare_meter("meter");
    REQUIRE(scalar != nullptr);
    REQUIRE(meter != nullptr);

    auto now = std::chrono::steady_clock::now();
    ValueChannelTelemetryBroker broker({}, [&] { return now; });
    REQUIRE(broker.replace_attachment(channels->attach_telemetry()));

    scalar->publish(-3.0f);
    auto response = broker.handle(InspectorRequestContext{.client_id = "actual-client"},
                                  request(1, pulp::inspect::methods::kTelemetryGetSnapshot,
                                          R"({"channels":["gain","meter"],"clientId":"spoofed"})"));
    REQUIRE_FALSE(response.is_error);
    INFO(response.params_json);
    auto value = json(response);
    CHECK(value["schema"].getString() == "pulp.inspect.telemetry.snapshot.v1");
    CHECK(value["schemaVersion"].getWithDefault<std::int64_t>(0) == 1);
    auto gain = channel(value, "gain");
    REQUIRE(gain.isObject());
    CHECK_FALSE(gain["available"].getWithDefault(true));
    CHECK(gain["staleReason"].getString() == "not_yet_consumed");
    CHECK(gain["sourceTimestampNs"].isVoid());
    CHECK(gain["uiTimestampNs"].isVoid());
    // A dead/unobserved meter must remain null rather than reading the default
    // scalar variant as a MeterFrame.
    channels.reset();
    response = broker.handle(
        InspectorRequestContext{.client_id = "actual-client"},
        request(2, pulp::inspect::methods::kTelemetryGetSnapshot, R"({"channels":["meter"]})"));
    REQUIRE_FALSE(response.is_error);
    const auto dead_value = json(response);
    auto dead = channel(dead_value, "meter");
    CHECK(dead["terminal"].getWithDefault(false));
    CHECK(dead["payload"].isVoid());
}

TEST_CASE("telemetry serializes every shape and marks consumed snapshots stale",
          "[inspect][telemetry][broker][schema]") {
    ValueChannelSet channels;
    auto* scalar = channels.declare_scalar("gain");
    auto* meter = channels.declare_meter("levels");
    auto* vector = channels.declare_vector("scope");
    auto* events = channels.declare_events("zero-crossings");
    REQUIRE(scalar != nullptr);
    REQUIRE(meter != nullptr);
    REQUIRE(vector != nullptr);
    REQUIRE(events != nullptr);
    auto now = std::chrono::steady_clock::now();
    ValueChannelTelemetryBroker broker({}, [&] { return now; });
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));

    scalar->publish(-6.0f);
    scalar->read();
    pulp::view::MeterFrame meter_frame;
    meter_frame.channels = 1;
    meter_frame.rms[0] = 0.25f;
    meter_frame.peak[0] = 0.5f;
    meter->publish(meter_frame);
    meter->read();
    const std::array<float, 3> scope{0.0f, 0.5f, -0.5f};
    vector->publish(scope.data(), static_cast<int>(scope.size()));
    vector->read();
    const pulp::view::ValueEvent zero{.frame_index = 0, .value = 0.0f};
    events->publish(&zero, 1);

    auto response = broker.handle(InspectorRequestContext{.client_id = "reader"},
                                  request(1, pulp::inspect::methods::kTelemetryGetSnapshot));
    REQUIRE_FALSE(response.is_error);
    auto value = json(response);
    CHECK(channel(value, "gain")["payload"].getWithDefault(0.0) == Catch::Approx(-6.0));
    CHECK(channel(value, "levels")["payload"]["peak"][0].getWithDefault(0.0) == Catch::Approx(0.5));
    CHECK(channel(value, "scope")["payload"]["values"].size() == 3);
    const auto event_payload = channel(value, "zero-crossings")["payload"];
    REQUIRE(event_payload.size() == 1);
    CHECK(event_payload[0]["value"].getWithDefault(1.0) == Catch::Approx(0.0));

    response = broker.handle(InspectorRequestContext{.client_id = "reader"},
                             request(2, pulp::inspect::methods::kTelemetryGetSnapshot));
    REQUIRE_FALSE(response.is_error);
    value = json(response);
    CHECK(channel(value, "zero-crossings")["payload"].size() == 1);

    now += 501ms;
    response = broker.handle(
        InspectorRequestContext{.client_id = "reader"},
        request(3, pulp::inspect::methods::kTelemetryGetSnapshot, R"({"channels":["gain"]})"));
    REQUIRE_FALSE(response.is_error);
    value = json(response);
    CHECK(channel(value, "gain")["stale"].getWithDefault(false));
    CHECK(channel(value, "gain")["staleReason"].getString() == "ui_not_consuming");
}

TEST_CASE("event snapshots retain the newest bounded suffix and expose loss",
          "[inspect][telemetry][broker][schema]") {
    ValueChannelSet channels;
    auto* events = channels.declare_events("onsets");
    REQUIRE(events != nullptr);
    ValueChannelTelemetryBroker::Config config;
    config.max_events_per_channel = 2;
    ValueChannelTelemetryBroker broker(config);
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));
    const std::array<pulp::view::ValueEvent, 3> first{{
        {.frame_index = 1, .value = 1.0f},
        {.frame_index = 2, .value = 2.0f},
        {.frame_index = 3, .value = 3.0f},
    }};
    events->publish(first.data(), static_cast<int>(first.size()));
    auto response = broker.handle(InspectorRequestContext{.client_id = "reader"},
                                  request(1, pulp::inspect::methods::kTelemetryGetSnapshot));
    REQUIRE_FALSE(response.is_error);
    auto snapshot = json(response);
    auto onsets = channel(snapshot, "onsets");
    REQUIRE(onsets["payload"].size() == 2);
    CHECK(onsets["payload"][0]["frameIndex"].getWithDefault<std::int64_t>(0) == 2);
    CHECK(onsets["payload"][1]["frameIndex"].getWithDefault<std::int64_t>(0) == 3);
    CHECK(onsets["coalesced"].getWithDefault<std::int64_t>(0) == 1);
    CHECK(onsets["sourcePublication"].getWithDefault<std::int64_t>(0) > 0);

    const pulp::view::ValueEvent overflow{.frame_index = 7, .value = 1.0f};
    for (int i = 0; i < 20; ++i)
        events->publish(&overflow, 1);
    response = broker.handle(InspectorRequestContext{.client_id = "reader"},
                             request(2, pulp::inspect::methods::kTelemetryGetSnapshot));
    REQUIRE_FALSE(response.is_error);
    snapshot = json(response);
    onsets = channel(snapshot, "onsets");
    CHECK(onsets["sourceDropped"].getWithDefault<std::int64_t>(0) > 0);

    ValueChannelSet wire_channels;
    auto* wire_events = wire_channels.declare_events("wire-onsets");
    REQUIRE(wire_events != nullptr);
    ValueChannelTelemetryBroker::Config wire_config;
    wire_config.max_events_per_channel = 8;
    wire_config.max_wire_bytes = 1024;
    ValueChannelTelemetryBroker wire_broker(wire_config);
    REQUIRE(wire_broker.replace_attachment(wire_channels.attach_telemetry()));
    std::array<pulp::view::ValueEvent, 12> wire_batch{};
    for (std::size_t i = 0; i < wire_batch.size(); ++i) {
        wire_batch[i] = {
            .frame_index = static_cast<std::uint32_t>(i + 1),
            .value = static_cast<float>(i + 1),
        };
    }
    wire_events->publish(wire_batch.data(), static_cast<int>(wire_batch.size()));
    const auto wire_response =
        wire_broker.handle(InspectorRequestContext{.client_id = "reader"},
                           request(3, pulp::inspect::methods::kTelemetryGetSnapshot));
    REQUIRE_FALSE(wire_response.is_error);
    INFO(wire_response.params_json);
    CHECK(pulp::inspect::encode_message(wire_response).size() <= wire_config.max_wire_bytes);
    const auto wire_snapshot = json(wire_response);
    const auto wire_onsets = channel(wire_snapshot, "wire-onsets");
    REQUIRE(wire_onsets["payload"].size() == 1);
    CHECK(wire_onsets["payload"][0]["frameIndex"].getWithDefault<std::int64_t>(0) == 12);
    // Four occurrences fell out of the eight-entry cache, then the wire cap
    // retained only the newest one of those eight.
    CHECK(wire_onsets["coalesced"].getWithDefault<std::int64_t>(0) == 11);
}

TEST_CASE("telemetry preserves non-finite values as parseable wire sentinels",
          "[inspect][telemetry][broker][schema]") {
    ValueChannelSet channels;
    auto* scalar = channels.declare_scalar("scalar");
    auto* meter = channels.declare_meter("meter");
    auto* vector = channels.declare_vector("vector");
    auto* events = channels.declare_events("events");
    REQUIRE(scalar != nullptr);
    REQUIRE(meter != nullptr);
    REQUIRE(vector != nullptr);
    REQUIRE(events != nullptr);
    ValueChannelTelemetryBroker broker;
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));

    scalar->publish(std::numeric_limits<float>::quiet_NaN());
    scalar->read();
    pulp::view::MeterFrame meter_frame;
    meter_frame.channels = 1;
    meter_frame.rms[0] = std::numeric_limits<float>::infinity();
    meter_frame.peak[0] = 0.5f;
    meter->publish(meter_frame);
    meter->read();
    const std::array<float, 1> vector_frame{-std::numeric_limits<float>::infinity()};
    vector->publish(vector_frame.data(), static_cast<int>(vector_frame.size()));
    vector->read();
    const pulp::view::ValueEvent event{
        .frame_index = 7,
        .value = std::numeric_limits<float>::quiet_NaN(),
    };
    events->publish(&event, 1);

    const auto response = broker.handle(InspectorRequestContext{.client_id = "reader"},
                                        request(1, pulp::inspect::methods::kTelemetryGetSnapshot));
    REQUIRE_FALSE(response.is_error);
    const auto snapshot = json(response);
    CHECK(channel(snapshot, "scalar")["payload"].getString() == "NaN");
    CHECK(channel(snapshot, "meter")["payload"]["rms"][0].getString() == "Infinity");
    CHECK(channel(snapshot, "vector")["payload"]["values"][0].getString() == "-Infinity");
    CHECK(channel(snapshot, "events")["payload"][0]["value"].getString() == "NaN");
}

TEST_CASE("telemetry subscriptions are contextual bounded and ID scoped",
          "[inspect][telemetry][broker]") {
    ValueChannelSet channels;
    auto* vector = channels.declare_vector("spectrum");
    REQUIRE(vector != nullptr);
    ValueChannelTelemetryBroker::Config config;
    config.max_vector_values = 4;
    ValueChannelTelemetryBroker broker(config);
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));
    std::vector<std::pair<std::string, std::string>> retired;
    broker.set_event_retirement_sink(
        [&](std::string_view client, std::string_view owner) {
            retired.emplace_back(client, owner);
        });

    std::array<float, 8> samples{1, 2, 3, 4, 5, 6, 7, 8};
    vector->publish(samples.data(), static_cast<int>(samples.size()));
    vector->read();
    auto snapshot = broker.handle(InspectorRequestContext{.client_id = "client-a"},
                                  request(1, pulp::inspect::methods::kTelemetryGetSnapshot,
                                          R"({"channels":["spectrum"],"maxVectorValues":2})"));
    REQUIRE_FALSE(snapshot.is_error);
    const auto snapshot_value = json(snapshot);
    const auto spectrum = channel(snapshot_value, "spectrum");
    CHECK(spectrum["payload"]["values"].size() == 2);
    CHECK(spectrum["payload"]["truncated"].getWithDefault(false));

    auto subscribed = broker.handle(
        InspectorRequestContext{.client_id = "client-a"},
        request(
            2, pulp::inspect::methods::kTelemetrySubscribe,
            R"({"channels":["spectrum"],"rateHz":100,"maxVectorValues":999,"clientId":"client-b"})"));
    REQUIRE_FALSE(subscribed.is_error);
    const auto first = json(subscribed);
    const auto first_id = std::string(first["subscriptionId"].getString());
    CHECK(first["schema"].getString() == "pulp.inspect.telemetry.subscription.v1");
    CHECK(first["requestedRateHz"].getWithDefault(0.0) == Catch::Approx(100.0));
    CHECK(first["effectiveRateHz"].getWithDefault(0.0) == Catch::Approx(60.0));
    CHECK(first["maxVectorValues"].getWithDefault<std::int64_t>(0) == 4);
    CHECK(broker.subscription_count() == 1);

    auto replaced = broker.handle(
        InspectorRequestContext{.client_id = "client-a"},
        request(3, pulp::inspect::methods::kTelemetrySubscribe, R"({"channels":["spectrum"]})"));
    REQUIRE_FALSE(replaced.is_error);
    const auto replacement_id = std::string(json(replaced)["subscriptionId"].getString());
    CHECK(replacement_id != first_id);
    CHECK(broker.subscription_count() == 1);
    REQUIRE(retired.size() == 1);
    CHECK((retired[0] == std::pair{std::string("client-a"), first_id}));

    auto mismatch = broker.handle(InspectorRequestContext{.client_id = "client-a"},
                                  request(4, pulp::inspect::methods::kTelemetryUnsubscribe,
                                          R"({"subscriptionId":"telemetry-wrong"})"));
    CHECK(mismatch.is_error);
    CHECK(mismatch.error_code == "subscription_mismatch");
    CHECK(broker.subscription_count() == 1);
    auto removed =
        broker.handle(InspectorRequestContext{.client_id = "client-a"},
                      request(5, pulp::inspect::methods::kTelemetryUnsubscribe,
                              std::string("{\"subscriptionId\":\"") + replacement_id + "\"}"));
    REQUIRE_FALSE(removed.is_error);
    CHECK(json(removed)["unsubscribed"].getWithDefault(false));
    CHECK(broker.subscription_count() == 0);
    REQUIRE(retired.size() == 2);
    CHECK((retired[1] == std::pair{std::string("client-a"), replacement_id}));

    auto bad_cap = broker.handle(InspectorRequestContext{.client_id = "client-a"},
                                 request(6, pulp::inspect::methods::kTelemetrySubscribe,
                                         R"({"channels":["spectrum"],"maxVectorValues":0})"));
    CHECK(bad_cap.is_error);
    auto no_identity = broker.handle(InspectorRequestContext{},
                                     request(7, pulp::inspect::methods::kTelemetrySubscribe));
    CHECK(no_identity.is_error);
    auto malformed = broker.handle(InspectorRequestContext{.client_id = "client-a"},
                                   request(8, pulp::inspect::methods::kTelemetrySubscribe, "{"));
    CHECK(malformed.is_error);
    auto unknown = broker.handle(
        InspectorRequestContext{.client_id = "client-a"},
        request(9, pulp::inspect::methods::kTelemetrySubscribe, R"({"channels":["missing"]})"));
    CHECK(unknown.is_error);
    auto duplicate = broker.handle(InspectorRequestContext{.client_id = "client-a"},
                                   request(10, pulp::inspect::methods::kTelemetrySubscribe,
                                           R"({"channels":["spectrum","spectrum"]})"));
    CHECK(duplicate.is_error);
}

TEST_CASE("telemetry poll retains source and transport loss debt until delivery",
          "[inspect][telemetry][broker]") {
    ValueChannelSet channels;
    auto* events = channels.declare_events("onsets");
    REQUIRE(events != nullptr);
    auto now = std::chrono::steady_clock::now();
    ValueChannelTelemetryBroker::Config config;
    config.max_events_per_channel = 2;
    ValueChannelTelemetryBroker broker(config, [&] { return now; });
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));

    std::vector<InspectorMessage> emitted;
    std::vector<InspectorTargetedEventResult> results{
        InspectorTargetedEventResult::DroppedLossy,
        InspectorTargetedEventResult::Queued,
    };
    broker.set_event_sink(
        [&](std::string_view client, const InspectorMessage& event, std::string_view) {
            CHECK(client == "slow-client");
            emitted.push_back(event);
            const auto result = results.front();
            results.erase(results.begin());
            return result;
        });
    auto subscribed = broker.handle(InspectorRequestContext{.client_id = "slow-client"},
                                    request(1, pulp::inspect::methods::kTelemetrySubscribe,
                                            R"({"channels":["onsets"],"rateHz":60})"));
    REQUIRE_FALSE(subscribed.is_error);

    const pulp::view::ValueEvent event{.frame_index = 7, .value = 1.0f};
    for (int i = 0; i < 20; ++i)
        events->publish(&event, 1);
    broker.poll();
    REQUIRE(emitted.size() == 1);
    const auto first = json(emitted[0]);
    CHECK(first["attemptSequence"].getWithDefault<std::int64_t>(0) == 1);
    const auto first_channel = channel(first, "onsets");
    CHECK(first_channel["sourceDropped"].getWithDefault<std::int64_t>(0) > 0);
    CHECK(first_channel["coalesced"].getWithDefault<std::int64_t>(0) > 0);

    for (int i = 0; i < 20; ++i)
        events->publish(&event, 1);
    now += 20ms;
    broker.poll();
    REQUIRE(emitted.size() == 2);
    const auto delivered = json(emitted[1]);
    CHECK(delivered["attemptSequence"].getWithDefault<std::int64_t>(0) == 2);
    CHECK(delivered["transportDroppedSincePrevious"].getWithDefault<std::int64_t>(0) == 1);
    CHECK(channel(delivered, "onsets")["sourceDropped"].getWithDefault<std::int64_t>(0) >
          first_channel["sourceDropped"].getWithDefault<std::int64_t>(0));
}

TEST_CASE("telemetry poll accumulates debt across repeated targeted evictions",
          "[inspect][telemetry][broker][backpressure]") {
    ValueChannelSet channels;
    REQUIRE(channels.declare_scalar("gain") != nullptr);
    auto now = std::chrono::steady_clock::now();
    ValueChannelTelemetryBroker broker({}, [&] { return now; });
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));

    const std::array results{
        InspectorTargetedEventResult::Queued,
        InspectorTargetedEventResult::QueuedAfterLossyEviction,
        InspectorTargetedEventResult::QueuedAfterLossyEviction,
        InspectorTargetedEventResult::QueuedAfterLossyEviction,
        InspectorTargetedEventResult::Queued,
        InspectorTargetedEventResult::Queued,
    };
    std::vector<std::int64_t> reported_debt;
    std::size_t attempt = 0;
    broker.set_event_sink(
        [&](std::string_view client, const InspectorMessage& event, std::string_view) {
            REQUIRE(client == "slow-client");
            reported_debt.push_back(
                json(event)["transportDroppedSincePrevious"]
                    .getWithDefault<std::int64_t>(-1));
            REQUIRE(attempt < results.size());
            return results[attempt++];
        });
    REQUIRE_FALSE(broker
                      .handle(InspectorRequestContext{.client_id = "slow-client"},
                              request(1, pulp::inspect::methods::kTelemetrySubscribe,
                                      R"({"channels":["gain"],"rateHz":60})"))
                      .is_error);

    for (std::size_t i = 0; i < results.size(); ++i) {
        broker.poll();
        now += 20ms;
    }

    CHECK(reported_debt == std::vector<std::int64_t>{0, 0, 1, 2, 3, 0});
}

TEST_CASE("telemetry subscription starts after the event overflow baseline",
          "[inspect][telemetry][broker][events]") {
    ValueChannelSet channels;
    auto* events = channels.declare_events("onsets");
    REQUIRE(events != nullptr);
    auto now = std::chrono::steady_clock::now();
    ValueChannelTelemetryBroker broker({}, [&] { return now; });
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));

    const pulp::view::ValueEvent occurrence{.frame_index = 3, .value = 1.0f};
    for (int i = 0; i < 20; ++i)
        events->publish(&occurrence, 1);

    std::vector<InspectorMessage> emitted;
    broker.set_event_sink([&](std::string_view, const InspectorMessage& event, std::string_view) {
        emitted.push_back(event);
        return InspectorTargetedEventResult::Queued;
    });
    const auto subscribed = broker.handle(InspectorRequestContext{.client_id = "new-client"},
                                          request(1, pulp::inspect::methods::kTelemetrySubscribe,
                                                  R"({"channels":["onsets"],"rateHz":60})"));
    REQUIRE_FALSE(subscribed.is_error);
    broker.poll();
    REQUIRE(emitted.size() == 1);
    const auto baseline_message = json(emitted[0]);
    const auto baseline = channel(baseline_message, "onsets");
    CHECK(baseline["payload"].size() == 0);
    CHECK(baseline["sourceDropped"].getWithDefault<std::int64_t>(-1) == 0);
    CHECK(baseline["coalesced"].getWithDefault<std::int64_t>(-1) == 0);

    events->publish(&occurrence, 1);
    now += 20ms;
    broker.poll();
    REQUIRE(emitted.size() == 2);
    const auto followup_message = json(emitted[1]);
    CHECK(channel(followup_message, "onsets")["payload"].size() == 1);
}

TEST_CASE("telemetry fans one event drain to isolated client delivery state",
          "[inspect][telemetry][broker][fanout]") {
    ValueChannelSet channels;
    auto* events = channels.declare_events("onsets");
    REQUIRE(events != nullptr);
    auto now = std::chrono::steady_clock::now();
    ValueChannelTelemetryBroker broker({}, [&] { return now; });
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));
    std::vector<InspectorMessage> slow;
    std::vector<InspectorMessage> healthy;
    int slow_attempt = 0;
    broker.set_event_sink(
        [&](std::string_view client, const InspectorMessage& event, std::string_view) {
            if (client == "slow") {
                slow.push_back(event);
                ++slow_attempt;
                return slow_attempt == 1   ? InspectorTargetedEventResult::DroppedLossy
                       : slow_attempt == 2 ? InspectorTargetedEventResult::QueuedAfterLossyEviction
                                           : InspectorTargetedEventResult::Queued;
            }
            healthy.push_back(event);
            return InspectorTargetedEventResult::Queued;
        });
    for (const auto* client : {"slow", "healthy"}) {
        auto response = broker.handle(InspectorRequestContext{.client_id = client},
                                      request(1, pulp::inspect::methods::kTelemetrySubscribe,
                                              R"({"channels":["onsets"],"rateHz":60})"));
        REQUIRE_FALSE(response.is_error);
    }
    const pulp::view::ValueEvent occurrence{.frame_index = 3, .value = 0.0f};
    events->publish(&occurrence, 1);
    broker.poll();
    REQUIRE(slow.size() == 1);
    REQUIRE(healthy.size() == 1);
    auto slow_first = json(slow[0]);
    auto healthy_first = json(healthy[0]);
    CHECK(channel(slow_first, "onsets")["payload"].size() == 1);
    CHECK(channel(healthy_first, "onsets")["payload"].size() == 1);

    now += 20ms;
    broker.poll();
    REQUIRE(slow.size() == 2);
    REQUIRE(healthy.size() == 2);
    auto slow_second = json(slow[1]);
    auto healthy_second = json(healthy[1]);
    CHECK(slow_second["transportDroppedSincePrevious"].getWithDefault<std::int64_t>(0) == 1);
    CHECK(healthy_second["transportDroppedSincePrevious"].getWithDefault<std::int64_t>(-1) == 0);

    now += 20ms;
    broker.poll();
    REQUIRE(slow.size() == 3);
    auto slow_third = json(slow[2]);
    CHECK(slow_third["transportDroppedSincePrevious"].getWithDefault<std::int64_t>(0) == 2);
    broker.disconnect("healthy");
    CHECK(broker.subscription_count() == 1);
}


TEST_CASE("subscription staleness floor expands for low requested rates",
          "[inspect][telemetry][broker][stale]") {
    ValueChannelSet channels;
    auto* scalar = channels.declare_scalar("gain");
    REQUIRE(scalar != nullptr);
    auto now = std::chrono::steady_clock::now();
    ValueChannelTelemetryBroker broker({}, [&] { return now; });
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));
    scalar->publish(1.0f);
    scalar->read();
    std::vector<InspectorMessage> emitted;
    broker.set_event_sink([&](std::string_view, const InspectorMessage& event, std::string_view) {
        emitted.push_back(event);
        return InspectorTargetedEventResult::Queued;
    });
    REQUIRE_FALSE(broker
                      .handle(InspectorRequestContext{.client_id = "reader"},
                              request(1, pulp::inspect::methods::kTelemetrySubscribe,
                                      R"({"channels":["gain"],"rateHz":1})"))
                      .is_error);
    broker.poll();
    now += 1s;
    broker.poll();
    REQUIRE(emitted.size() == 2);
    auto within_rate_window = json(emitted[1]);
    CHECK_FALSE(channel(within_rate_window, "gain")["stale"].getWithDefault(true));
    now += 1100ms;
    broker.poll();
    REQUIRE(emitted.size() == 3);
    auto overdue = json(emitted[2]);
    CHECK(channel(overdue, "gain")["stale"].getWithDefault(false));
}

TEST_CASE("telemetry reports source death once for continuous and events",
          "[inspect][telemetry][broker][lifecycle]") {
    auto channels = std::make_unique<ValueChannelSet>();
    channels->declare_scalar("gain");
    channels->declare_events("onsets");
    auto now = std::chrono::steady_clock::now();
    ValueChannelTelemetryBroker broker({}, [&] { return now; });
    REQUIRE(broker.replace_attachment(channels->attach_telemetry()));
    std::vector<InspectorMessage> emitted;
    broker.set_event_sink([&](std::string_view, const InspectorMessage& event, std::string_view) {
        emitted.push_back(event);
        return InspectorTargetedEventResult::Queued;
    });
    REQUIRE_FALSE(broker
                      .handle(InspectorRequestContext{.client_id = "reader"},
                              request(1, pulp::inspect::methods::kTelemetrySubscribe))
                      .is_error);
    channels.reset();
    broker.poll();
    REQUIRE(emitted.size() == 1);
    auto terminal = json(emitted[0]);
    CHECK(channel(terminal, "gain")["terminal"].getWithDefault(false));
    CHECK(channel(terminal, "onsets")["terminal"].getWithDefault(false));
    now += 1s;
    broker.poll();
    CHECK(emitted.size() == 1);
}

TEST_CASE("telemetry preserves named subscriptions across same-set reattach",
          "[inspect][telemetry][broker][reload]") {
    ValueChannelSet channels;
    auto* scalar = channels.declare_scalar("gain");
    REQUIRE(scalar != nullptr);
    auto now = std::chrono::steady_clock::now();
    ValueChannelTelemetryBroker broker({}, [&] { return now; });
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));
    std::vector<InspectorMessage> emitted;
    broker.set_event_sink([&](std::string_view, const InspectorMessage& event, std::string_view) {
        emitted.push_back(event);
        return InspectorTargetedEventResult::Queued;
    });
    auto response = broker.handle(
        InspectorRequestContext{.client_id = "reload-client"},
        request(1, pulp::inspect::methods::kTelemetrySubscribe, R"({"channels":["gain"]})"));
    REQUIRE_FALSE(response.is_error);
    const auto id = std::string(json(response)["subscriptionId"].getString());

    broker.clear_attachment();
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));
    scalar->publish(0.75f);
    scalar->read();
    broker.poll();
    REQUIRE(emitted.size() == 1);
    const auto sample = json(emitted[0]);
    CHECK(sample["subscriptionId"].getString() == id);
    CHECK(sample["reattached"].getWithDefault(false));
    CHECK(sample["sourceGeneration"].getWithDefault<std::int64_t>(0) == 2);
    CHECK(broker.subscription_count() == 1);

    now += 100ms;
    broker.replace_with_empty_source();
    broker.poll();
    REQUIRE(emitted.size() == 2);
    const auto empty_sample = json(emitted[1]);
    CHECK(empty_sample["sourceGeneration"].getWithDefault<std::int64_t>(0) == 3);
    CHECK(empty_sample["reattached"].getWithDefault(false));
    CHECK(channel(empty_sample, "gain")["staleReason"].getString() == "unavailable_after_reattach");

    broker.clear_attachment();
    ValueChannelSet replacement;
    replacement.declare_scalar("different");
    REQUIRE(broker.replace_attachment(replacement.attach_telemetry()));
    broker.poll();
    REQUIRE(emitted.size() == 3);
    const auto removed_sample = json(emitted[2]);
    const auto removed = channel(removed_sample, "gain");
    CHECK(removed["staleReason"].getString() == "unavailable_after_reattach");
    CHECK(removed_sample["reattached"].getWithDefault(false));

    // Invalid replacement is explicit and clears the current claim.
    CHECK_FALSE(broker.replace_attachment({}));
    CHECK(replacement.attach_telemetry().valid());
}

TEST_CASE("telemetry enforces channel and wire ceilings with typed terminal",
          "[inspect][telemetry][broker][bounds]") {
    ValueChannelSet channels;
    std::string requested = R"({"channels":[)";
    for (int i = 0; i < 33; ++i) {
        const auto name = std::string(90, static_cast<char>('a' + (i % 20))) + std::to_string(i);
        REQUIRE(channels.declare_scalar(name) != nullptr);
        if (i != 0)
            requested += ',';
        requested += '"' + name + '"';
    }
    requested += "]}";
    ValueChannelTelemetryBroker::Config config;
    config.max_wire_bytes = 1024;
    ValueChannelTelemetryBroker broker(config);
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));
    const auto final_name = std::string(90, static_cast<char>('a' + (32 % 20))) + "32";
    auto selected_final =
        broker.handle(InspectorRequestContext{.client_id = "bounded"},
                      request(0, pulp::inspect::methods::kTelemetryGetSnapshot,
                              std::string("{\"channels\":[\"") + final_name + "\"]}"));
    REQUIRE_FALSE(selected_final.is_error);
    const auto selected_final_json = json(selected_final);
    CHECK(channel(selected_final_json, final_name).isObject());
    auto over_quota =
        broker.handle(InspectorRequestContext{.client_id = "bounded"},
                      request(1, pulp::inspect::methods::kTelemetrySubscribe, requested));
    CHECK(over_quota.is_error);
    const auto implicit_over_quota =
        broker.handle(InspectorRequestContext{.client_id = "bounded"},
                      request(2, pulp::inspect::methods::kTelemetrySubscribe));
    CHECK(implicit_over_quota.is_error);
    const std::string unbounded_unknown_name(64u * 1024u, 'z');
    const auto unknown =
        broker.handle(InspectorRequestContext{.client_id = "bounded"},
                      request(3, pulp::inspect::methods::kTelemetryGetSnapshot,
                              std::string("{\"channels\":[\"") + unbounded_unknown_name + "\"]}"));
    CHECK(unknown.is_error);
    CHECK(unknown.params_json.find(unbounded_unknown_name) == std::string::npos);
    CHECK(pulp::inspect::encode_message(unknown).size() <= config.max_wire_bytes);
    const auto duplicate = broker.handle(
        InspectorRequestContext{.client_id = "bounded"},
        request(4, pulp::inspect::methods::kTelemetryGetSnapshot,
                std::string("{\"channels\":[\"") + final_name + "\",\"" + final_name + "\"]}"));
    CHECK(duplicate.is_error);
    CHECK(duplicate.params_json.find(final_name) == std::string::npos);
    CHECK(pulp::inspect::encode_message(duplicate).size() <= config.max_wire_bytes);

    const std::string oversized_subscription_id(2048, 's');
    const auto oversized_unsubscribe = broker.handle(
        InspectorRequestContext{.client_id = "bounded"},
        request(5, pulp::inspect::methods::kTelemetryUnsubscribe,
                std::string("{\"subscriptionId\":\"") + oversized_subscription_id + "\"}"));
    CHECK(oversized_unsubscribe.is_error);
    CHECK(oversized_unsubscribe.error_code == "invalid_params");
    CHECK(oversized_unsubscribe.params_json.find(oversized_subscription_id) == std::string::npos);
    CHECK(pulp::inspect::encode_message(oversized_unsubscribe).size() <= config.max_wire_bytes);

    ValueChannelSet terminal_channels;
    for (int i = 0; i < 8; ++i)
        REQUIRE(terminal_channels.declare_scalar("channel-" + std::to_string(i)) != nullptr);
    ValueChannelTelemetryBroker terminal_broker(config);
    REQUIRE(terminal_broker.replace_attachment(terminal_channels.attach_telemetry()));
    std::vector<InspectorMessage> emitted;
    terminal_broker.set_event_sink(
        [&](std::string_view, const InspectorMessage& event, std::string_view) {
            emitted.push_back(event);
            return InspectorTargetedEventResult::Queued;
        });
    auto subscribed =
        terminal_broker.handle(InspectorRequestContext{.client_id = "bounded"},
                               request(6, pulp::inspect::methods::kTelemetrySubscribe));
    REQUIRE_FALSE(subscribed.is_error);
    terminal_broker.poll();
    REQUIRE(emitted.size() == 1);
    const auto terminal = json(emitted[0]);
    CHECK(terminal["wireTruncated"].getWithDefault(false));
    CHECK(terminal["terminalReason"].getString() == "wire_limit");
    CHECK(terminal["subscriptionId"].isString());
    CHECK(terminal["maxVectorValues"].isInt());
    CHECK(pulp::inspect::encode_message(emitted[0]).size() <= 1024);
    CHECK(terminal_broker.subscription_count() == 0);
}

TEST_CASE("telemetry wire ceiling includes the encoded protocol envelope",
          "[inspect][telemetry][broker][bounds]") {
    const auto now = std::chrono::steady_clock::time_point(123456789ns);
    const auto emit_scalar_sample = [&](std::string name, std::size_t wire_bytes) {
        ValueChannelSet channels;
        REQUIRE(channels.declare_scalar(name) != nullptr);
        ValueChannelTelemetryBroker::Config config;
        config.max_wire_bytes = wire_bytes;
        ValueChannelTelemetryBroker broker(config, [=] { return now; });
        REQUIRE(broker.replace_attachment(channels.attach_telemetry()));
        std::vector<InspectorMessage> emitted;
        broker.set_event_sink(
            [&](std::string_view, const InspectorMessage& event, std::string_view) {
                emitted.push_back(event);
                return InspectorTargetedEventResult::Queued;
            });
        const auto subscribed =
            broker.handle(InspectorRequestContext{.client_id = "bounded"},
                          request(1, pulp::inspect::methods::kTelemetrySubscribe,
                                  std::string("{\"channels\":[\"") + name + "\"]}"));
        REQUIRE_FALSE(subscribed.is_error);
        broker.poll();
        REQUIRE(emitted.size() == 1);
        return emitted.front();
    };

    const auto probe = emit_scalar_sample("x", 4096);
    const auto envelope_bytes =
        pulp::inspect::encode_message(probe).size() - probe.params_json.size();
    REQUIRE(envelope_bytes > 0);
    const auto base_params_bytes = probe.params_json.size() - 1;
    REQUIRE(base_params_bytes + 1 < 1024);
    const auto target_params_bytes = 1024 - envelope_bytes / 2;
    REQUIRE(target_params_bytes > base_params_bytes);
    const std::string boundary_name(target_params_bytes - base_params_bytes, 'x');
    const auto bounded = emit_scalar_sample(boundary_name, 1024);
    const auto bounded_json = json(bounded);
    CHECK(bounded_json["wireTruncated"].getWithDefault(false));
    CHECK(pulp::inspect::encode_message(bounded).size() <= 1024);
}

TEST_CASE("oversize subscription responses fail without storing state",
          "[inspect][telemetry][broker][bounds]") {
    ValueChannelSet channels;
    const std::string long_name(2000, 'x');
    REQUIRE(channels.declare_scalar(long_name) != nullptr);
    ValueChannelTelemetryBroker::Config config;
    config.max_wire_bytes = 1024;
    ValueChannelTelemetryBroker broker(config);
    REQUIRE(broker.replace_attachment(channels.attach_telemetry()));
    const auto response =
        broker.handle(InspectorRequestContext{.client_id = "bounded"},
                      request(1, pulp::inspect::methods::kTelemetrySubscribe,
                              std::string("{\"channels\":[\"") + long_name + "\"]}"));
    CHECK(response.is_error);
    CHECK(response.error_code == "telemetry_payload_too_large");
    CHECK(broker.subscription_count() == 0);
}

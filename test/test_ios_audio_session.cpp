// Unit tests for the iOS AVAudioSession ↔ C++ bridge (workstream 05 slice 5.2).
// Platform-agnostic: these run on every platform; the Swift side is
// exercised by iOS integration tests once the bridge is wired into a
// real AUv3 example (workstream 05 slice 5.1).

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/ios_audio_session.hpp>

#include <vector>

using namespace pulp::format;

namespace {

struct Captured {
    PulpIosAudioEvent event;
    int32_t reason;
    int32_t options;
};

} // namespace

TEST_CASE("emit returns false when no listener is attached",
          "[ios][audio-session]") {
    set_ios_audio_session_listener({});
    PulpIosAudioSessionEvent e{};
    e.event = PULP_IOS_AUDIO_EVENT_INTERRUPTION_BEGAN;
    REQUIRE(emit_ios_audio_session_event(e) == false);
}

TEST_CASE("listener receives events", "[ios][audio-session]") {
    std::vector<Captured> calls;
    set_ios_audio_session_listener(
        [&](const PulpIosAudioSessionEvent& e) {
            calls.push_back({e.event, e.reason, e.options});
        });

    PulpIosAudioSessionEvent begin_evt{};
    begin_evt.event = PULP_IOS_AUDIO_EVENT_INTERRUPTION_BEGAN;
    REQUIRE(emit_ios_audio_session_event(begin_evt));

    PulpIosAudioSessionEvent end_evt{};
    end_evt.event = PULP_IOS_AUDIO_EVENT_INTERRUPTION_ENDED;
    end_evt.options = PULP_IOS_INTERRUPTION_OPTION_SHOULD_RESUME;
    REQUIRE(emit_ios_audio_session_event(end_evt));

    PulpIosAudioSessionEvent route_evt{};
    route_evt.event = PULP_IOS_AUDIO_EVENT_ROUTE_CHANGED;
    route_evt.reason = PULP_IOS_ROUTE_CHANGE_NEW_DEVICE_AVAILABLE;
    REQUIRE(emit_ios_audio_session_event(route_evt));

    REQUIRE(calls.size() == 3);
    REQUIRE(calls[0].event == PULP_IOS_AUDIO_EVENT_INTERRUPTION_BEGAN);
    REQUIRE(calls[1].event == PULP_IOS_AUDIO_EVENT_INTERRUPTION_ENDED);
    REQUIRE(calls[1].options == PULP_IOS_INTERRUPTION_OPTION_SHOULD_RESUME);
    REQUIRE(calls[2].event == PULP_IOS_AUDIO_EVENT_ROUTE_CHANGED);
    REQUIRE(calls[2].reason == PULP_IOS_ROUTE_CHANGE_NEW_DEVICE_AVAILABLE);

    set_ios_audio_session_listener({});
}

TEST_CASE("C ABI entry routes to the C++ listener",
          "[ios][audio-session][c-abi]") {
    int calls = 0;
    set_ios_audio_session_listener(
        [&](const PulpIosAudioSessionEvent&) { ++calls; });
    PulpIosAudioSessionEvent e{};
    e.event = PULP_IOS_AUDIO_EVENT_MEDIA_SERVICES_RESET;
    pulp_ios_audio_session_emit(&e);
    REQUIRE(calls == 1);

    // When the C++ listener is detached, the raw C callback is used as
    // a fallback. Install one via the C ABI.
    set_ios_audio_session_listener({});
    int c_calls = 0;
    pulp_ios_audio_session_set_callback(
        [](const PulpIosAudioSessionEvent*, void* ud) {
            ++(*static_cast<int*>(ud));
        },
        &c_calls);
    pulp_ios_audio_session_emit(&e);
    REQUIRE(c_calls == 1);

    pulp_ios_audio_session_set_callback(nullptr, nullptr);
}

TEST_CASE("to_string covers every event code",
          "[ios][audio-session]") {
    REQUIRE(to_string(PULP_IOS_AUDIO_EVENT_NONE) == "none");
    REQUIRE(to_string(PULP_IOS_AUDIO_EVENT_INTERRUPTION_BEGAN) == "interruption_began");
    REQUIRE(to_string(PULP_IOS_AUDIO_EVENT_INTERRUPTION_ENDED) == "interruption_ended");
    REQUIRE(to_string(PULP_IOS_AUDIO_EVENT_ROUTE_CHANGED) == "route_changed");
    REQUIRE(to_string(PULP_IOS_AUDIO_EVENT_MEDIA_SERVICES_RESET) == "media_services_reset");
}

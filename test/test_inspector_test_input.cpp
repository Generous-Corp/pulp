#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/test_input.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using namespace pulp::inspect;

namespace {

class RecordingTestInputSource final : public InspectorTestInputSource {
  public:
    TestInputApplyResult inject_midi(const MidiTestInput& input) override {
        midi = input;
        ++midi_calls;
        return midi_result;
    }

    TestInputApplyResult set_transport(const StandaloneTransportTestInput& input) override {
        transport = input;
        ++transport_calls;
        return transport_result;
    }

    void release_test_input(TestInputReleaseReason reason) noexcept override {
        release_reason = reason;
        ++release_calls;
    }

    MidiTestInput midi;
    StandaloneTransportTestInput transport;
    TestInputApplyResult midi_result = TestInputApplyResult::success();
    TestInputApplyResult transport_result = TestInputApplyResult::success();
    TestInputReleaseReason release_reason = TestInputReleaseReason::ControllerReleased;
    int midi_calls = 0;
    int transport_calls = 0;
    int release_calls = 0;
};

InspectorMessage request(std::int64_t id, std::string method, std::string params) {
    return make_request(id, std::move(method), std::move(params));
}

} // namespace

TEST_CASE("test input methods are typed control capabilities", "[inspect][test-input][policy]") {
    const auto* midi = find_inspector_method(methods::kTestInjectMidi);
    const auto* transport = find_inspector_method(methods::kTestSetTransport);
    REQUIRE(midi != nullptr);
    REQUIRE(transport != nullptr);
    CHECK(midi->capability == InspectorCapability::TestInput);
    CHECK(transport->capability == InspectorCapability::TestInput);
    CHECK(capability_requires_controller_lease(midi->capability));
    CHECK(capability_requires_controller_lease(transport->capability));

    const auto develop = profile_capabilities(InspectorProfile::Develop);
    const auto observe = profile_capabilities(InspectorProfile::Observe);
    CHECK(std::find(develop.begin(), develop.end(), InspectorCapability::TestInput) !=
          develop.end());
    CHECK(std::find(observe.begin(), observe.end(), InspectorCapability::TestInput) ==
          observe.end());
    CHECK(std::find(develop.begin(), develop.end(), InspectorCapability::RuntimeEval) ==
          develop.end());

    CHECK(find_inspector_method("State.setParameter")->capability ==
          InspectorCapability::StateWrite);
    CHECK(find_inspector_method("DOM.highlightNode")->capability ==
          InspectorCapability::AuthoringTweaks);
    CHECK(find_inspector_method("Inspector.setBypass")->capability ==
          InspectorCapability::AuthoringTweaks);
    CHECK(find_inspector_method("Inspector.loadTweaks")->capability ==
          InspectorCapability::Unavailable);
    CHECK(find_inspector_method("Inspector.saveTweaks")->capability ==
          InspectorCapability::Unavailable);
    CHECK(find_inspector_method("Inspector.jumpToSource")->capability ==
          InspectorCapability::Unavailable);
}

TEST_CASE("Test.injectMidi accepts only bounded note events", "[inspect][test-input][midi]") {
    RecordingTestInputSource source;
    TestInputDomain domain;
    domain.set_source(&source);

    auto response = domain.handle(request(
        1, methods::kTestInjectMidi, R"({"kind":"note_on","channel":16,"note":127,"velocity":1})"));
    REQUIRE_FALSE(response.is_error);
    CHECK(response.params_json == R"({"accepted":true})");
    CHECK(source.midi_calls == 1);
    CHECK(source.midi.kind == MidiTestInputKind::NoteOn);
    CHECK(source.midi.channel == 15);
    CHECK(source.midi.note == 127);
    CHECK(source.midi.velocity == 1);

    response = domain.handle(
        request(2, methods::kTestInjectMidi, R"({"kind":"note_off","channel":1,"note":0})"));
    REQUIRE_FALSE(response.is_error);
    CHECK(source.midi.kind == MidiTestInputKind::NoteOff);
    CHECK(source.midi.channel == 0);
    CHECK(source.midi.velocity == 0);

    for (const auto* invalid : {
             R"({})",
             R"({"kind":"note_on","channel":1,"note":60})",
             R"({"kind":"cc","channel":1,"note":60,"velocity":1})",
             R"({"kind":"note_on","channel":0,"note":60,"velocity":1})",
             R"({"kind":"note_on","channel":17,"note":60,"velocity":1})",
             R"({"kind":"note_on","channel":1,"note":128,"velocity":1})",
             R"({"kind":"note_on","channel":1,"note":60,"velocity":128})",
             R"({"kind":"note_on","channel":1.0,"note":60,"velocity":1})",
             R"({"kind":"note_on","channel":1,"channel":2,"note":60,"velocity":1})",
             R"({"kind":"note_on","channel":1,"note":60,"velocity":1,"raw":144})",
             R"([])",
             R"({not-json})",
         }) {
        const auto rejected = domain.handle(request(3, methods::kTestInjectMidi, invalid));
        CHECK(rejected.is_error);
        CHECK(rejected.error_code == "invalid_params");
    }
    CHECK(source.midi_calls == 2);
}

TEST_CASE("Test.setTransport is bounded and idempotent", "[inspect][test-input][transport]") {
    RecordingTestInputSource source;
    TestInputDomain domain;
    domain.set_source(&source);

    auto response =
        domain.handle(request(1, methods::kTestSetTransport,
                              R"({"playing":false,"position_samples":0,"tempo_bpm":400})"));
    REQUIRE_FALSE(response.is_error);
    CHECK(response.params_json == R"({"applied":true})");
    REQUIRE(source.transport.playing.has_value());
    CHECK_FALSE(*source.transport.playing);
    REQUIRE(source.transport.position_samples.has_value());
    CHECK(*source.transport.position_samples == 0);
    REQUIRE(source.transport.tempo_bpm.has_value());
    CHECK(*source.transport.tempo_bpm == 400.0);

    for (const auto* invalid : {
             R"({})",
             R"({"playing":0})",
             R"({"position_samples":-1})",
             R"({"position_samples":1.5})",
             R"({"tempo_bpm":19.999})",
             R"({"tempo_bpm":400.001})",
             R"({"tempo_bpm":"120"})",
             R"({"playing":true,"position":12})",
         }) {
        const auto rejected = domain.handle(request(2, methods::kTestSetTransport, invalid));
        CHECK(rejected.is_error);
        CHECK(rejected.error_code == "invalid_params");
    }
    CHECK(source.transport_calls == 1);
}

TEST_CASE("test input reports unavailable and source failures without applying",
          "[inspect][test-input][errors]") {
    TestInputDomain missing;
    const auto unavailable = missing.handle(
        request(1, methods::kTestInjectMidi, R"({"kind":"note_off","channel":1,"note":60})"));
    REQUIRE(unavailable.is_error);
    CHECK(unavailable.error_code == "test_input_unavailable");

    RecordingTestInputSource source;
    source.midi_result =
        TestInputApplyResult::failure("input_queue_full", "The bounded input queue is full");
    TestInputDomain domain;
    domain.set_source(&source);
    const auto rejected = domain.handle(request(
        2, methods::kTestInjectMidi, R"({"kind":"note_on","channel":1,"note":60,"velocity":100})"));
    REQUIRE(rejected.is_error);
    CHECK(rejected.error_code == "input_queue_full");
}

TEST_CASE("test input cleanup forwards the lifecycle reason", "[inspect][test-input][teardown]") {
    RecordingTestInputSource source;
    TestInputDomain domain;
    domain.set_source(&source);
    domain.release(TestInputReleaseReason::SessionTeardown);
    CHECK(source.release_calls == 1);
    CHECK(source.release_reason == TestInputReleaseReason::SessionTeardown);
}

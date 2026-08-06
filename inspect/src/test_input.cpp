#include <pulp/inspect/test_input.hpp>

#include <choc/text/choc_JSON.h>

#include <cmath>
#include <limits>
#include <string_view>

namespace pulp::inspect {
namespace {

InspectorMessage invalid_params(std::int64_t request_id, std::string message) {
    return make_error(request_id, std::move(message), "invalid_params");
}

bool has_only_members(choc::value::ValueView object,
                      std::initializer_list<std::string_view> allowed) {
    if (!object.isObject())
        return false;
    for (std::uint32_t index = 0; index < object.size(); ++index) {
        const auto member = object.getObjectMemberAt(index);
        for (std::uint32_t prior = 0; prior < index; ++prior) {
            if (object.getObjectMemberAt(prior).name == member.name)
                return false;
        }
        bool known = false;
        for (const auto name : allowed) {
            if (member.name == name) {
                known = true;
                break;
            }
        }
        if (!known)
            return false;
    }
    return true;
}

std::optional<std::int64_t> exact_integer(choc::value::ValueView value) {
    if (value.isInt32())
        return value.getInt32();
    if (value.isInt64())
        return value.getInt64();
    return std::nullopt;
}

InspectorMessage apply_result(std::int64_t request_id, const TestInputApplyResult& result,
                              std::string_view success_json) {
    if (result.applied)
        return make_response(request_id, std::string(success_json));
    return make_error(
        request_id, result.error_message.empty() ? "Test input was rejected" : result.error_message,
        result.error_code.empty() ? "test_input_rejected" : result.error_code);
}

InspectorMessage handle_midi(InspectorTestInputSource& source, const InspectorMessage& request) {
    choc::value::Value params;
    try {
        params = choc::json::parse(request.params_json);
    } catch (...) {
        return invalid_params(request.id, "Test.injectMidi params must be valid JSON");
    }
    if (!has_only_members(params, {"kind", "channel", "note", "velocity"})) {
        return invalid_params(request.id,
                              "Test.injectMidi params must be an exact MIDI note object");
    }
    if (!params.hasObjectMember("kind") || !params["kind"].isString() ||
        !params.hasObjectMember("channel") || !params.hasObjectMember("note")) {
        return invalid_params(request.id, "Test.injectMidi requires kind, channel, and note");
    }

    MidiTestInput input;
    const auto kind = params["kind"].getString();
    if (kind == "note_on") {
        input.kind = MidiTestInputKind::NoteOn;
        if (!params.hasObjectMember("velocity")) {
            return invalid_params(request.id, "Test.injectMidi note_on requires velocity");
        }
    } else if (kind == "note_off") {
        input.kind = MidiTestInputKind::NoteOff;
    } else {
        return invalid_params(request.id, "Test.injectMidi kind must be note_on or note_off");
    }

    const auto channel = exact_integer(params["channel"]);
    const auto note = exact_integer(params["note"]);
    const auto velocity = params.hasObjectMember("velocity") ? exact_integer(params["velocity"])
                                                             : std::optional<std::int64_t>(0);
    if (!channel || *channel < 1 || *channel > 16) {
        return invalid_params(request.id, "Test.injectMidi channel must be 1 through 16");
    }
    if (!note || *note < 0 || *note > 127) {
        return invalid_params(request.id, "Test.injectMidi note must be 0 through 127");
    }
    if (!velocity || *velocity < 0 || *velocity > 127) {
        return invalid_params(request.id, "Test.injectMidi velocity must be 0 through 127");
    }

    input.channel = static_cast<std::uint8_t>(*channel - 1);
    input.note = static_cast<std::uint8_t>(*note);
    input.velocity = static_cast<std::uint8_t>(*velocity);
    return apply_result(request.id, source.inject_midi(input), R"({"accepted":true})");
}

InspectorMessage handle_transport(InspectorTestInputSource& source,
                                  const InspectorMessage& request) {
    choc::value::Value params;
    try {
        params = choc::json::parse(request.params_json);
    } catch (...) {
        return invalid_params(request.id, "Test.setTransport params must be valid JSON");
    }
    if (!has_only_members(params, {"playing", "position_samples", "tempo_bpm"})) {
        return invalid_params(request.id, "Test.setTransport params contain an unknown field");
    }

    StandaloneTransportTestInput input;
    if (params.hasObjectMember("playing")) {
        if (!params["playing"].isBool()) {
            return invalid_params(request.id, "Test.setTransport playing must be boolean");
        }
        input.playing = params["playing"].getBool();
    }
    if (params.hasObjectMember("position_samples")) {
        const auto position = exact_integer(params["position_samples"]);
        if (!position || *position < 0) {
            return invalid_params(
                request.id, "Test.setTransport position_samples must be a nonnegative integer");
        }
        input.position_samples = *position;
    }
    if (params.hasObjectMember("tempo_bpm")) {
        const auto value = params["tempo_bpm"];
        if (!value.isFloat32() && !value.isFloat64() && !value.isInt32() && !value.isInt64()) {
            return invalid_params(request.id, "Test.setTransport tempo_bpm must be numeric");
        }
        double tempo = std::numeric_limits<double>::quiet_NaN();
        if (value.isFloat64())
            tempo = value.getFloat64();
        else if (value.isFloat32())
            tempo = value.getFloat32();
        else if (value.isInt64())
            tempo = static_cast<double>(value.getInt64());
        else if (value.isInt32())
            tempo = value.getInt32();
        if (!std::isfinite(tempo) || tempo < 20.0 || tempo > 400.0) {
            return invalid_params(
                request.id, "Test.setTransport tempo_bpm must be finite and between 20 and 400");
        }
        input.tempo_bpm = tempo;
    }
    if (!input.playing && !input.position_samples && !input.tempo_bpm) {
        return invalid_params(request.id,
                              "Test.setTransport requires at least one transport field");
    }

    return apply_result(request.id, source.set_transport(input), R"({"applied":true})");
}

} // namespace

InspectorMessage TestInputDomain::handle(const InspectorMessage& request) const {
    if (!source_) {
        return make_error(request.id, "No standalone test input source is attached",
                          "test_input_unavailable");
    }
    if (request.method == methods::kTestInjectMidi)
        return handle_midi(*source_, request);
    if (request.method == methods::kTestSetTransport)
        return handle_transport(*source_, request);
    return make_error(request.id, "Unknown Test method", "method_not_found");
}

void TestInputDomain::release(TestInputReleaseReason reason) const noexcept {
    if (source_)
        source_->release_test_input(reason);
}

} // namespace pulp::inspect

#pragma once

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace pulp_mcp::detail {

struct InspectorMidiArguments {
    std::string kind;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    std::chrono::milliseconds hold_duration{0};
};

struct InspectorTransportArguments {
    std::optional<bool> playing;
    std::optional<std::int64_t> position_samples;
    std::optional<double> tempo_bpm;
};

std::optional<InspectorMidiArguments>
parse_inspector_midi_arguments(const choc::value::Value& arguments, std::string& error);

std::optional<InspectorTransportArguments>
parse_inspector_transport_arguments(const choc::value::Value& arguments, std::string& error);

} // namespace pulp_mcp::detail

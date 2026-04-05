#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/state/parameter.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace pulp::format::aax {

struct PluginCodes {
    uint32_t manufacturer_id = 0;
    uint32_t product_id = 0;
    uint32_t native_id_base = 0;
};

enum class StemKind {
    none,
    mono,
    stereo,
    lcr,
    lcrs,
    quad,
    surround_5_0,
    surround_5_1,
    surround_6_0,
    surround_6_1,
    surround_7_0,
    surround_7_1,
};

struct ParameterBinding {
    state::ParamID id = 0;
    std::string aax_id;
    std::string name;
    std::string unit;
    state::ParamRange range;
    std::function<std::string(float)> to_string;
    std::function<float(const std::string&)> from_string;
    bool discrete = false;
    uint32_t step_count = 0;
};

struct ComponentLayout {
    StemKind input_stem = StemKind::none;
    StemKind output_stem = StemKind::none;
    int main_input_channels = 0;
    int main_output_channels = 0;
    int sidechain_channels = 0;
    uint32_t native_plugin_id = 0;
    std::string short_name;
    std::string description;
};

struct PluginDefinition {
    PluginDescriptor descriptor;
    PluginCodes codes;
    std::vector<ParameterBinding> parameters;
    std::vector<ComponentLayout> components;
    bool supports_midi_input = false;
    bool supports_midi_output = false;
    bool uses_transport = true;
    bool supports_sidechain = false;
    int latency_samples = 0;
    std::size_t packet_float_count = 1; // slot 0 is reserved for AAX master bypass
};

struct DefinitionResult {
    bool ok = false;
    PluginDefinition definition;
    std::string error;
};

uint32_t fourcc(std::string_view text);
std::string fourcc_string(uint32_t value);
std::string parameter_id_string(state::ParamID id);
uint32_t derive_native_plugin_id(uint32_t base_id, std::size_t variant_index);

DefinitionResult build_plugin_definition(ProcessorFactory factory, const PluginCodes& codes);

int stem_channel_count(StemKind stem);
const char* stem_signature(StemKind stem);
StemKind stem_kind_for_channels(int channels);

} // namespace pulp::format::aax

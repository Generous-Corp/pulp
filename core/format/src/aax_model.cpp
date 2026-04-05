#include <pulp/format/aax_model.hpp>

#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>

namespace pulp::format::aax {

namespace {

struct StemMapping {
    int channels;
    StemKind stem;
    const char* signature;
};

constexpr StemMapping kStemMappings[] = {
    {0, StemKind::none, "none"},
    {1, StemKind::mono, "mono"},
    {2, StemKind::stereo, "stereo"},
    {3, StemKind::lcr, "lcr"},
    {4, StemKind::quad, "quad"},
    {5, StemKind::surround_5_0, "5.0"},
    {6, StemKind::surround_5_1, "5.1"},
    {7, StemKind::surround_6_1, "6.1"},
    {8, StemKind::surround_7_1, "7.1"},
};

std::string truncate_copy(std::string value, std::size_t max_size) {
    if (value.size() <= max_size) {
        return value;
    }
    value.resize(max_size);
    return value;
}

std::string component_label(const PluginDescriptor& descriptor,
                            StemKind input_stem,
                            StemKind output_stem,
                            int sidechain_channels)
{
    std::ostringstream out;
    out << descriptor.name;
    out << " " << stem_signature(input_stem) << " -> " << stem_signature(output_stem);
    if (sidechain_channels > 0) {
        out << " + SC";
    }
    return out.str();
}

bool invalid_code(uint32_t code) {
    return code == 0;
}

} // namespace

uint32_t fourcc(std::string_view text) {
    if (text.size() != 4) {
        return 0;
    }

    return (static_cast<uint32_t>(static_cast<unsigned char>(text[0])) << 24)
         | (static_cast<uint32_t>(static_cast<unsigned char>(text[1])) << 16)
         | (static_cast<uint32_t>(static_cast<unsigned char>(text[2])) << 8)
         | static_cast<uint32_t>(static_cast<unsigned char>(text[3]));
}

std::string fourcc_string(uint32_t value) {
    std::string text(4, '\0');
    text[0] = static_cast<char>((value >> 24) & 0xff);
    text[1] = static_cast<char>((value >> 16) & 0xff);
    text[2] = static_cast<char>((value >> 8) & 0xff);
    text[3] = static_cast<char>(value & 0xff);
    return text;
}

std::string parameter_id_string(state::ParamID id) {
    std::array<char, 16> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "p%08X", id);
    return std::string(buffer.data());
}

uint32_t derive_native_plugin_id(uint32_t base_id, std::size_t variant_index) {
    if (variant_index == 0) {
        return base_id;
    }

    static constexpr std::string_view kSuffixes =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    auto text = fourcc_string(base_id);
    if (variant_index < kSuffixes.size()) {
        text[3] = kSuffixes[variant_index];
        return fourcc(text);
    }

    return base_id ^ static_cast<uint32_t>(variant_index * 0x45d9f3bu);
}

int stem_channel_count(StemKind stem) {
    for (const auto& mapping : kStemMappings) {
        if (mapping.stem == stem) {
            return mapping.channels;
        }
    }
    return 0;
}

const char* stem_signature(StemKind stem) {
    for (const auto& mapping : kStemMappings) {
        if (mapping.stem == stem) {
            return mapping.signature;
        }
    }
    return "unknown";
}

StemKind stem_kind_for_channels(int channels) {
    for (const auto& mapping : kStemMappings) {
        if (mapping.channels == channels) {
            return mapping.stem;
        }
    }
    return StemKind::none;
}

DefinitionResult build_plugin_definition(ProcessorFactory factory, const PluginCodes& codes) {
    DefinitionResult result;

    if (!factory) {
        result.error = "AAX factory function is null";
        return result;
    }

    if (invalid_code(codes.manufacturer_id) || invalid_code(codes.product_id) || invalid_code(codes.native_id_base)) {
        result.error = "AAX plugin identifiers must be non-zero";
        return result;
    }

    auto processor = factory();
    if (!processor) {
        result.error = "AAX factory did not return a processor instance";
        return result;
    }

    PluginDefinition definition;
    definition.codes = codes;
    definition.descriptor = processor->descriptor();
    definition.supports_midi_input = definition.descriptor.accepts_midi;
    definition.supports_midi_output = definition.descriptor.produces_midi;
    definition.uses_transport = true;
    definition.latency_samples = processor->latency_samples();

    if (definition.descriptor.output_buses.size() > 1) {
        result.error = "AAX currently supports exactly one output bus per Pulp plugin";
        return result;
    }

    if (definition.descriptor.output_buses.empty()) {
        result.error = "AAX requires at least one declared output bus";
        return result;
    }

    const auto& main_output_bus = definition.descriptor.output_buses.front();
    if (main_output_bus.optional) {
        result.error = "AAX does not support an optional main output bus";
        return result;
    }

    int main_input_channels = 0;
    int sidechain_channels = 0;

    if (definition.descriptor.input_buses.size() > 2) {
        result.error = "AAX currently supports one main input bus and one optional mono sidechain";
        return result;
    }

    if (!definition.descriptor.input_buses.empty()) {
        const auto& main_input_bus = definition.descriptor.input_buses.front();
        if (main_input_bus.optional) {
            result.error = "AAX does not support an optional main input bus";
            return result;
        }
        main_input_channels = main_input_bus.default_channels;
    }

    if (definition.descriptor.input_buses.size() == 2) {
        const auto& sidechain_bus = definition.descriptor.input_buses[1];
        if (!sidechain_bus.optional) {
            result.error = "AAX secondary input bus must be optional to be treated as sidechain";
            return result;
        }
        if (sidechain_bus.default_channels != 1) {
            result.error = "AAX sidechain support is limited to a mono optional bus";
            return result;
        }
        sidechain_channels = sidechain_bus.default_channels;
        definition.supports_sidechain = true;
    }

    auto input_stem = stem_kind_for_channels(main_input_channels);
    auto output_stem = stem_kind_for_channels(main_output_bus.default_channels);

    if (main_input_channels > 0 && input_stem == StemKind::none) {
        result.error = "AAX does not support the requested main input channel count";
        return result;
    }

    if (main_output_bus.default_channels > 0 && output_stem == StemKind::none) {
        result.error = "AAX does not support the requested main output channel count";
        return result;
    }

    if (definition.descriptor.category == PluginCategory::Effect
        && main_input_channels == 0
        && main_output_bus.default_channels > 0)
    {
        result.error = "AAX effects require an audio input bus";
        return result;
    }

    if (definition.descriptor.category == PluginCategory::Instrument
        && !definition.descriptor.accepts_midi)
    {
        result.error = "AAX instruments must declare MIDI input support";
        return result;
    }

    if (definition.descriptor.category == PluginCategory::MidiEffect
        && !definition.descriptor.accepts_midi)
    {
        result.error = "AAX MIDI effects must declare MIDI input support";
        return result;
    }

    pulp::state::StateStore store;
    processor->set_state_store(&store);
    processor->define_parameters(store);

    for (const auto& param : store.all_params()) {
        ParameterBinding binding;
        binding.id = param.id;
        binding.aax_id = parameter_id_string(param.id);
        binding.name = truncate_copy(param.name.empty() ? binding.aax_id : param.name, 31);
        binding.unit = param.unit;
        binding.range = param.range;
        binding.to_string = param.to_string;
        binding.from_string = param.from_string;
        binding.discrete = param.range.step > 0.0f;

        if (binding.discrete) {
            const auto span = param.range.max - param.range.min;
            const auto steps = std::max(0.0f, std::round(span / param.range.step));
            binding.step_count = static_cast<uint32_t>(steps) + 1u;
        }

        definition.parameters.push_back(std::move(binding));
    }

    ComponentLayout component;
    component.input_stem = input_stem;
    component.output_stem = output_stem;
    component.main_input_channels = main_input_channels;
    component.main_output_channels = main_output_bus.default_channels;
    component.sidechain_channels = sidechain_channels;
    component.native_plugin_id = derive_native_plugin_id(codes.native_id_base, 0);
    component.short_name = truncate_copy(definition.descriptor.name, 31);
    component.description = component_label(definition.descriptor, input_stem, output_stem, sidechain_channels);
    definition.components.push_back(std::move(component));
    definition.packet_float_count = definition.parameters.size() + 1u;

    result.ok = true;
    result.definition = std::move(definition);
    return result;
}

} // namespace pulp::format::aax

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/aax_model.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

class StereoEffect final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "StereoEffect",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.stereo-effect",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2, false}},
            .output_buses = {{"Main Out", 2, false}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-24.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = 2,
            .name = "Mode",
            .unit = "",
            .range = {0.0f, 3.0f, 0.0f, 1.0f},
            .kind = pulp::state::ParamKind::Enum,
            .value_labels = {"A", "B", "C", "D"},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>&,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {}
};

class StereoWithSidechain final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "StereoSC",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.stereo-sidechain",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2, false}, {"Sidechain", 1, true}},
            .output_buses = {{"Main Out", 2, false}},
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>&,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {}
};

class StereoInstrument final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "Tone",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.tone",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2, false}},
            .accepts_midi = true,
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>&,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {}
};

class InvalidSidechain final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "InvalidSC",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.invalid-sidechain",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2, false}, {"Wide Sidechain", 2, true}},
            .output_buses = {{"Main Out", 2, false}},
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>&,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {}
};

std::unique_ptr<pulp::format::Processor> make_stereo_effect() {
    return std::make_unique<StereoEffect>();
}

std::unique_ptr<pulp::format::Processor> make_stereo_sidechain() {
    return std::make_unique<StereoWithSidechain>();
}

std::unique_ptr<pulp::format::Processor> make_stereo_instrument() {
    return std::make_unique<StereoInstrument>();
}

std::unique_ptr<pulp::format::Processor> make_invalid_sidechain() {
    return std::make_unique<InvalidSidechain>();
}

class ConfigurableProcessor final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return descriptor_;
    }

    void define_parameters(pulp::state::StateStore& store) override {
        for (const auto& param : parameters_) {
            store.add_parameter(param);
        }
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>&,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {}

    int latency_samples() const override {
        return latency_samples_;
    }

    static void configure(pulp::format::PluginDescriptor descriptor,
                          std::vector<pulp::state::ParamInfo> parameters = {},
                          int latency_samples = 0)
    {
        descriptor_ = std::move(descriptor);
        parameters_ = std::move(parameters);
        latency_samples_ = latency_samples;
    }

private:
    static inline pulp::format::PluginDescriptor descriptor_{};
    static inline std::vector<pulp::state::ParamInfo> parameters_{};
    static inline int latency_samples_ = 0;
};

std::unique_ptr<pulp::format::Processor> make_configured_processor() {
    return std::make_unique<ConfigurableProcessor>();
}

std::unique_ptr<pulp::format::Processor> make_null_processor() {
    return nullptr;
}

pulp::format::aax::PluginCodes valid_codes() {
    return {
        .manufacturer_id = pulp::format::aax::fourcc("Pulp"),
        .product_id = pulp::format::aax::fourcc("Test"),
        .native_id_base = pulp::format::aax::fourcc("PTst"),
    };
}

pulp::format::PluginDescriptor descriptor_with_buses(
    std::vector<pulp::format::BusInfo> inputs,
    std::vector<pulp::format::BusInfo> outputs,
    pulp::format::PluginCategory category = pulp::format::PluginCategory::Effect,
    bool accepts_midi = false,
    bool produces_midi = false)
{
    return {
        .name = "EdgeCase",
        .manufacturer = "Pulp",
        .bundle_id = "com.pulp.edge-case",
        .version = "1.0.0",
        .category = category,
        .input_buses = std::move(inputs),
        .output_buses = std::move(outputs),
        .accepts_midi = accepts_midi,
        .produces_midi = produces_midi,
    };
}

} // namespace

TEST_CASE("AAX model builds a stable parameter packet for stereo effects", "[aax][model]") {
    pulp::format::aax::PluginCodes codes{
        .manufacturer_id = pulp::format::aax::fourcc("Pulp"),
        .product_id = pulp::format::aax::fourcc("Gain"),
        .native_id_base = pulp::format::aax::fourcc("PGan"),
    };

    auto result = pulp::format::aax::build_plugin_definition(make_stereo_effect, codes);
    REQUIRE(result.ok);
    REQUIRE(result.definition.parameters.size() == 2);
    REQUIRE(result.definition.packet_float_count == 3);
    REQUIRE(result.definition.components.size() == 1);
    REQUIRE(result.definition.components[0].main_input_channels == 2);
    REQUIRE(result.definition.components[0].main_output_channels == 2);
    REQUIRE(result.definition.components[0].input_stem == pulp::format::aax::StemKind::stereo);
    REQUIRE(result.definition.components[0].output_stem == pulp::format::aax::StemKind::stereo);
    REQUIRE(result.definition.parameters[0].aax_id == "p00000001");
    REQUIRE(result.definition.parameters[1].discrete);
    REQUIRE(result.definition.parameters[1].step_count == 4);
}

TEST_CASE("AAX model preserves mono sidechain layouts", "[aax][model]") {
    pulp::format::aax::PluginCodes codes{
        .manufacturer_id = pulp::format::aax::fourcc("Pulp"),
        .product_id = pulp::format::aax::fourcc("Comp"),
        .native_id_base = pulp::format::aax::fourcc("PCmp"),
    };

    auto result = pulp::format::aax::build_plugin_definition(make_stereo_sidechain, codes);
    REQUIRE(result.ok);
    REQUIRE(result.definition.supports_sidechain);
    REQUIRE(result.definition.components[0].sidechain_channels == 1);
}

TEST_CASE("AAX model emits one component per declared bus layout", "[aax][model]") {
    auto descriptor = descriptor_with_buses({{"Input", 2}}, {{"Output", 2}});
    descriptor.supported_bus_layouts = {
        {.inputs = {1}, .outputs = {1}, .name = "Mono"},
        {.inputs = {2}, .outputs = {2}, .name = "Stereo"},
    };
    ConfigurableProcessor::configure(std::move(descriptor));
    pulp::format::aax::PluginCodes codes{
        .manufacturer_id = pulp::format::aax::fourcc("Pulp"),
        .product_id = pulp::format::aax::fourcc("Lays"),
        .native_id_base = pulp::format::aax::fourcc("PLay"),
    };

    const auto result = pulp::format::aax::build_plugin_definition(
        make_configured_processor, codes);
    REQUIRE(result.ok);
    REQUIRE(result.definition.components.size() == 2);
    REQUIRE(result.definition.components[0].main_input_channels == 1);
    REQUIRE(result.definition.components[1].main_input_channels == 2);
    REQUIRE(result.definition.components[0].native_plugin_id !=
            result.definition.components[1].native_plugin_id);
}

TEST_CASE("AAX model allows MIDI instruments with no audio input bus", "[aax][model]") {
    pulp::format::aax::PluginCodes codes{
        .manufacturer_id = pulp::format::aax::fourcc("Pulp"),
        .product_id = pulp::format::aax::fourcc("Tone"),
        .native_id_base = pulp::format::aax::fourcc("PTon"),
    };

    auto result = pulp::format::aax::build_plugin_definition(make_stereo_instrument, codes);
    REQUIRE(result.ok);
    REQUIRE(result.definition.supports_midi_input);
    REQUIRE(result.definition.components[0].input_stem == pulp::format::aax::StemKind::none);
    REQUIRE(result.definition.components[0].output_stem == pulp::format::aax::StemKind::stereo);
}

TEST_CASE("AAX model rejects unsupported sidechain layouts", "[aax][model]") {
    pulp::format::aax::PluginCodes codes{
        .manufacturer_id = pulp::format::aax::fourcc("Pulp"),
        .product_id = pulp::format::aax::fourcc("Bad!"),
        .native_id_base = pulp::format::aax::fourcc("PBad"),
    };

    auto result = pulp::format::aax::build_plugin_definition(make_invalid_sidechain, codes);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("mono optional bus") != std::string::npos);
}

TEST_CASE("AAX model rejects missing factories and plugin identifiers", "[aax][model]") {
    auto codes = valid_codes();

    auto null_factory = pulp::format::aax::build_plugin_definition(nullptr, codes);
    REQUIRE_FALSE(null_factory.ok);
    REQUIRE(null_factory.error.find("factory function is null") != std::string::npos);

    auto null_processor = pulp::format::aax::build_plugin_definition(make_null_processor, codes);
    REQUIRE_FALSE(null_processor.ok);
    REQUIRE(null_processor.error.find("did not return a processor") != std::string::npos);

    auto zero_codes = pulp::format::aax::build_plugin_definition(make_stereo_effect, {});
    REQUIRE_FALSE(zero_codes.ok);
    REQUIRE(zero_codes.error.find("non-zero") != std::string::npos);
}

TEST_CASE("AAX model rejects invalid output bus declarations", "[aax][model]") {
    auto codes = valid_codes();

    ConfigurableProcessor::configure(descriptor_with_buses({{"Main In", 2, false}}, {}));
    auto no_output = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(no_output.ok);
    REQUIRE(no_output.error.find("at least one declared output bus") != std::string::npos);

    ConfigurableProcessor::configure(
        descriptor_with_buses({{"Main In", 2, false}}, {{"Main Out", 2, false}, {"Aux Out", 2, true}}));
    auto multiple_outputs = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(multiple_outputs.ok);
    REQUIRE(multiple_outputs.error.find("exactly one output bus") != std::string::npos);

    ConfigurableProcessor::configure(
        descriptor_with_buses({{"Main In", 2, false}}, {{"Main Out", 2, true}}));
    auto optional_output = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(optional_output.ok);
    REQUIRE(optional_output.error.find("optional main output bus") != std::string::npos);
}

TEST_CASE("AAX model rejects invalid input and sidechain bus declarations", "[aax][model]") {
    auto codes = valid_codes();

    ConfigurableProcessor::configure(
        descriptor_with_buses({{"Main In", 2, true}}, {{"Main Out", 2, false}}));
    auto optional_input = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(optional_input.ok);
    REQUIRE(optional_input.error.find("optional main input bus") != std::string::npos);

    ConfigurableProcessor::configure(descriptor_with_buses(
        {{"Main In", 2, false}, {"Sidechain", 1, true}, {"Aux", 1, true}},
        {{"Main Out", 2, false}}));
    auto too_many_inputs = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(too_many_inputs.ok);
    REQUIRE(too_many_inputs.error.find("one main input bus and one optional mono sidechain") != std::string::npos);

    ConfigurableProcessor::configure(descriptor_with_buses(
        {{"Main In", 2, false}, {"Sidechain", 1, false}},
        {{"Main Out", 2, false}}));
    auto required_sidechain = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(required_sidechain.ok);
    REQUIRE(required_sidechain.error.find("secondary input bus must be optional") != std::string::npos);
}

TEST_CASE("AAX model rejects unsupported channel counts", "[aax][model]") {
    auto codes = valid_codes();

    ConfigurableProcessor::configure(
        descriptor_with_buses({{"Main In", 9, false}}, {{"Main Out", 2, false}}));
    auto unsupported_input = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(unsupported_input.ok);
    REQUIRE(unsupported_input.error.find("main input channel count") != std::string::npos);

    ConfigurableProcessor::configure(
        descriptor_with_buses({{"Main In", 2, false}}, {{"Main Out", 9, false}}));
    auto unsupported_output = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(unsupported_output.ok);
    REQUIRE(unsupported_output.error.find("main output channel count") != std::string::npos);
}

TEST_CASE("AAX model rejects category layouts that cannot run in AAX", "[aax][model]") {
    auto codes = valid_codes();

    ConfigurableProcessor::configure(descriptor_with_buses({}, {{"Main Out", 2, false}}));
    auto effect_without_input = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(effect_without_input.ok);
    REQUIRE(effect_without_input.error.find("effects require an audio input bus") != std::string::npos);

    ConfigurableProcessor::configure(descriptor_with_buses(
        {},
        {{"Main Out", 2, false}},
        pulp::format::PluginCategory::Instrument,
        false));
    auto instrument_without_midi = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(instrument_without_midi.ok);
    REQUIRE(instrument_without_midi.error.find("instruments must declare MIDI input") != std::string::npos);

    ConfigurableProcessor::configure(descriptor_with_buses(
        {{"Main In", 0, false}},
        {{"Main Out", 0, false}},
        pulp::format::PluginCategory::MidiEffect,
        false,
        true));
    auto midi_effect_without_midi = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE_FALSE(midi_effect_without_midi.ok);
    REQUIRE(midi_effect_without_midi.error.find("MIDI effects must declare MIDI input") != std::string::npos);
}

TEST_CASE("AAX model validates fourcc and native plugin id derivation helpers", "[aax][model]") {
    REQUIRE(pulp::format::aax::fourcc("") == 0);
    REQUIRE(pulp::format::aax::fourcc("abc") == 0);
    REQUIRE(pulp::format::aax::fourcc("abcde") == 0);
    REQUIRE(pulp::format::aax::fourcc_string(pulp::format::aax::fourcc("Pulp")) == "Pulp");
    REQUIRE(pulp::format::aax::parameter_id_string(0) == "p00000000");
    REQUIRE(pulp::format::aax::parameter_id_string(42) == "p0000002A");
    REQUIRE(pulp::format::aax::parameter_id_string(0xffffffffu) == "pFFFFFFFF");

    const auto base = pulp::format::aax::fourcc("ABCD");
    REQUIRE(pulp::format::aax::derive_native_plugin_id(base, 0) == base);
    REQUIRE(pulp::format::aax::fourcc_string(pulp::format::aax::derive_native_plugin_id(base, 1)) == "ABC1");
    REQUIRE(pulp::format::aax::fourcc_string(pulp::format::aax::derive_native_plugin_id(base, 10)) == "ABCA");
    REQUIRE(pulp::format::aax::fourcc_string(pulp::format::aax::derive_native_plugin_id(base, 35)) == "ABCZ");
    REQUIRE(pulp::format::aax::fourcc_string(pulp::format::aax::derive_native_plugin_id(base, 36)) == "ABCa");
    REQUIRE(pulp::format::aax::fourcc_string(pulp::format::aax::derive_native_plugin_id(base, 61)) == "ABCz");

    const auto hashed = pulp::format::aax::derive_native_plugin_id(base, 62);
    REQUIRE(hashed == (base ^ static_cast<uint32_t>(62u * 0x45d9f3bu)));
}

TEST_CASE("AAX model exposes stem mapping helpers for supported and unknown layouts", "[aax][model]") {
    REQUIRE(pulp::format::aax::stem_kind_for_channels(0) == pulp::format::aax::StemKind::none);
    REQUIRE(pulp::format::aax::stem_kind_for_channels(1) == pulp::format::aax::StemKind::mono);
    REQUIRE(pulp::format::aax::stem_kind_for_channels(2) == pulp::format::aax::StemKind::stereo);
    REQUIRE(pulp::format::aax::stem_kind_for_channels(3) == pulp::format::aax::StemKind::lcr);
    REQUIRE(pulp::format::aax::stem_kind_for_channels(4) == pulp::format::aax::StemKind::quad);
    REQUIRE(pulp::format::aax::stem_kind_for_channels(5) == pulp::format::aax::StemKind::surround_5_0);
    REQUIRE(pulp::format::aax::stem_kind_for_channels(6) == pulp::format::aax::StemKind::surround_5_1);
    REQUIRE(pulp::format::aax::stem_kind_for_channels(7) == pulp::format::aax::StemKind::surround_6_1);
    REQUIRE(pulp::format::aax::stem_kind_for_channels(8) == pulp::format::aax::StemKind::surround_7_1);
    REQUIRE(pulp::format::aax::stem_kind_for_channels(9) == pulp::format::aax::StemKind::none);

    REQUIRE(pulp::format::aax::stem_channel_count(pulp::format::aax::StemKind::stereo) == 2);
    REQUIRE(pulp::format::aax::stem_channel_count(pulp::format::aax::StemKind::surround_7_1) == 8);
    REQUIRE(std::string(pulp::format::aax::stem_signature(pulp::format::aax::StemKind::surround_5_1)) == "5.1");
    REQUIRE(std::string(pulp::format::aax::stem_signature(pulp::format::aax::StemKind::surround_7_1)) == "7.1");
    REQUIRE(pulp::format::aax::stem_channel_count(pulp::format::aax::StemKind::lcrs) == 0);
    REQUIRE(std::string(pulp::format::aax::stem_signature(pulp::format::aax::StemKind::lcrs)) == "unknown");
    REQUIRE(pulp::format::aax::stem_channel_count(pulp::format::aax::StemKind::surround_6_0) == 0);
    REQUIRE(std::string(pulp::format::aax::stem_signature(pulp::format::aax::StemKind::surround_6_0)) == "unknown");
}

TEST_CASE("AAX model carries descriptor and parameter metadata into definitions", "[aax][model][issue-648]") {
    auto codes = valid_codes();
    auto descriptor = descriptor_with_buses(
        {{"Main In", 2, false}},
        {{"Main Out", 2, false}},
        pulp::format::PluginCategory::Effect,
        true,
        true);
    descriptor.name = "MetadataEffect";

    ConfigurableProcessor::configure(
        std::move(descriptor),
        {
            {
                .id = 7,
                .name = "Blend",
                .unit = "%",
                .range = {0.0f, 100.0f, 25.0f, 0.0f},
                .to_string = [](float value) { return std::to_string(static_cast<int>(value)) + "%"; },
                .from_string = [](const std::string& text) { return std::stof(text); },
            },
            {
                .id = 8,
                .name = "Shape",
                .unit = "",
                .range = {0.0f, 1.0f, 0.0f, 0.25f},
                .kind = pulp::state::ParamKind::Enum,
                .value_labels = {"0", "1", "2", "3", "4"},
            },
        },
        128);

    auto result = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE(result.ok);
    REQUIRE(result.definition.codes.manufacturer_id == codes.manufacturer_id);
    REQUIRE(result.definition.supports_midi_input);
    REQUIRE(result.definition.supports_midi_output);
    REQUIRE(result.definition.uses_transport);
    REQUIRE(result.definition.latency_samples == 128);
    REQUIRE(result.definition.packet_float_count == 3);
    REQUIRE(result.definition.parameters.size() == 2);
    REQUIRE(result.definition.parameters[0].id == 7);
    REQUIRE(result.definition.parameters[0].aax_id == "p00000007");
    REQUIRE(result.definition.parameters[0].name == "Blend");
    REQUIRE(result.definition.parameters[0].unit == "%");
    REQUIRE(result.definition.parameters[0].range.default_value == 25.0f);
    REQUIRE(result.definition.parameters[0].to_string(42.0f) == "42%");
    REQUIRE(result.definition.parameters[0].from_string("64") == 64.0f);
    REQUIRE_FALSE(result.definition.parameters[0].discrete);
    REQUIRE(result.definition.parameters[1].discrete);
    REQUIRE(result.definition.parameters[1].step_count == 5);
    REQUIRE(result.definition.components.size() == 1);
    REQUIRE(result.definition.components[0].native_plugin_id == codes.native_id_base);
    REQUIRE(result.definition.components[0].description == "MetadataEffect stereo -> stereo");
}

TEST_CASE("AAX model truncates long labels and falls back for empty parameter names", "[aax][model]") {
    auto codes = valid_codes();
    auto descriptor = descriptor_with_buses({{"Main In", 2, false}}, {{"Main Out", 2, false}});
    descriptor.name = "ThisPluginNameIsDefinitelyLongerThanThirtyOneCharacters";

    ConfigurableProcessor::configure(
        std::move(descriptor),
        {
            {
                .id = 0x1234,
                .name = "ThisParameterNameIsDefinitelyLongerThanThirtyOneCharacters",
                .unit = "Hz",
                .range = {0.0f, 1.0f, 0.5f, 0.0f},
            },
            {
                .id = 0xBEEF,
                .name = "",
                .unit = "",
                .range = {0.0f, 10.0f, 1.0f, 0.0f},
            },
        });

    auto result = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE(result.ok);
    REQUIRE(result.definition.parameters.size() == 2);
    REQUIRE(result.definition.parameters[0].name.size() == 31);
    REQUIRE(result.definition.parameters[0].name == "ThisParameterNameIsDefinitelyLo");
    REQUIRE(result.definition.parameters[1].name == "p0000BEEF");
    REQUIRE(result.definition.components[0].short_name.size() == 31);
    REQUIRE(result.definition.components[0].short_name == "ThisPluginNameIsDefinitelyLonge");
}

// AAX audit topic: parameter taper mapping. A binding must preserve whether a
// parameter is linear, log/skewed, or discrete/enum so the host plays back the
// correct automation curve and step count. Previously only linear + a single
// discrete step_count were asserted; a dropped skew or wrong step_count would
// pass silently.
TEST_CASE("AAX model preserves linear, log, and enum parameter tapers", "[aax][model]") {
    auto codes = valid_codes();
    auto descriptor = descriptor_with_buses({{"Main In", 2, false}}, {{"Main Out", 2, false}});
    descriptor.name = "TaperEffect";

    ConfigurableProcessor::configure(
        std::move(descriptor),
        {
            // Linear gain 0..1.
            {.id = 1, .name = "Gain", .unit = "", .range = {0.0f, 1.0f, 0.5f, 0.0f}},
            // Log/skewed frequency 20..20k, 1 kHz at the normalized midpoint.
            {.id = 2, .name = "Freq", .unit = "Hz",
             .range = pulp::state::ParamRange::with_center(20.0f, 20000.0f, 1000.0f, 1000.0f)},
            // Enum/discrete: 3 explicitly declared positions (0,1,2).
            {.id = 3, .name = "Mode", .unit = "", .range = {0.0f, 2.0f, 0.0f, 1.0f},
             .kind = pulp::state::ParamKind::Enum,
             .value_labels = {"A", "B", "C"}},
        });

    auto result = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE(result.ok);
    REQUIRE(result.definition.parameters.size() == 3);

    const auto& gain = result.definition.parameters[0];
    const auto& freq = result.definition.parameters[1];
    const auto& mode = result.definition.parameters[2];

    // Linear taper: skew == 1, continuous.
    REQUIRE(gain.range.is_linear());
    REQUIRE(gain.range.skew == 1.0f);
    REQUIRE_FALSE(gain.discrete);

    // Log taper survives into the binding: non-linear skew, and the normalized
    // midpoint still maps back near the chosen 1 kHz center.
    REQUIRE_FALSE(freq.range.is_linear());
    REQUIRE(freq.range.skew != 1.0f);
    REQUIRE_FALSE(freq.discrete);
    const float mid = freq.range.denormalize(0.5f);
    REQUIRE(mid > 999.0f);
    REQUIRE(mid < 1001.0f);

    // Enum/discrete taper: 3 steps (0,1,2).
    REQUIRE(mode.discrete);
    REQUIRE(mode.step_count == 3u);
}

// AAX audit topic: master bypass. AAX reserves packet slot 0 for the host
// master-bypass control, so the parameter packet is always plugin-params + 1 —
// even for a plugin with no automatable parameters. A regression that stopped
// reserving the slot would misalign every parameter index in the packet.
TEST_CASE("AAX model reserves a master-bypass packet slot regardless of parameter count", "[aax][model]") {
    auto codes = valid_codes();

    // Zero parameters: the packet still has the reserved bypass slot.
    ConfigurableProcessor::configure(
        descriptor_with_buses({{"Main In", 2, false}}, {{"Main Out", 2, false}}));
    auto none = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE(none.ok);
    REQUIRE(none.definition.parameters.empty());
    REQUIRE(none.definition.packet_float_count == 1u);

    // Three parameters: bypass slot + three params.
    ConfigurableProcessor::configure(
        descriptor_with_buses({{"Main In", 2, false}}, {{"Main Out", 2, false}}),
        {
            {.id = 1, .name = "A", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}},
            {.id = 2, .name = "B", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}},
            {.id = 3, .name = "C", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}},
        });
    auto three = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE(three.ok);
    REQUIRE(three.definition.parameters.size() == 3);
    REQUIRE(three.definition.packet_float_count == 4u);
}

// AAX audit topic: parameter IDs. Bindings must keep declaration order and give
// each parameter a stable, unique AAX id derived from its ParamID — hosts key
// automation off these strings, so a reordering or collision corrupts sessions.
TEST_CASE("AAX model assigns stable unique ids in declaration order", "[aax][model]") {
    auto codes = valid_codes();
    ConfigurableProcessor::configure(
        descriptor_with_buses({{"Main In", 2, false}}, {{"Main Out", 2, false}}),
        {
            {.id = 0x10, .name = "First",  .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}},
            {.id = 0x02, .name = "Second", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}},
            {.id = 0xAB, .name = "Third",  .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}},
        });
    auto result = pulp::format::aax::build_plugin_definition(make_configured_processor, codes);
    REQUIRE(result.ok);
    const auto& p = result.definition.parameters;
    REQUIRE(p.size() == 3);
    // Declaration order preserved (not sorted by id).
    REQUIRE(p[0].name == "First");
    REQUIRE(p[1].name == "Second");
    REQUIRE(p[2].name == "Third");
    // aax_id == parameter_id_string(id), and ids are unique.
    REQUIRE(p[0].aax_id == pulp::format::aax::parameter_id_string(0x10));
    REQUIRE(p[1].aax_id == pulp::format::aax::parameter_id_string(0x02));
    REQUIRE(p[2].aax_id == pulp::format::aax::parameter_id_string(0xAB));
    REQUIRE(p[0].aax_id != p[1].aax_id);
    REQUIRE(p[1].aax_id != p[2].aax_id);
    REQUIRE(p[0].aax_id != p[2].aax_id);
}

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/aax_model.hpp>

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

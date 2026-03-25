// Test: Verify the PULP_CLAP_PLUGIN() macro generates a valid CLAP entry
// This doesn't create a real .clap bundle — it just validates the generated
// symbols and factory can be called programmatically.

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/clap_entry.hpp>
#include <cmath>

// Minimal test processor
namespace test_clap {

class TestProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "TestClap",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.clap",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .default_input_channels = 2,
            .default_output_channels = 2,
        };
    }
    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = 1, .name = "Gain", .unit = "dB",
                             .range = {-60.0f, 24.0f, 0.0f, 0.1f}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels(); ++ch) {
            auto ic = in.channel(ch);
            auto oc = out.channel(ch);
            for (std::size_t i = 0; i < out.num_samples(); ++i) oc[i] = ic[i];
        }
    }
};

inline std::unique_ptr<pulp::format::Processor> create_test() {
    return std::make_unique<TestProcessor>();
}

} // namespace test_clap

// Generate the CLAP entry
PULP_CLAP_PLUGIN(test_clap::create_test)

TEST_CASE("PULP_CLAP_PLUGIN generates valid entry", "[clap][entry]") {
    REQUIRE(clap_entry.init != nullptr);
    REQUIRE(clap_entry.get_factory != nullptr);

    // Initialize
    REQUIRE(clap_entry.init("test"));

    // Get factory
    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    REQUIRE(factory->get_plugin_count(factory) == 1);

    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);
    REQUIRE(std::string(desc->name) == "TestClap");
    REQUIRE(std::string(desc->id) == "com.pulp.test.clap");
    REQUIRE(std::string(desc->vendor) == "PulpTest");

    clap_entry.deinit();
}

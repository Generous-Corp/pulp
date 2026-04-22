// Test: Verify the PULP_CLAP_PLUGIN() macro generates a valid CLAP entry
// This doesn't create a real .clap bundle — it just validates the generated
// symbols and factory can be called programmatically.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/clap_entry.hpp>
#include <cmath>
#include <cstring>
#include <vector>

// Minimal test processor
namespace test_clap {

class TestProcessor;
inline TestProcessor* g_last_processor = nullptr;

class TestProcessor : public pulp::format::Processor {
public:
    TestProcessor() { g_last_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "TestClap",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.clap",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
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

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::string plugin_state;
};

inline std::unique_ptr<pulp::format::Processor> create_test() {
    return std::make_unique<TestProcessor>();
}

} // namespace test_clap

namespace {

using Catch::Matchers::WithinAbs;

struct MemoryStream {
    std::vector<uint8_t> bytes;
    std::size_t read_offset = 0;
};

int64_t stream_write(const clap_ostream_t* stream, const void* buffer, uint64_t size) {
    auto* sink = static_cast<MemoryStream*>(stream->ctx);
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    sink->bytes.insert(sink->bytes.end(), bytes, bytes + size);
    return static_cast<int64_t>(size);
}

int64_t stream_read(const clap_istream_t* stream, void* buffer, uint64_t size) {
    auto* source = static_cast<MemoryStream*>(stream->ctx);
    const auto remaining = source->bytes.size() - source->read_offset;
    const auto to_copy = remaining < size ? remaining : static_cast<std::size_t>(size);
    if (to_copy == 0) return 0;
    std::memcpy(buffer, source->bytes.data() + source->read_offset, to_copy);
    source->read_offset += to_copy;
    return static_cast<int64_t>(to_copy);
}

} // namespace

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

TEST_CASE("CLAP state extension round-trips plugin-owned payload", "[clap][entry][state]") {
    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);

    const clap_plugin_t* plugin1 = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin1 != nullptr);
    REQUIRE(plugin1->init(plugin1));
    auto* proc1 = test_clap::g_last_processor;
    REQUIRE(proc1 != nullptr);
    proc1->state().set_value(1, -12.5f);
    proc1->plugin_state = "snapshots=A|B";

    auto* state1 = static_cast<const clap_plugin_state_t*>(
        plugin1->get_extension(plugin1, CLAP_EXT_STATE));
    REQUIRE(state1 != nullptr);

    MemoryStream sink;
    clap_ostream_t out_stream{.ctx = &sink, .write = stream_write};
    REQUIRE(state1->save(plugin1, &out_stream));

    const clap_plugin_t* plugin2 = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(plugin2 != nullptr);
    REQUIRE(plugin2->init(plugin2));
    auto* proc2 = test_clap::g_last_processor;
    REQUIRE(proc2 != nullptr);
    proc2->state().set_value(1, 6.0f);
    proc2->plugin_state = "stale";

    auto* state2 = static_cast<const clap_plugin_state_t*>(
        plugin2->get_extension(plugin2, CLAP_EXT_STATE));
    REQUIRE(state2 != nullptr);

    MemoryStream source{.bytes = sink.bytes};
    clap_istream_t in_stream{.ctx = &source, .read = stream_read};
    REQUIRE(state2->load(plugin2, &in_stream));
    REQUIRE_THAT(proc2->state().get_value(1), WithinAbs(-12.5, 0.01));
    REQUIRE(proc2->plugin_state == "snapshots=A|B");

    plugin1->destroy(plugin1);
    plugin2->destroy(plugin2);
    clap_entry.deinit();
}

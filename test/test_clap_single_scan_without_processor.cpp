// The single-plugin macro owns clap_entry, so exercise it in its own binary.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_entry.hpp>

#include <array>
#include <memory>
#include <string_view>

namespace {

int g_built = 0;
int g_processed = 0;

pulp::format::PluginDescriptor scan_descriptor() {
    return {
        .name = "SingleScan",
        .manufacturer = "PulpTest",
        .bundle_id = "com.pulp.test.scan.single",
        .version = "2.3.4",
        .category = pulp::format::PluginCategory::Effect,
        .input_buses = {{"In", 2}},
        .output_buses = {{"Out", 2}},
    };
}

class ScanProcessor : public pulp::format::Processor {
  public:
    ScanProcessor() {
        ++g_built;
    }
    pulp::format::PluginDescriptor descriptor() const override {
        return scan_descriptor();
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        ++g_processed;
        for (std::size_t c = 0; c < out.num_channels(); ++c)
            for (std::size_t n = 0; n < out.num_samples(); ++n)
                out.channel_ptr(c)[n] = in.channel_ptr(c)[n] * 0.5f;
    }
    void process(pulp::format::ProcessBuffers& audio, pulp::midi::MidiBuffer& mi,
                 pulp::midi::MidiBuffer& mo, const pulp::format::ProcessContext& ctx) override {
        if (auto* out = audio.main_output()) {
            if (auto* in = audio.main_input())
                process(*out, *in, mi, mo, ctx);
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_processor() {
    return std::make_unique<ScanProcessor>();
}

} // namespace

PULP_CLAP_PLUGIN_WITH_DESCRIPTOR(create_processor, scan_descriptor)

TEST_CASE("CLAP single-plugin static descriptor defers construction until instance init",
          "[clap][entry][scan][single]") {
    // Do not reset the counter: that would hide construction during static init.
    REQUIRE(g_built == 0);
    REQUIRE(clap_entry.init("test"));
    REQUIRE(g_built == 0);
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    REQUIRE(factory->get_plugin_count(factory) == 1);
    const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(descriptor != nullptr);
    CHECK(std::string_view(descriptor->id) == "com.pulp.test.scan.single");
    CHECK(std::string_view(descriptor->name) == "SingleScan");
    CHECK(std::string_view(descriptor->vendor) == "PulpTest");
    CHECK(std::string_view(descriptor->version) == "2.3.4");
    REQUIRE(descriptor->features != nullptr);
    REQUIRE(descriptor->features[0] != nullptr);
    CHECK(std::string_view(descriptor->features[0]) == CLAP_PLUGIN_FEATURE_AUDIO_EFFECT);
    CHECK(descriptor->features[1] == nullptr);
    CHECK(factory->get_plugin_descriptor(factory, 1) == nullptr);
    CHECK(factory->get_plugin_descriptor(factory, 0) == descriptor);
    REQUIRE(clap_entry.init("test"));
    REQUIRE(g_built == 0);

    const auto* plugin = factory->create_plugin(factory, nullptr, descriptor->id);
    REQUIRE(plugin != nullptr);
    const auto destroy = [](const clap_plugin_t* p) { p->destroy(p); };
    std::unique_ptr<const clap_plugin_t, decltype(destroy)> instance(plugin, destroy);
    REQUIRE(g_built == 0);
    REQUIRE(plugin->init(plugin));
    REQUIRE(g_built == 1);
    REQUIRE(plugin->activate(plugin, 48000.0, 16, 16));
    REQUIRE(plugin->start_processing(plugin));

    std::array<float, 16> in_l{}, in_r{}, out_l{}, out_r{};
    in_l.fill(1.0f);
    in_r.fill(-0.5f);
    float* in_channels[] = {in_l.data(), in_r.data()};
    float* out_channels[] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t in_buffer{in_channels, nullptr, 2, 0, 0};
    clap_audio_buffer_t out_buffer{out_channels, nullptr, 2, 0, 0};
    clap_input_events_t in_events{
        nullptr,
        [](const clap_input_events_t*) -> uint32_t { return 0; },
        [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; },
    };
    clap_output_events_t out_events{
        nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
    clap_process_t process{};
    process.frames_count = 16;
    process.audio_inputs = &in_buffer;
    process.audio_inputs_count = 1;
    process.audio_outputs = &out_buffer;
    process.audio_outputs_count = 1;
    process.in_events = &in_events;
    process.out_events = &out_events;
    CHECK(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR);
    CHECK(g_processed == 1);
    for (std::size_t n = 0; n < out_l.size(); ++n) {
        CHECK(out_l[n] == 0.5f);
        CHECK(out_r[n] == -0.25f);
    }
    CHECK(g_built == 1);
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    // Destroy the instance before deinitializing its module.
    instance.reset();
    clap_entry.deinit();
}

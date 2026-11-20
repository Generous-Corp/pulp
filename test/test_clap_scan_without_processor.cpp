// A host scan (module init, plug-in count, descriptor reads) must not build a
// Processor when the plug-in supplies a static descriptor, must answer
// byte-identical descriptors either way, and must still create and run the
// plug-in afterwards. A plug-in without a static descriptor keeps building one.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_entry.hpp>
#include <pulp/format/processor.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_static_built = 0;
int g_dynamic_built = 0;

pulp::format::PluginDescriptor make_descriptor(const char* name, const char* id) {
    return {
        .name = name,
        .manufacturer = "PulpTest",
        .bundle_id = id,
        .version = "2.3.4",
        .category = pulp::format::PluginCategory::Effect,
        .input_buses = {{"In", 2}},
        .output_buses = {{"Out", 2}},
    };
}

pulp::format::PluginDescriptor static_desc() {
    return make_descriptor("ScanSame", "com.pulp.test.scan.static");
}
pulp::format::PluginDescriptor dynamic_desc() {
    return make_descriptor("ScanSame", "com.pulp.test.scan.dynamic");
}

// Models a heavy constructor: counts builds and allocates.
template <int& Counter, pulp::format::PluginDescriptor (*Desc)()>
class CountingProcessor : public pulp::format::Processor {
  public:
    CountingProcessor() : ballast_(1 << 20) {
        ++Counter;
    }
    pulp::format::PluginDescriptor descriptor() const override {
        return Desc();
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        for (std::size_t c = 0; c < out.num_channels(); ++c)
            for (std::size_t n = 0; n < out.num_samples(); ++n)
                out.channel_ptr(c)[n] = in.channel_ptr(c)[n] * 0.5f;
    }
    void process(pulp::format::ProcessBuffers& audio, pulp::midi::MidiBuffer& mi,
                 pulp::midi::MidiBuffer& mo, const pulp::format::ProcessContext& ctx) override {
        if (auto* o = audio.main_output()) {
            if (auto* i = audio.main_input())
                process(*o, *i, mi, mo, ctx);
        }
    }

  private:
    std::vector<char> ballast_;
};

std::unique_ptr<pulp::format::Processor> create_static() {
    return std::make_unique<CountingProcessor<g_static_built, static_desc>>();
}
std::unique_ptr<pulp::format::Processor> create_dynamic() {
    return std::make_unique<CountingProcessor<g_dynamic_built, dynamic_desc>>();
}

} // namespace

PULP_CLAP_BUNDLE_PLUGIN_WITH_DESCRIPTOR(ScanStatic, create_static, static_desc)
PULP_CLAP_BUNDLE_PLUGIN(ScanDynamic, create_dynamic)
PULP_CLAP_BUNDLE_ENTRY()

TEST_CASE("CLAP scan builds no Processor when a static descriptor is given",
          "[clap][entry][scan]") {
    g_static_built = 0;
    g_dynamic_built = 0;

    REQUIRE(clap_entry.init("test"));
    auto* factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    REQUIRE(factory->get_plugin_count(factory) == 2);

    const clap_plugin_descriptor_t* ds = nullptr;
    const clap_plugin_descriptor_t* dd = nullptr;
    for (uint32_t i = 0; i < 2; ++i) {
        const auto* d = factory->get_plugin_descriptor(factory, i);
        REQUIRE(d != nullptr);
        if (std::strcmp(d->id, "com.pulp.test.scan.static") == 0)
            ds = d;
        else
            dd = d;
    }
    REQUIRE(ds != nullptr);
    REQUIRE(dd != nullptr);

    // The scan built nothing for the static plug-in; the control still builds.
    CHECK(g_static_built == 0);
    CHECK(g_dynamic_built == 1);

    // Descriptor answers match the Processor-derived ones field for field.
    CHECK(std::string(ds->name) == dd->name);
    CHECK(std::string(ds->vendor) == dd->vendor);
    CHECK(std::string(ds->version) == dd->version);
    CHECK(std::strcmp(ds->features[0], dd->features[0]) == 0);
    CHECK(ds->features[1] == nullptr);
    CHECK(dd->features[1] == nullptr);
    pulp::format::clap_generic::ClapPluginRecord* rs = nullptr;
    pulp::format::clap_generic::ClapPluginRecord* rd = nullptr;
    for (uint32_t i = 0; i < pulp::format::clap_generic::g_clap_record_count; ++i) {
        auto& r = pulp::format::clap_generic::g_clap_records[i];
        (r.static_descriptor ? rs : rd) = &r;
    }
    REQUIRE(rs != nullptr);
    REQUIRE(rd != nullptr);
    CHECK(rs->desc.name == rd->desc.name);
    CHECK(rs->desc.version == rd->desc.version);
    CHECK(rs->desc.input_buses.size() == rd->desc.input_buses.size());
    CHECK(rs->desc.output_buses.size() == rd->desc.output_buses.size());

    // create_plugin / init / activate / process still work and build on demand.
    const clap_plugin_t* p = factory->create_plugin(factory, nullptr, "com.pulp.test.scan.static");
    REQUIRE(p != nullptr);
    REQUIRE(p->init(p));
    REQUIRE(g_static_built == 1);
    REQUIRE(p->activate(p, 48000.0, 16, 256));
    REQUIRE(p->start_processing(p));

    std::vector<float> in_l(64, 1.0f), in_r(64, 1.0f), out_l(64, 0.0f), out_r(64, 0.0f);
    float* in_ch[2] = {in_l.data(), in_r.data()};
    float* out_ch[2] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t in_buf{in_ch, nullptr, 2, 0, 0};
    clap_audio_buffer_t out_buf{out_ch, nullptr, 2, 0, 0};
    struct Q {
        static uint32_t size(const clap_input_events_t*) {
            return 0;
        }
        static const clap_event_header_t* get(const clap_input_events_t*, uint32_t) {
            return nullptr;
        }
        static bool push(const clap_output_events_t*, const clap_event_header_t*) {
            return true;
        }
    };
    clap_input_events_t ie{nullptr, Q::size, Q::get};
    clap_output_events_t oe{nullptr, Q::push};
    clap_process_t pr{};
    pr.frames_count = 64;
    pr.audio_inputs = &in_buf;
    pr.audio_inputs_count = 1;
    pr.audio_outputs = &out_buf;
    pr.audio_outputs_count = 1;
    pr.in_events = &ie;
    pr.out_events = &oe;
    CHECK(p->process(p, &pr) != CLAP_PROCESS_ERROR);
    CHECK(out_l[10] == 0.5f);

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    clap_entry.deinit();
}

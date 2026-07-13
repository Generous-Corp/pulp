// CLAP multi-plugin-bundle entry test.
//
// Proves the deliverable a single-plugin binary cannot: two plugins exposed
// from ONE CLAP module entry via the bundle macros. It pins:
//   * N PULP_CLAP_BUNDLE_PLUGIN + one PULP_CLAP_BUNDLE_ENTRY compile and yield a
//     factory whose get_plugin_count() == 2, each descriptor distinct;
//   * create_plugin() resolves each plugin BY ID to its own factory/processor;
//   * descriptors are built in entry_init() (host module load), never at C++
//     static init — the single global that used to hold one descriptor is gone;
//   * the shared keyed registry carries both plugins (for cross-format lookup).

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_entry.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>

#include <cstring>
#include <memory>
#include <string>

namespace {

template <char Tag>
class ClapBundleProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = (Tag == 'A') ? "ClapBundleA" : "ClapBundleB",
            .manufacturer = "PulpTest",
            .bundle_id = (Tag == 'A') ? "com.pulp.test.clapbundle.a"
                                      : "com.pulp.test.clapbundle.b",
            .version = "1.0.0",
            .category = (Tag == 'A') ? pulp::format::PluginCategory::Effect
                                     : pulp::format::PluginCategory::Instrument,
            .input_buses = (Tag == 'A')
                ? std::vector<pulp::format::BusInfo>{{"Main In", 2}}
                : std::vector<pulp::format::BusInfo>{},
            .output_buses = {{"Main Out", 2}},
            .accepts_midi = (Tag == 'B'),  // instrument takes note input
        };
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            float* dst = out.channel_ptr(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) dst[n] = 0.0f;
        }
    }
    void process(pulp::format::ProcessBuffers& audio,
                 pulp::midi::MidiBuffer& mi, pulp::midi::MidiBuffer& mo,
                 const pulp::format::ProcessContext& ctx) override {
        if (auto* o = audio.main_output()) {
            pulp::audio::BufferView<const float> empty;
            process(*o, empty, mi, mo, ctx);
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_clap_a() {
    return std::make_unique<ClapBundleProcessor<'A'>>();
}
std::unique_ptr<pulp::format::Processor> create_clap_b() {
    return std::make_unique<ClapBundleProcessor<'B'>>();
}

}  // namespace

// Two plugins, one module entry — the bundle shape.
PULP_CLAP_BUNDLE_PLUGIN(ClapA, create_clap_a)
PULP_CLAP_BUNDLE_PLUGIN(ClapB, create_clap_b)
PULP_CLAP_BUNDLE_ENTRY()

TEST_CASE("CLAP bundle exposes two plugins from one entry", "[clap][bundle][entry]") {
    // init() builds every registered plugin's descriptor (deferred from static
    // init). Nothing was constructed before this call.
    REQUIRE(clap_entry.init("test"));

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    REQUIRE(factory->get_plugin_count(factory) == 2);

    // Both descriptors present and distinct.
    std::string id0 = factory->get_plugin_descriptor(factory, 0)->id;
    std::string id1 = factory->get_plugin_descriptor(factory, 1)->id;
    REQUIRE(id0 != id1);
    REQUIRE(factory->get_plugin_descriptor(factory, 2) == nullptr);

    // create_plugin resolves BY ID to the right plugin — not position, not a
    // shared global.
    const clap_plugin_t* pa =
        factory->create_plugin(factory, nullptr, "com.pulp.test.clapbundle.a");
    const clap_plugin_t* pb =
        factory->create_plugin(factory, nullptr, "com.pulp.test.clapbundle.b");
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    REQUIRE(std::strcmp(pa->desc->name, "ClapBundleA") == 0);
    REQUIRE(std::strcmp(pb->desc->name, "ClapBundleB") == 0);
    // A is an Effect (audio in), B is an Instrument (note in, no audio in).
    REQUIRE(std::strcmp(pa->desc->features[0], CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0);
    REQUIRE(std::strcmp(pb->desc->features[0], CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0);

    // Unknown id resolves to nothing.
    REQUIRE(factory->create_plugin(factory, nullptr, "com.pulp.test.missing") == nullptr);

    pa->destroy(pa);
    pb->destroy(pb);

    // The shared keyed registry carries both (cross-format enumeration / editor
    // assets), each bound to its own factory.
    const auto* ra = pulp::format::find_plugin("com.pulp.test.clapbundle.a");
    const auto* rb = pulp::format::find_plugin("com.pulp.test.clapbundle.b");
    REQUIRE(ra != nullptr);
    REQUIRE(rb != nullptr);
    CHECK(ra->factory == create_clap_a);
    CHECK(rb->factory == create_clap_b);

    clap_entry.deinit();
}

TEST_CASE("CLAP bundle drops a duplicate plugin id from enumeration",
          "[clap][bundle][entry]") {
    using namespace pulp::format::clap_generic;

    // A developer mistake: two records sharing one bundle_id. create_plugin can
    // only ever resolve the first, so the host must not be handed the second as
    // an advertised-but-unreachable descriptor — in the CLAP factory OR in the
    // shared keyed registry.
    reset_clap_records_for_testing();
    pulp::format::reset_registry_for_testing();
    register_clap_record(create_clap_a, /*publish_keyed=*/true);
    register_clap_record(create_clap_a, /*publish_keyed=*/true);  // same id
    init_all_records();

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    REQUIRE(factory->get_plugin_count(factory) == 1);  // deduped
    REQUIRE(std::strcmp(factory->get_plugin_descriptor(factory, 0)->id,
                        "com.pulp.test.clapbundle.a") == 0);
    REQUIRE(factory->get_plugin_descriptor(factory, 1) == nullptr);
    // The keyed registry dedups too: publication happens after dedup, so the
    // duplicate never lands a second (unreachable) entry.
    REQUIRE(pulp::format::registered_plugins().size() == 1);

    // Restore the static two-plugin bundle so this test is order-independent.
    reset_clap_records_for_testing();
    pulp::format::reset_registry_for_testing();
    register_clap_record(create_clap_a, /*publish_keyed=*/true);
    register_clap_record(create_clap_b, /*publish_keyed=*/true);
    init_all_records();
}

TEST_CASE("CLAP keyed-registry publication is gated to bundle records",
          "[clap][bundle][entry]") {
    namespace pf = pulp::format;

    // Legacy single-plugin contract: register_clap_record(publish_keyed=false)
    // (what PULP_CLAP_PLUGIN emits) must leave the shared keyed table empty —
    // matching AU/VST3, where only bundle macros publish keyed entries.
    pf::clap_generic::reset_clap_records_for_testing();
    pf::reset_registry_for_testing();
    pf::clap_generic::register_clap_record(create_clap_a, /*publish_keyed=*/false);
    pf::clap_generic::init_all_records();
    CHECK(pf::registered_plugins().empty());
    CHECK(pf::find_plugin("com.pulp.test.clapbundle.a") == nullptr);

    // A bundle record (publish_keyed=true) DOES publish itself.
    pf::clap_generic::reset_clap_records_for_testing();
    pf::reset_registry_for_testing();
    pf::clap_generic::register_clap_record(create_clap_a, /*publish_keyed=*/true);
    pf::clap_generic::init_all_records();
    CHECK(pf::registered_plugins().size() == 1);
    CHECK(pf::find_plugin("com.pulp.test.clapbundle.a") != nullptr);

    // Restore the static two-plugin bundle so this test is order-independent.
    pf::clap_generic::reset_clap_records_for_testing();
    pf::reset_registry_for_testing();
    pf::clap_generic::register_clap_record(create_clap_a, /*publish_keyed=*/true);
    pf::clap_generic::register_clap_record(create_clap_b, /*publish_keyed=*/true);
    pf::clap_generic::init_all_records();
}

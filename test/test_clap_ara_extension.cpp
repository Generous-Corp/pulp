// End-to-end test for the CLAP → ARA companion-factory path.
// Workstream 06 slice A2a — exercises the same code path a real CLAP
// host (Bitwig, REAPER) takes: constructs a PulpClapPlugin, calls
// clap_adapter::clap_get_extension(kClapAraFactoryExtension), and
// verifies the result.
//
// Without PULP_HAS_ARA the factory must be nullptr (no SDK linked in).
// With PULP_HAS_ARA the factory must be a valid ARA::ARAFactory — the
// same pointer ara_factory.cpp publishes.

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/format/clap_adapter.hpp>

#include <cstring>

using namespace pulp;
using namespace pulp::format;

namespace {

// Minimal Processor used for the test. Doesn't override
// create_ara_document_controller, so the CLAP adapter ends up with a
// null ara_controller. The factory pointer is still returned because
// ara_factory.cpp publishes a shared factory per process.
class NullProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "Null";
        d.manufacturer = "Test";
        d.bundle_id = "com.test.null";
        d.version = "0.0.0";
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

std::unique_ptr<Processor> make_null() {
    return std::make_unique<NullProcessor>();
}

} // namespace

TEST_CASE("CLAP get_extension returns expected ARA-factory behaviour per build",
          "[ara][clap][live]") {
    pulp::format::clap_adapter::PulpClapPlugin plugin;
    plugin.factory = make_null;
    plugin.plugin.plugin_data = &plugin;

    const void* ext = clap_adapter::clap_get_extension(
        &plugin.plugin, kClapAraFactoryExtension);

#ifdef PULP_HAS_ARA
    REQUIRE(ext != nullptr);
    // The factory returned from the CLAP extension path must match the
    // one ara_companion_factory_for publishes directly. CLAP hosts and
    // VST3/AU hosts therefore agree on a single factory identity.
    REQUIRE(ext == ara_companion_factory_for(plugin.ara_controller.get()));
#else
    REQUIRE(ext == nullptr);
#endif
}

TEST_CASE("CLAP get_extension ignores unknown extension IDs",
          "[ara][clap][live]") {
    pulp::format::clap_adapter::PulpClapPlugin plugin;
    plugin.factory = make_null;
    plugin.plugin.plugin_data = &plugin;

    REQUIRE(clap_adapter::clap_get_extension(&plugin.plugin,
                                             "com.example.not-a-real-ext") == nullptr);
}

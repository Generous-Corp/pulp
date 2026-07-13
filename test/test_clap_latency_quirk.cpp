// test_clap_latency_quirk.cpp — empirical proof that the CLAP adapter
// actually RESPECTS the `clamp_latency_to_nonneg` host-quirk end-to-end
// (host-quirks enforcement plan, P3a).
//
// The unit tests in test_host_quirks.cpp prove the helper logic + runtime
// policy in isolation. This test drives the *real* CLAP adapter through
// the clap_plugin_latency extension with a processor that reports a
// negative latency, and asserts the host-visible value:
//   * quirk enforced (default policy)         → clamped to 0
//   * quirk disabled (PULP_HOST_QUIRKS=off)   → raw value wraps (unsigned)
// proving the adapter consults the quirk rather than hardcoding behavior.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/clap_entry.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <clap/clap.h>

#include <memory>
#include <optional>
#include <string>

using namespace pulp;
using namespace pulp::format;

namespace {

constexpr const char* kPluginId = "com.pulp.test.latency-quirk";

// Pathological processor: reports a negative latency so the clamp is
// observable. CLAP reports latency as uint32, so the raw value wraps when
// the quirk is filtered out.
class NegativeLatencyProcessor : public Processor {
public:
    static constexpr int kRawLatency = -64;

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "PulpLatencyQuirk";
        d.manufacturer = "PulpTest";
        d.bundle_id = kPluginId;
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
        d.accepts_midi = false;
        d.tail_samples = 0;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const ProcessContext&) override {}
    int latency_samples() const override { return kRawLatency; }
};

std::unique_ptr<Processor> make_negative_latency() {
    return std::make_unique<NegativeLatencyProcessor>();
}

}  // namespace

PULP_CLAP_PLUGIN(make_negative_latency)

namespace {

// Create + init a fresh plugin instance (init() is where the adapter
// caches resolved_quirks()), read its host-reported latency via the CLAP
// latency extension, then destroy it. Set the desired policy BEFORE
// calling this so the cached quirks reflect it.
uint32_t adapter_reported_latency() {
    auto* factory = static_cast<const clap_plugin_factory_t*>(
        clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);
    const clap_plugin_t* p = factory->create_plugin(factory, nullptr, desc->id);
    REQUIRE(p != nullptr);
    REQUIRE(p->init(p));  // caches resolved_quirks() under the current policy
    auto* latency = static_cast<const clap_plugin_latency_t*>(
        p->get_extension(p, CLAP_EXT_LATENCY));
    REQUIRE(latency != nullptr);
    const uint32_t reported = latency->get(p);
    p->destroy(p);
    return reported;
}

}  // namespace

TEST_CASE("CLAP adapter respects clamp_latency_to_nonneg end-to-end",
          "[format][host-quirks][p3][clap][latency]") {
    REQUIRE(clap_entry.init("test"));  // builds the plugin's descriptor
    set_host_quirk_policy(std::nullopt);
    clear_quirk_overrides();

    SECTION("enforced policy clamps the processor's negative latency to 0") {
        set_host_quirk_policy(QuirkFilter{});  // all tiers — clamp is Validated
        REQUIRE(adapter_reported_latency() == 0u);
    }

    SECTION("PULP_HOST_QUIRKS=off lets the raw negative latency wrap through") {
        set_host_quirk_policy(kQuirkFilterOff);
        REQUIRE(adapter_reported_latency()
                == static_cast<uint32_t>(NegativeLatencyProcessor::kRawLatency));
    }

    set_host_quirk_policy(std::nullopt);
    clap_entry.deinit();
}

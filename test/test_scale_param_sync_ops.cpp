// The per-block parameter sync at 2,000 parameters, held to a bound by
// counting its work rather than timing it, so the check cannot flake.
//
// The CLAP and VST3 adapters run in this process (the Pulp Scale Probe
// processor behind PULP_CLAP_PLUGIN and PulpVst3Processor), driven through
// real PluginSlots, so the counters in adapter_boundary.hpp are the ones the
// adapters update. A quiet block (nothing written to the store) must count no
// reads, compares or index scans; a block after one host-automated parameter
// must count about one pass over the parameters, never a full pass per
// parameter. The controls prove the counters see the work: an automated block
// must count the re-read, and a block whose processor writes the store must
// count the diff.

#include <catch2/catch_test_macros.hpp>

// The VST3 adapter header first: the SDK's controller headers expect to see
// their own interface declarations before any other plug-in API's.
#include <pulp/format/vst3_adapter.hpp>

#include "fixtures/scale_probe/scale_probe.hpp"
#include "plugin_slot_clap_internal.hpp"
#include "plugin_slot_vst3_internal.hpp"
#include "scale_probe_hosting.hpp"

#include <pulp/format/adapter_boundary.hpp>
#include <pulp/format/clap_entry.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

PULP_CLAP_PLUGIN(pulp::test_fixtures::create_scale_probe)

namespace {

namespace host = pulp::host;
namespace boundary = pulp::format::boundary;
using Probe = pulp::test_fixtures::ScaleProbe;

constexpr int kFrames = 64;
constexpr std::uint64_t kN = Probe::kParamCount;
// One pass plus the handful of parameters a block touches directly.
constexpr std::uint64_t kOnePass = kN + 16;

std::unique_ptr<host::PluginSlot> clap_in_process() {
    static const bool initialized = clap_entry.init("pulp-scale-probe-in-process");
    REQUIRE(initialized);
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory);
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc);
    host::PluginInfo info;
    info.format = host::PluginFormat::CLAP;
    info.name = "Pulp Scale Probe (in process)";
    // make_clap_slot leaves init to the creator, as a loader does after create.
    return host::make_clap_slot(info, [factory, desc](const clap_host_t* clap_host) {
        const clap_plugin_t* plugin = factory->create_plugin(factory, clap_host, desc->id);
        if (plugin && !plugin->init(plugin)) {
            plugin->destroy(plugin);
            return static_cast<const clap_plugin_t*>(nullptr);
        }
        return plugin;
    });
}

std::unique_ptr<host::PluginSlot> vst3_in_process() {
    using namespace Steinberg;
    auto* plugin =
        new pulp::format::vst3::PulpVst3Processor(pulp::test_fixtures::create_scale_probe);
    REQUIRE(plugin->initialize(host::vst3_host_application()) == kResultOk);
    Vst::IAudioProcessor* audio = nullptr;
    plugin->queryInterface(Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&audio));
    Vst::IEditController* controller = nullptr;
    plugin->queryInterface(Vst::IEditController::iid, reinterpret_cast<void**>(&controller));
    host::PluginInfo info;
    info.format = host::PluginFormat::VST3;
    info.name = "Pulp Scale Probe (in process)";
    return host::make_vst3_slot(info, plugin, audio, controller);
}

struct Adapter {
    const char* label;
    std::function<std::unique_ptr<host::PluginSlot>()> make;
};

std::vector<Adapter> adapters() {
    return {{"CLAP", clap_in_process}, {"VST3", vst3_in_process}};
}

} // namespace

TEST_CASE("Per-block parameter sync at 2,000 parameters counts nothing when quiet and one pass "
          "when automated",
          "[host][scale][param-sync][rt-safety]") {
    // Without counting compiled in every delta is zero, which would read as a pass.
    REQUIRE(boundary::kParamSyncCountersEnabled);
    for (const auto& adapter : adapters()) {
        INFO(adapter.label);
        auto slot = adapter.make();
        REQUIRE(slot);
        REQUIRE(slot->prepare(48000.0, kFrames));
        pulp::test::scale_probe::Block<kFrames> block;
        for (int i = 0; i < 16; ++i)
            block.run(*slot);

        // Quiet blocks: no store write, so no snapshot, diff or index scan.
        const auto quiet_start = boundary::param_sync_counts();
        for (int i = 0; i < 64; ++i)
            block.run(*slot);
        const auto quiet = boundary::param_sync_counts() - quiet_start;
        INFO("quiet: " << quiet.reads << " reads, " << quiet.compares << " compares, "
                       << quiet.scans << " scans over 64 blocks");
        CHECK(quiet.reads == 0);
        CHECK(quiet.compares == 0);
        CHECK(quiet.scans == 0);

        // One host-automated parameter per block.
        boundary::ParamSyncCounts worst{};
        for (int i = 0; i < 64; ++i) {
            const int index = 2 + (i * 7) % (Probe::kParamCount - 2);
            block.params.push({Probe::param_id(index), 0, (i % 2) ? 0.25f : 0.75f, 0});
            const auto before = boundary::param_sync_counts();
            block.run(*slot);
            const auto d = boundary::param_sync_counts() - before;
            worst.reads = std::max(worst.reads, d.reads);
            worst.compares = std::max(worst.compares, d.compares);
            worst.scans = std::max(worst.scans, d.scans);
        }
        INFO("automated, worst block: " << worst.reads << " reads, " << worst.compares
                                        << " compares, " << worst.scans << " scans");
        CHECK(worst.reads <= kOnePass);
        CHECK(worst.compares <= kOnePass);
        CHECK(worst.scans <= kOnePass);
        // Control: the write did trigger the re-read the bound is about.
        CHECK(worst.reads >= kN);

        // Control for the diff: input makes the processor write its Input Peak
        // during process(), so the post-process diff must run, once.
        boundary::ParamSyncCounts input_worst{};
        for (int b = 0; b < 8; ++b) {
            for (int i = 0; i < kFrames; ++i) {
                const float v = 0.1f * static_cast<float>(b + 1) *
                                static_cast<float>(std::sin(0.05 * (b * kFrames + i)));
                block.input(0)[i] = v;
                block.input(1)[i] = v;
            }
            const auto before = boundary::param_sync_counts();
            block.run(*slot);
            const auto d = boundary::param_sync_counts() - before;
            input_worst.compares = std::max(input_worst.compares, d.compares);
            input_worst.reads = std::max(input_worst.reads, d.reads);
        }
        INFO("processor writes, worst block: " << input_worst.reads << " reads, "
                                               << input_worst.compares << " compares");
        CHECK(input_worst.compares >= kN);
        CHECK(input_worst.compares <= kOnePass);
        CHECK(input_worst.reads <= kOnePass);
    }
}

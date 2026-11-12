// The format adapters at product scale: an instrument with 2,000 parameters,
// six stereo outputs and a stereo input (the Pulp Scale Probe fixture), loaded
// as each format's real bundle through PluginSlot, the path a user's plug-in
// takes. For VST3, CLAP and (on macOS) AU it proves:
// - the host sees all 2,000 parameter IDs, once each, with the same names and
//   order in two independent instances;
// - state saved from one instance restores into another and saves back
//   byte-identically;
// - the format's own bus API reports six stereo outputs (main first, then
//   aux) and one stereo input, and a render puts each bus's own tone on that
//   bus, while the input reaches the processor;
// - the audio callback allocates nothing at that scale, with and without a
//   host-automated parameter in the block (the per-block parameter sync is
//   the part that grows with the parameter count).
// The standalone's persistence path (plugin_state_io) is proved at the same
// scale without a bundle. The timing of the per-block sync is measured in
// test_host_scale_probe_sync_cost.cpp, outside the required gate.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "scale_probe_hosting.hpp"

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/format/detail/vst3_midi_mapping.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/state/store.hpp>

#if defined(PULP_TEST_SCALE_PROBE_CLAP) || defined(PULP_TEST_SCALE_PROBE_VST3)
#include <dlfcn.h>
#endif
#ifdef PULP_TEST_SCALE_PROBE_CLAP
#include <clap/clap.h>
#endif
#ifdef PULP_TEST_SCALE_PROBE_VST3
#include "plugin_slot_vst3_internal.hpp"
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::test::scale_probe;
namespace host = pulp::host;

namespace {

constexpr int kFrames = 64;
constexpr double kRate = 48000.0;

std::unique_ptr<host::PluginSlot> load_prepared(const Target& target) {
    std::string why;
    auto slot = load(target, why);
    INFO(target.label << ": " << why);
    REQUIRE(slot);
    REQUIRE(slot->prepare(kRate, kFrames));
    return slot;
}

/// The 64 parameters the state test writes: dense ones across every group
/// and all 16 sparse ones.
std::vector<int> state_indices() {
    std::vector<int> out;
    for (int k = 0; k < 48; ++k)
        out.push_back(2 + k * 41);
    for (int i = Probe::kDenseParams; i < Probe::kParamCount; ++i)
        out.push_back(i);
    return out;
}

/// A plain value inside `info`'s range, distinct per index.
float value_for(const host::HostParamInfo& info, int k) {
    const double frac = 0.1 + 0.8 * std::fmod(0.6180339887 * (k + 1), 1.0);
    const float v = info.min_value + static_cast<float>(frac) * (info.max_value - info.min_value);
    // Wide ranges include the stepped 0-127 ones: keep every value on a step.
    return info.max_value - info.min_value >= 2.0f ? std::round(v) : v;
}

struct BusLayout {
    std::vector<int> outputs; // channels per output bus
    std::vector<bool> output_main;
    std::vector<int> inputs;
};

std::string binary_of(const char* bundle) {
    namespace fs = std::filesystem;
    const fs::path p(bundle);
    if (fs::is_directory(p))
        return (p / "Contents" / "MacOS" / p.stem()).string();
    return p.string();
}

#ifdef PULP_TEST_SCALE_PROBE_CLAP
const void* clap_host_extension(const clap_host_t*, const char*) {
    return nullptr;
}
void clap_host_noop(const clap_host_t*) {}

BusLayout clap_layout(const char* bundle) {
    BusLayout out;
    void* handle = dlopen(binary_of(bundle).c_str(), RTLD_LAZY | RTLD_LOCAL);
    REQUIRE(handle);
    const auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    REQUIRE(entry);
    REQUIRE(entry->init(bundle));
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory);
    REQUIRE(factory->get_plugin_count(factory) >= 1);
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    static const clap_host_t clap_host{
        CLAP_VERSION_INIT,   nullptr,        "Pulp scale test", "Pulp",        "", "1.0",
        clap_host_extension, clap_host_noop, clap_host_noop,    clap_host_noop};
    const clap_plugin_t* plugin = factory->create_plugin(factory, &clap_host, desc->id);
    REQUIRE(plugin);
    REQUIRE(plugin->init(plugin));
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    REQUIRE(ports);
    for (bool is_input : {false, true}) {
        const auto n = ports->count(plugin, is_input);
        for (std::uint32_t i = 0; i < n; ++i) {
            clap_audio_port_info_t info{};
            REQUIRE(ports->get(plugin, i, is_input, &info));
            if (is_input) {
                out.inputs.push_back(static_cast<int>(info.channel_count));
            } else {
                out.outputs.push_back(static_cast<int>(info.channel_count));
                out.output_main.push_back((info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0);
            }
        }
    }
    plugin->destroy(plugin);
    entry->deinit();
    return out;
}
#endif

#ifdef PULP_TEST_SCALE_PROBE_VST3
BusLayout vst3_layout(const char* bundle) {
    using namespace Steinberg;
    BusLayout out;
    void* handle = dlopen(binary_of(bundle).c_str(), RTLD_LAZY | RTLD_LOCAL);
    REQUIRE(handle);
    if (auto* enter = reinterpret_cast<bool (*)(void*)>(dlsym(handle, "bundleEntry")))
        REQUIRE(enter(nullptr));
    using GetFactory = IPluginFactory*(PLUGIN_API*)();
    auto get_factory = reinterpret_cast<GetFactory>(dlsym(handle, "GetPluginFactory"));
    REQUIRE(get_factory);
    IPluginFactory* factory = get_factory();
    REQUIRE(factory);
    Vst::IComponent* component = nullptr;
    for (int32 i = 0; i < factory->countClasses() && !component; ++i) {
        PClassInfo info{};
        if (factory->getClassInfo(i, &info) == kResultOk &&
            std::strcmp(info.category, kVstAudioEffectClass) == 0)
            factory->createInstance(info.cid, Vst::IComponent::iid,
                                    reinterpret_cast<void**>(&component));
    }
    REQUIRE(component);
    REQUIRE(component->initialize(host::vst3_host_application()) == kResultOk);
    for (auto dir : {Vst::kOutput, Vst::kInput}) {
        const int32 n = component->getBusCount(Vst::kAudio, dir);
        for (int32 i = 0; i < n; ++i) {
            Vst::BusInfo info{};
            REQUIRE(component->getBusInfo(Vst::kAudio, dir, i, info) == kResultOk);
            if (dir == Vst::kInput) {
                out.inputs.push_back(info.channelCount);
            } else {
                out.outputs.push_back(info.channelCount);
                out.output_main.push_back(info.busType == Vst::kMain);
            }
        }
    }
    component->terminate();
    component->release();
    factory->release();
    if (auto* leave = reinterpret_cast<bool (*)()>(dlsym(handle, "bundleExit")))
        leave();
    return out;
}
#endif

#ifdef PULP_TEST_SCALE_PROBE_AU
BusLayout au_layout() {
    BusLayout out;
    REQUIRE_FALSE(au_component_id().empty()); // registers the component once
    const auto desc = pulp::test::au_component_description(PULP_TEST_SCALE_PROBE_AU, false);
    REQUIRE(desc);
    AudioComponent component = AudioComponentFindNext(nullptr, &*desc);
    REQUIRE(component);
    AudioComponentInstance unit = nullptr;
    REQUIRE(AudioComponentInstanceNew(component, &unit) == noErr);
    for (auto scope : {kAudioUnitScope_Output, kAudioUnitScope_Input}) {
        UInt32 count = 0;
        UInt32 size = sizeof(count);
        REQUIRE(AudioUnitGetProperty(unit, kAudioUnitProperty_ElementCount, scope, 0, &count,
                                     &size) == noErr);
        for (UInt32 e = 0; e < count; ++e) {
            AudioStreamBasicDescription asbd{};
            size = sizeof(asbd);
            REQUIRE(AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, scope, e, &asbd,
                                         &size) == noErr);
            if (scope == kAudioUnitScope_Input) {
                out.inputs.push_back(static_cast<int>(asbd.mChannelsPerFrame));
            } else {
                out.outputs.push_back(static_cast<int>(asbd.mChannelsPerFrame));
                out.output_main.push_back(e == 0); // AU element 0 is the main output
            }
        }
    }
    // The unit also initializes at that layout, at its own default formats...
    const OSStatus initialized = AudioUnitInitialize(unit);
    INFO("AudioUnitInitialize at the default formats: " << initialized);
    CHECK(initialized == noErr);
    if (initialized == noErr)
        AudioUnitUninitialize(unit);
    // ...and after a host sets every element, input and all six outputs, to
    // 48 kHz stereo, as a host does before it renders.
    AudioStreamBasicDescription stereo48{};
    stereo48.mSampleRate = 48000.0;
    stereo48.mFormatID = kAudioFormatLinearPCM;
    stereo48.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    stereo48.mBytesPerPacket = sizeof(float);
    stereo48.mFramesPerPacket = 1;
    stereo48.mBytesPerFrame = sizeof(float);
    stereo48.mChannelsPerFrame = 2;
    stereo48.mBitsPerChannel = 32;
    for (auto scope : {kAudioUnitScope_Input, kAudioUnitScope_Output}) {
        const auto count = scope == kAudioUnitScope_Input ? out.inputs.size() : out.outputs.size();
        for (UInt32 e = 0; e < count; ++e) {
            INFO("set 48 kHz on " << (scope == kAudioUnitScope_Input ? "input" : "output")
                                  << " element " << e);
            CHECK(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, e, &stereo48,
                                       sizeof(stereo48)) == noErr);
        }
    }
    const OSStatus initialized48 = AudioUnitInitialize(unit);
    INFO("AudioUnitInitialize at 48 kHz on every element: " << initialized48);
    CHECK(initialized48 == noErr);
    if (initialized48 == noErr)
        AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    return out;
}
#endif

BusLayout native_layout(const Target& target) {
#ifdef PULP_TEST_SCALE_PROBE_CLAP
    if (target.format == host::PluginFormat::CLAP)
        return clap_layout(target.path);
#endif
#ifdef PULP_TEST_SCALE_PROBE_VST3
    if (target.format == host::PluginFormat::VST3)
        return vst3_layout(target.path);
#endif
#ifdef PULP_TEST_SCALE_PROBE_AU
    if (target.format == host::PluginFormat::AudioUnitV3)
        return au_layout();
#endif
    FAIL("no native bus query for " << target.label);
    return {};
}

} // namespace

TEST_CASE("Scale probe bundles are built for every hosted format", "[host][scale]") {
    CHECK_FALSE(targets().empty());
}

TEST_CASE("Every format shows the host all 2,000 parameter IDs, stable across instances",
          "[host][scale][params]") {
    const auto expected = fixture_ids();
    for (const auto& target : targets()) {
        INFO(target.label);
        auto first = load_prepared(target);
        auto second = load_prepared(target);
        const auto a = first->parameters();
        const auto b = second->parameters();

        std::map<std::uint32_t, int> seen;
        std::vector<std::uint32_t> order_a;
        std::vector<std::uint32_t> extras;
        std::set<std::uint32_t> midi_controllers;
        const std::set<std::uint32_t> wanted(expected.begin(), expected.end());
        for (const auto& p : a) {
            ++seen[p.id];
            if (wanted.count(p.id))
                order_a.push_back(p.id);
            else if (target.format == host::PluginFormat::VST3 &&
                     pulp::format::detail::is_vst3_midi_cc_param(p.id))
                midi_controllers.insert(p.id);
            else
                extras.push_back(p.id);
        }
        INFO(midi_controllers.size() << " hidden MIDI-controller parameter(s), " << extras.size()
                                     << " other parameter(s) beyond the fixture's");
        CHECK(extras.size() <= 1); // a synthesized bypass at most
        if (target.format == host::PluginFormat::VST3) {
            // VST3 has no MIDI CC events, so the adapter exposes one hidden
            // proxy parameter per channel and controller (IMidiMapping): a
            // fixed block in a reserved ID range, the same in every instance.
            namespace d = pulp::format::detail;
            REQUIRE(midi_controllers.size() ==
                    static_cast<std::size_t>(d::kVst3MidiChannels * d::kVst3ControllersPerChannel));
            for (const auto [channel, controller] :
                 {std::pair{0, 0}, std::pair{0, 7}, std::pair{9, 74}, std::pair{15, 129}}) {
                const auto id = d::vst3_midi_cc_param_id(channel, controller);
                INFO("channel " << channel << " controller " << controller);
                REQUIRE(midi_controllers.count(id) == 1);
                const auto decoded = d::vst3_decode_midi_cc_param(id);
                REQUIRE(decoded.channel == channel);
                REQUIRE(decoded.controller == controller);
            }
        } else {
            REQUIRE(midi_controllers.empty());
        }
        for (const auto id : expected) {
            INFO("parameter id " << id);
            REQUIRE(seen[id] == 1);
        }
        REQUIRE(order_a == expected);

        // Names, and the same list in a second, independent instance.
        REQUIRE(a.size() == b.size());
        std::map<std::uint32_t, std::string> names;
        for (const auto& p : a)
            names[p.id] = p.name;
        for (std::size_t i = 0; i < b.size(); ++i) {
            REQUIRE(b[i].id == a[i].id);
            REQUIRE(b[i].name == a[i].name);
        }
        for (int i :
             {0, 1, 2, 999, Probe::kDenseParams - 1, Probe::kDenseParams, Probe::kParamCount - 1}) {
            INFO("parameter index " << i);
            REQUIRE(names[Probe::param_id(i)] == Probe::param_name(i));
        }
    }
}

TEST_CASE("Every format restores 2,000-parameter state into a new instance byte-identically",
          "[host][scale][state]") {
    using clock = std::chrono::steady_clock;
    for (const auto& target : targets()) {
        INFO(target.label);
        auto source = load_prepared(target);
        const auto params = source->parameters();
        std::map<std::uint32_t, host::HostParamInfo> info;
        for (const auto& p : params)
            info[p.id] = p;

        const auto indices = state_indices();
        std::map<std::uint32_t, float> written;
        for (std::size_t k = 0; k < indices.size(); ++k) {
            const auto id = Probe::param_id(indices[k]);
            REQUIRE(info.count(id) == 1);
            const float v = value_for(info[id], static_cast<int>(k));
            source->set_parameter(id, v);
            written[id] = v;
        }
        Block<kFrames> block;
        block.run(*source); // writes queued for the audio thread land here

        const auto save_start = clock::now();
        const auto saved = source->save_state();
        const auto save_us =
            std::chrono::duration<double, std::micro>(clock::now() - save_start).count();
        REQUIRE_FALSE(saved.empty());

        auto restored = load_prepared(target);
        const auto restore_start = clock::now();
        REQUIRE(restored->restore_state(saved));
        const auto restore_us =
            std::chrono::duration<double, std::micro>(clock::now() - restore_start).count();
        Block<kFrames> block2;
        block2.run(*restored);
        const auto resaved = restored->save_state();
        WARN(target.label << ": state " << saved.size() << " bytes, save " << save_us
                          << " us, restore " << restore_us << " us");

        REQUIRE(resaved == saved);
        for (const auto& [id, v] : written) {
            INFO("parameter id " << id);
            const float tolerance =
                std::max(1e-3f, 1e-4f * (info[id].max_value - info[id].min_value));
            REQUIRE(std::abs(restored->get_parameter(id) - v) <= tolerance);
        }
    }
}

TEST_CASE("Every format reports six stereo outputs and a stereo input through its own bus API",
          "[host][scale][buses]") {
    for (const auto& target : targets()) {
        INFO(target.label);
        const auto layout = native_layout(target);
        REQUIRE(layout.outputs == std::vector<int>(Probe::kOutputBuses, 2));
        REQUIRE(layout.output_main.size() == static_cast<std::size_t>(Probe::kOutputBuses));
        REQUIRE(layout.output_main[0]);
        for (int k = 1; k < Probe::kOutputBuses; ++k)
            REQUIRE_FALSE(layout.output_main[static_cast<std::size_t>(k)]);
        REQUIRE(layout.inputs == std::vector<int>{2});
    }
}

TEST_CASE("Every format renders each output bus's own tone and delivers the input",
          "[host][scale][buses]") {
    namespace analysis = pulp::test::audio;
    constexpr int kBlocks = 64;
    for (const auto& target : targets()) {
        INFO(target.label);
        auto slot = load_prepared(target);
        Block<kFrames> block;
        std::vector<std::vector<double>> captured(Probe::kOutputBuses);
        double input_phase = 0.0;
        for (int b = 0; b < kBlocks; ++b) {
            for (int i = 0; i < kFrames; ++i) {
                const float v = 0.5f * static_cast<float>(std::sin(input_phase));
                block.input(0)[i] = v;
                block.input(1)[i] = v;
                input_phase += 2.0 * 3.14159265358979323846 * 1000.0 / kRate;
            }
            block.run(*slot);
            for (int k = 0; k < Probe::kOutputBuses; ++k)
                for (int i = 0; i < kFrames; ++i)
                    captured[static_cast<std::size_t>(k)].push_back(block.output(k, 0)[i]);
        }
        for (int k = 0; k < Probe::kOutputBuses; ++k) {
            INFO("output bus " << k);
            const auto& samples = captured[static_cast<std::size_t>(k)];
            const auto own = analysis::fit_tone(samples, Probe::bus_frequency(k) / kRate);
            CHECK(std::abs(own.amplitude - Probe::kToneLevel) < 0.01);
            // Not a neighbour's tone: a swapped bus reads its neighbour here.
            for (int other : {k - 1, k + 1}) {
                if (other < 0 || other >= Probe::kOutputBuses)
                    continue;
                const auto wrong = analysis::fit_tone(samples, Probe::bus_frequency(other) / kRate);
                CHECK(wrong.amplitude < 0.01);
            }
        }
        // The input reached the processor, which reports its peak.
        const float peak = slot->get_parameter(Probe::param_id(Probe::kInputPeakIndex));
        CHECK(std::abs(peak - 0.5f) < 0.02f);
    }
}

TEST_CASE("Every format's audio callback allocates nothing at 2,000 parameters",
          "[host][scale][rt-safety]") {
    constexpr int kBlocks = 256;
    for (const auto& target : targets()) {
        INFO(target.label);
        auto slot = load_prepared(target);
        Block<kFrames> block;
        for (int i = 0; i < 16; ++i)
            block.run(*slot);

        std::size_t quiet = 0;
        for (int i = 0; i < kBlocks; ++i) {
            pulp::test::RtAllocationProbe probe;
            block.run(*slot);
            quiet += probe.allocation_count();
        }
        CHECK(quiet == 0);

        // One host-automated parameter per block: every block then re-reads
        // and re-compares all 2,000 values in the adapters that sync per block.
        std::size_t automated = 0;
        for (int i = 0; i < kBlocks; ++i) {
            const int index = 2 + (i * 7) % (Probe::kParamCount - 2);
            block.params.push({Probe::param_id(index), 0, (i % 2) ? 0.25f : 0.75f, 0});
            pulp::test::RtAllocationProbe probe;
            block.run(*slot);
            automated += probe.allocation_count();
        }
        CHECK(automated == 0);

        // Control: the fixture allocates on command, inside the plug-in. If the
        // probe cannot see that, the zeros above prove nothing.
        slot->set_parameter(Probe::param_id(Probe::kCommandIndex), 1.0f);
        block.run(*slot);
        std::size_t commanded = 0;
        for (int i = 0; i < 4; ++i) {
            pulp::test::RtAllocationProbe probe;
            block.run(*slot);
            commanded += probe.allocation_count();
        }
        INFO("the allocation probe must see allocations made inside the hosted plug-in");
        REQUIRE(commanded > 0);
        slot->set_parameter(Probe::param_id(Probe::kCommandIndex), 0.0f);
    }
}

TEST_CASE("The standalone's persistence path round-trips 2,000-parameter state",
          "[host][scale][state][standalone]") {
    pulp::state::StateStore store;
    auto processor = pulp::test_fixtures::create_scale_probe();
    processor->define_parameters(store);
    REQUIRE(store.param_count() == static_cast<std::size_t>(Probe::kParamCount));
    const auto indices = state_indices();
    for (std::size_t k = 0; k < indices.size(); ++k) {
        const auto id = Probe::param_id(indices[k]);
        const auto* info = store.info(id);
        REQUIRE(info);
        const double frac = 0.1 + 0.8 * std::fmod(0.6180339887 * (k + 1), 1.0);
        store.set_value(
            id, static_cast<float>(info->range.min + frac * (info->range.max - info->range.min)));
    }
    const auto blob = pulp::format::plugin_state_io::serialize(store, *processor);
    REQUIRE_FALSE(blob.empty());

    pulp::state::StateStore fresh;
    auto other = pulp::test_fixtures::create_scale_probe();
    other->define_parameters(fresh);
    REQUIRE(pulp::format::plugin_state_io::deserialize(blob, fresh, *other));
    REQUIRE(pulp::format::plugin_state_io::serialize(fresh, *other) == blob);
    for (const auto index : indices) {
        const auto id = Probe::param_id(index);
        REQUIRE(fresh.get_value(id) == store.get_value(id));
    }
}

TEST_CASE("The AU instrument with six outputs and an input prepares in the AUv3 slot",
          "[host][scale][buses][au]") {
#ifdef PULP_TEST_SCALE_PROBE_AU
    // Loaded in process because the test registers the component with this
    // process only; the shared tone and state cases then render all seven
    // elements through the slot.
    const Target au{host::PluginFormat::AudioUnitV3, PULP_TEST_SCALE_PROBE_AU, "AU (in process)"};
    std::string why;
    auto slot = load(au, why);
    INFO(why);
    REQUIRE(slot);
    REQUIRE(slot->prepare(48000.0, kFrames));
    Block<kFrames> block;
    block.run(*slot);
#else
    SKIP("the AU component is not built in this configuration");
#endif
}

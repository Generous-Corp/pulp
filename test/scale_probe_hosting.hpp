#pragma once

// Loads the Pulp Scale Probe fixture (test/fixtures/scale_probe) as each
// format's real bundle through PluginSlot, and runs host blocks with its full
// bus layout: one stereo input and six stereo outputs. Shared by the scale
// tests (the required ones and the timing one).

#include "fixtures/scale_probe/scale_probe.hpp"

#include <pulp/host/parameter_event_queue.hpp>
#include <pulp/host/plugin_slot.hpp>

#ifdef PULP_TEST_SCALE_PROBE_AU
#include "support/au_component_registration.hpp"
#include <pulp/host/plugin_slot_auv3.hpp>
#endif

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pulp::test::scale_probe {

using Probe = pulp::test_fixtures::ScaleProbe;

struct Target {
    ::pulp::host::PluginFormat format;
    const char* path;
    const char* label;
};

inline std::vector<Target> targets() {
    std::vector<Target> out;
#ifdef PULP_TEST_SCALE_PROBE_VST3
    out.push_back({::pulp::host::PluginFormat::VST3, PULP_TEST_SCALE_PROBE_VST3, "VST3"});
#endif
#ifdef PULP_TEST_SCALE_PROBE_CLAP
    out.push_back({::pulp::host::PluginFormat::CLAP, PULP_TEST_SCALE_PROBE_CLAP, "CLAP"});
#endif
#ifdef PULP_TEST_SCALE_PROBE_AU
    out.push_back(
        {::pulp::host::PluginFormat::AudioUnitV3, PULP_TEST_SCALE_PROBE_AU, "AU (in process)"});
#endif
    return out;
}

#ifdef PULP_TEST_SCALE_PROBE_AU
/// "TYPE:SUBT:MANU" of the probe component, registered with this process once.
inline const std::string& au_component_id() {
    static const std::string id = [] {
        const auto desc = au_component_description(PULP_TEST_SCALE_PROBE_AU, true);
        return desc ? pulp::test::au_component_id(*desc) : std::string{};
    }();
    return id;
}
#endif

/// The loaded slot, or null with `why` filled.
inline std::unique_ptr<::pulp::host::PluginSlot> load(const Target& target, std::string& why) {
    if (!std::filesystem::exists(target.path)) {
        why = std::string("missing bundle ") + target.path;
        return nullptr;
    }
    ::pulp::host::PluginInfo info;
    info.format = target.format;
    info.path = target.path;
    info.name = "Pulp Scale Probe";
    info.is_instrument = true;
    info.is_effect = false;
    info.num_inputs = 2;
    info.num_outputs = 2;
    std::unique_ptr<::pulp::host::PluginSlot> slot;
#ifdef PULP_TEST_SCALE_PROBE_AU
    if (target.format == ::pulp::host::PluginFormat::AudioUnitV3) {
        info.unique_id = au_component_id();
        if (info.unique_id.empty()) {
            why = "the AU component did not register";
            return nullptr;
        }
        // The component is registered with this process only, so it must
        // load in process: the out-of-process host cannot see a process-local
        // registration and hands back a unit with no buses, which then fails
        // allocateRenderResources (-10875).
        ::pulp::host::AuV3LoadOptions options;
        options.out_of_process = false;
        slot = ::pulp::host::load_auv3_plugin(info, options);
    }
#endif
    if (!slot && target.format != ::pulp::host::PluginFormat::AudioUnitV3)
        slot = ::pulp::host::PluginSlot::load(info);
    if (!slot || !slot->is_loaded()) {
        why = "the slot did not load";
        return nullptr;
    }
    return slot;
}

/// Every fixture ID, in declaration order.
inline std::vector<std::uint32_t> fixture_ids() {
    std::vector<std::uint32_t> ids;
    ids.reserve(Probe::kParamCount);
    for (int i = 0; i < Probe::kParamCount; ++i)
        ids.push_back(Probe::param_id(i));
    return ids;
}

/// Host buses for one block at the fixture's full layout. Storage is sized
/// once, so running a block allocates nothing on the host side.
template <int Frames> struct Block {
    static constexpr int kOutputs = Probe::kOutputBuses;
    std::vector<std::vector<float>> store =
        std::vector<std::vector<float>>(2 + 2 * kOutputs, std::vector<float>(Frames, 0.0f));
    std::array<const float*, 2> in_ptrs{};
    std::array<std::array<float*, 2>, kOutputs> out_ptrs{};
    std::array<::pulp::format::ProcessBusBufferView<const float>, 1> ins{};
    std::array<::pulp::format::ProcessBusBufferView<float>, kOutputs> outs{};
    ::pulp::midi::MidiBuffer midi_in;
    ::pulp::midi::MidiBuffer midi_out;
    ::pulp::host::ParameterEventQueue params;
    ::pulp::format::ProcessContext context;

    Block() {
        in_ptrs = {store[0].data(), store[1].data()};
        ins[0].buffer = ::pulp::audio::BufferView<const float>(in_ptrs.data(), 2, Frames);
        for (int k = 0; k < kOutputs; ++k) {
            out_ptrs[k] = {store[2 + 2 * k].data(), store[3 + 2 * k].data()};
            outs[k].info.role =
                k == 0 ? ::pulp::format::BusRole::Main : ::pulp::format::BusRole::Aux;
            outs[k].buffer = ::pulp::audio::BufferView<float>(out_ptrs[k].data(), 2, Frames);
        }
        context.sample_rate = 48000.0;
        context.num_samples = Frames;
    }

    float* input(int channel) {
        return store[static_cast<std::size_t>(channel)].data();
    }
    const float* output(int bus, int channel) const {
        return store[static_cast<std::size_t>(2 + 2 * bus + channel)].data();
    }

    void run(::pulp::host::PluginSlot& slot) {
        ::pulp::format::ProcessBuffers buffers{
            ::pulp::format::ProcessBusBufferSet<const float>{std::span(ins)},
            ::pulp::format::ProcessBusBufferSet<float>{std::span(outs)}};
        slot.process(buffers, midi_in, midi_out, params, Frames, context);
        params.clear();
    }
};

} // namespace pulp::test::scale_probe

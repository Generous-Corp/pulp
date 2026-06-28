#include "harness/scoped_rt_process_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/native_core_processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/native_components/native_core.h>
#include <pulp/native_components/native_core.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/state/store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

pulp::state::ParamInfo make_param(pulp::state::ParamID id,
                                  const char* name,
                                  pulp::state::ParamRange range) {
    pulp::state::ParamInfo info;
    info.id = id;
    info.name = name;
    info.range = range;
    return info;
}

struct RtGainInstance {
    float gain = 1.0f;
    bool active = false;
};

const char kRtCoreId[] = "com.pulp.test.rt-gain";
const char kRtCoreName[] = "RT Gain";
const char kRtParamId[] = "gain";

pulp_native_descriptor_v1 g_rt_desc = [] {
    pulp_native_descriptor_v1 d{};
    d.size = sizeof(d);
    d.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    d.id = kRtCoreId;
    d.id_len = std::strlen(kRtCoreId);
    d.name = kRtCoreName;
    d.name_len = std::strlen(kRtCoreName);
    d.plugin_version = 1;
    d.default_input_bus_count = 1;
    d.default_output_bus_count = 1;
    return d;
}();

pulp_native_param_v1 g_rt_param = [] {
    pulp_native_param_v1 p{};
    p.size = sizeof(p);
    p.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    p.id = kRtParamId;
    p.id_len = std::strlen(kRtParamId);
    p.id_hash = pulp::native_components::param_id_hash(kRtParamId);
    p.min_value = 0.0;
    p.max_value = 2.0;
    p.default_value = 1.0;
    p.flags = PULP_NATIVE_PARAM_AUTOMATABLE;
    return p;
}();

const pulp_native_descriptor_v1* rt_descriptor() {
    return &g_rt_desc;
}

const pulp_native_param_v1* rt_parameters(uint32_t* count) {
    if (count != nullptr) *count = 1;
    return &g_rt_param;
}

pulp_native_status rt_create(const pulp_native_host_services_v1*,
                             pulp_native_instance** out) {
    *out = reinterpret_cast<pulp_native_instance*>(new RtGainInstance());
    return PULP_NATIVE_OK;
}

void rt_destroy(pulp_native_instance* instance) {
    delete reinterpret_cast<RtGainInstance*>(instance);
}

pulp_native_status rt_prepare(pulp_native_instance*,
                              const pulp_native_prepare_v1*) {
    return PULP_NATIVE_OK;
}

void rt_release(pulp_native_instance*) {}

pulp_native_status rt_set_bus_layout(pulp_native_instance*,
                                     const pulp_native_bus_layout_v1*) {
    return PULP_NATIVE_OK;
}

pulp_native_status rt_resume(pulp_native_instance* instance) {
    reinterpret_cast<RtGainInstance*>(instance)->active = true;
    return PULP_NATIVE_OK;
}

pulp_native_status rt_suspend(pulp_native_instance* instance) {
    reinterpret_cast<RtGainInstance*>(instance)->active = false;
    return PULP_NATIVE_OK;
}

void rt_reset(pulp_native_instance*) {}

pulp_native_status rt_process(pulp_native_instance* instance,
                              const pulp_native_process_v1* process) {
    auto* self = reinterpret_cast<RtGainInstance*>(instance);
    if (process->params != nullptr && process->params->events != nullptr) {
        for (uint32_t i = 0; i < process->params->count; ++i) {
            const auto& event = process->params->events[i];
            if (event.param_id_hash == g_rt_param.id_hash) {
                self->gain = static_cast<float>(event.value);
            }
        }
    }

    const auto* audio = process->audio;
    for (uint32_t bus = 0; bus < audio->output_bus_count; ++bus) {
        const auto& input_bus = audio->inputs[bus];
        const auto& output_bus = audio->outputs[bus];
        for (uint32_t channel = 0; channel < output_bus.channel_count; ++channel) {
            const float* in = input_bus.channels[channel];
            float* out = output_bus.channels[channel];
            for (uint32_t sample = 0; sample < audio->frame_count; ++sample) {
                out[sample] = in[sample] * self->gain;
            }
        }
    }
    return PULP_NATIVE_OK;
}

uint32_t rt_report_latency(pulp_native_instance*) {
    return 0;
}

uint32_t rt_report_tail(pulp_native_instance*) {
    return 0;
}

pulp_native_core_v1 g_rt_core = [] {
    pulp_native_core_v1 c{};
    c.size = sizeof(c);
    c.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    c.descriptor = rt_descriptor;
    c.parameters = rt_parameters;
    c.create = rt_create;
    c.destroy = rt_destroy;
    c.prepare = rt_prepare;
    c.release = rt_release;
    c.set_bus_layout = rt_set_bus_layout;
    c.resume = rt_resume;
    c.suspend = rt_suspend;
    c.reset = rt_reset;
    c.process = rt_process;
    c.report_latency = rt_report_latency;
    c.report_tail = rt_report_tail;
    return c;
}();

struct Block {
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float*> channels;

    Block(std::size_t frames, float fill)
        : left(frames, fill), right(frames, fill) {
        channels = {left.data(), right.data()};
    }
};

using pulp::test::ScopedRtProcessProbe;

}  // namespace

TEST_CASE("StateStore RT parameter writes do not allocate",
          "[rt-safety][state]") {
    pulp::state::StateStore store;
    store.add_parameter(make_param(1, "Gain", {0.0f, 1.0f, 0.0f}));

    int audio_listener_calls = 0;
    int main_listener_calls = 0;
    auto audio_token = store.add_audio_listener(
        [&](pulp::state::ParamID, float) { ++audio_listener_calls; });
    auto main_token = store.add_listener(
        [&](pulp::state::ParamID, float) { ++main_listener_calls; },
        pulp::state::ListenerThread::Main);

    std::size_t allocation_count = 0;
    std::size_t allocated_bytes = 0;
    {
        ScopedRtProcessProbe probe;
        store.set_value_rt(1, 0.75f);
        store.set_normalized_rt(1, 0.25f);
        allocation_count = probe.allocation_count();
        allocated_bytes = probe.allocated_bytes();
    }

    REQUIRE(allocation_count == 0);
    REQUIRE(allocated_bytes == 0);
    REQUIRE(audio_listener_calls == 2);
    REQUIRE(main_listener_calls == 0);
    REQUIRE_THAT(store.get_value(1), WithinAbs(0.25f, 1e-6f));
    // Two queued changes to the same param drain (count == 2) but coalesce
    // to a single current-value Main-listener call.
    REQUIRE(store.pump_listeners() == 2);
    REQUIRE(main_listener_calls == 1);
}

TEST_CASE("NativeCoreProcessor process runs without heap allocation",
          "[rt-safety][native-core-processor]") {
    pulp::format::NativeCoreProcessor proc(&g_rt_core);
    REQUIRE(proc.valid());

    pulp::state::StateStore store;
    proc.define_parameters(store);

    pulp::format::PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 64;
    prepare.input_channels = 2;
    prepare.output_channels = 2;
    proc.prepare(prepare);

    constexpr std::size_t kFrames = 64;
    Block input(kFrames, 1.0f);
    Block output(kFrames, 0.0f);
    pulp::audio::BufferView<float> output_view(output.channels.data(), 2, kFrames);
    pulp::audio::BufferView<const float> input_view(
        const_cast<const float* const*>(input.channels.data()), 2, kFrames);

    pulp::state::ParameterEventQueue events;
    REQUIRE(events.push({0, 0, 0.5f, 0}));
    proc.set_param_events(&events);

    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::format::ProcessContext process;
    process.sample_rate = 48000.0;
    process.num_samples = static_cast<int>(kFrames);

    std::size_t allocation_count = 0;
    std::size_t allocated_bytes = 0;
    {
        ScopedRtProcessProbe probe;
        proc.process(output_view, input_view, midi_in, midi_out, process);
        allocation_count = probe.allocation_count();
        allocated_bytes = probe.allocated_bytes();
    }

    REQUIRE(allocation_count == 0);
    REQUIRE(allocated_bytes == 0);
    REQUIRE_THAT(output.left.front(), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(output.right.back(), WithinAbs(0.5f, 1e-6f));
}

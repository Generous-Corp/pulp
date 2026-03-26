#pragma once

// Generic LV2 entry point generator
// Plugin developers include this and call PULP_LV2_PLUGIN() with their factory function.
// All LV2 boilerplate (descriptor, instantiate, run, cleanup) is generated automatically.
//
// Usage (in one .cpp file per plugin):
//   #include "my_processor.hpp"
//   #include <pulp/format/lv2_entry.hpp>
//   PULP_LV2_PLUGIN(my_namespace::create_my_processor, "http://pulp.audio/plugins/my_plugin")

#include <pulp/format/processor.hpp>
#include <pulp/format/lv2_adapter.hpp>
#include <pulp/runtime/log.hpp>

#include <lv2/core/lv2.h>

#include <cstring>
#include <vector>

namespace pulp::format::lv2_generic {

// Populated at static init by PULP_LV2_PLUGIN
inline ProcessorFactory g_factory = nullptr;
inline const char* g_uri = nullptr;

// ── LV2 Callbacks ────────────────────────────────────────────────────────

inline LV2_Handle instantiate(
    const LV2_Descriptor*,
    double sample_rate,
    const char*,
    const LV2_Feature* const*)
{
    if (!g_factory) return nullptr;

    auto* inst = new lv2_adapter::PulpLv2Instance();
    inst->factory = g_factory;
    inst->sample_rate = sample_rate;
    inst->processor = g_factory();
    if (!inst->processor) {
        delete inst;
        return nullptr;
    }

    // Define parameters
    inst->processor->define_parameters(inst->store);
    auto params = inst->store.all_parameters();
    inst->num_params = static_cast<int>(params.size());
    inst->param_ids.reserve(params.size());
    for (const auto& p : params) {
        inst->param_ids.push_back(p.id);
    }

    // Allocate control port pointer arrays
    inst->control_in_ports = new float*[inst->num_params]();
    inst->control_out_ports = new float*[inst->num_params]();

    // Count audio channels from descriptor
    auto desc = inst->processor->descriptor();
    inst->num_audio_inputs = 0;
    for (const auto& bus : desc.input_buses) {
        inst->num_audio_inputs += bus.default_channels;
    }
    inst->num_audio_outputs = 0;
    for (const auto& bus : desc.output_buses) {
        inst->num_audio_outputs += bus.default_channels;
    }

    // Prepare the processor
    format::PrepareContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.max_block_size = 4096;  // LV2 doesn't specify upfront; use reasonable max
    inst->processor->prepare(ctx);

    return static_cast<LV2_Handle>(inst);
}

inline void connect_port(LV2_Handle handle, uint32_t port, void* data) {
    auto* inst = static_cast<lv2_adapter::PulpLv2Instance*>(handle);

    int audio_in_end = inst->num_audio_inputs;
    int audio_out_end = audio_in_end + inst->num_audio_outputs;
    int control_end = audio_out_end + inst->num_params;

    int idx = static_cast<int>(port);

    if (idx < audio_in_end) {
        inst->audio_in_ports[idx] = static_cast<float*>(data);
    } else if (idx < audio_out_end) {
        inst->audio_out_ports[idx - audio_in_end] = static_cast<float*>(data);
    } else if (idx < control_end) {
        inst->control_in_ports[idx - audio_out_end] = static_cast<float*>(data);
    }
    // MIDI atom ports would follow control ports
}

inline void activate(LV2_Handle) {
    // Nothing needed — processor is prepared at instantiation
}

inline void run(LV2_Handle handle, uint32_t n_samples) {
    auto* inst = static_cast<lv2_adapter::PulpLv2Instance*>(handle);
    if (!inst->processor) return;

    // Read control port values into the parameter store
    for (int i = 0; i < inst->num_params; ++i) {
        if (inst->control_in_ports[i]) {
            float value = *inst->control_in_ports[i];
            inst->store.set_value(inst->param_ids[i], value);
        }
    }

    // Build buffer views
    const float* in_ptrs[lv2_adapter::kMaxChannels] = {};
    float* out_ptrs[lv2_adapter::kMaxChannels] = {};

    for (int i = 0; i < inst->num_audio_inputs && i < lv2_adapter::kMaxChannels; ++i) {
        in_ptrs[i] = inst->audio_in_ports[i];
    }
    for (int i = 0; i < inst->num_audio_outputs && i < lv2_adapter::kMaxChannels; ++i) {
        out_ptrs[i] = inst->audio_out_ports[i];
    }

    audio::BufferView<const float> input(in_ptrs,
        static_cast<size_t>(inst->num_audio_inputs), n_samples);
    audio::BufferView<float> output(out_ptrs,
        static_cast<size_t>(inst->num_audio_outputs), n_samples);

    // MIDI (TODO: parse LV2 atom sequences when MIDI ports are connected)
    midi::MidiBuffer midi_in, midi_out;

    format::ProcessContext proc_ctx;
    proc_ctx.sample_rate = inst->sample_rate;
    proc_ctx.block_size = static_cast<int>(n_samples);

    inst->processor->process(output, input, midi_in, midi_out, proc_ctx);
}

inline void deactivate(LV2_Handle) {
    // Nothing needed
}

inline void cleanup(LV2_Handle handle) {
    auto* inst = static_cast<lv2_adapter::PulpLv2Instance*>(handle);
    if (inst) {
        inst->processor->release();
        delete[] inst->control_in_ports;
        delete[] inst->control_out_ports;
        delete inst;
    }
}

// The LV2 descriptor
inline LV2_Descriptor g_lv2_descriptor = {
    nullptr,       // URI — set at static init
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    nullptr        // extension_data
};

} // namespace pulp::format::lv2_generic

// ── PULP_LV2_PLUGIN Macro ───────────────────────────────────────────────

#define PULP_LV2_PLUGIN(factory_fn, plugin_uri)                                 \
    namespace {                                                                  \
    struct PulpLv2Init {                                                         \
        PulpLv2Init() {                                                          \
            pulp::format::lv2_generic::g_factory = factory_fn;                   \
            pulp::format::lv2_generic::g_uri = plugin_uri;                       \
            pulp::format::lv2_generic::g_lv2_descriptor.URI = plugin_uri;        \
        }                                                                        \
    } s_lv2_init;                                                                \
    }                                                                            \
                                                                                 \
    extern "C" {                                                                 \
    LV2_SYMBOL_EXPORT                                                            \
    const LV2_Descriptor* lv2_descriptor(uint32_t index) {                       \
        return (index == 0) ? &pulp::format::lv2_generic::g_lv2_descriptor       \
                            : nullptr;                                           \
    }                                                                            \
    }

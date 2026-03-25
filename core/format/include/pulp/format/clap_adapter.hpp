#pragma once

// CLAP Adapter for Pulp
// Implements the CLAP plugin entry point wrapping pulp::format::Processor
// Built from CLAP specification headers (MIT license)

#include <pulp/format/processor.hpp>
#include <clap/clap.h>

namespace pulp::format::clap_adapter {

// CLAP plugin instance — wraps a Pulp Processor
struct PulpClapPlugin {
    clap_plugin_t plugin;
    std::unique_ptr<Processor> processor;
    state::StateStore store;
    ProcessorFactory factory;

    // Audio working state
    double sample_rate = 48000.0;
    int max_buffer_size = 512;
};

// CLAP entry point and factory
const clap_plugin_entry_t* get_clap_entry();

// Create a CLAP plugin descriptor from a Pulp PluginDescriptor
clap_plugin_descriptor_t make_clap_descriptor(const PluginDescriptor& desc);

// Plugin lifecycle callbacks
bool clap_init(const clap_plugin_t* plugin);
void clap_destroy(const clap_plugin_t* plugin);
bool clap_activate(const clap_plugin_t* plugin, double sr, uint32_t min_frames, uint32_t max_frames);
void clap_deactivate(const clap_plugin_t* plugin);
bool clap_start_processing(const clap_plugin_t* plugin);
void clap_stop_processing(const clap_plugin_t* plugin);
void clap_reset(const clap_plugin_t* plugin);
clap_process_status clap_process(const clap_plugin_t* plugin, const clap_process_t* process);
const void* clap_get_extension(const clap_plugin_t* plugin, const char* id);
void clap_on_main_thread(const clap_plugin_t* plugin);

} // namespace pulp::format::clap_adapter

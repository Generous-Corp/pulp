#pragma once

// Internal CLAP-slot factory: build a slot around a caller-created plugin
// instance instead of a bundle on disk.
//
// load_clap_plugin() dlopens a .clap, walks its factory, and creates the
// instance itself. That couples the slot's PluginSlot behavior — parameter
// caching, editor negotiation, bypass — to a real file, which is the wrong
// granularity for exercising the protocol against a plugin whose call sequence
// is under test.
//
// The creator callback receives the slot's own clap_host_t, exactly as the
// real factory path does, so a plugin created through it can call back into
// the host extensions (clap_host_gui and friends). The returned instance is
// owned by the slot: it is destroyed with the slot, matching load_clap_plugin.

#include <pulp/host/plugin_slot.hpp>

#include <clap/clap.h>

#include <functional>
#include <memory>

namespace pulp::host {

/// Creates a plugin instance bound to `host`. Return nullptr to fail the load.
/// `init()` is NOT called for you — return an instance that is ready to adopt.
using ClapPluginCreator = std::function<const clap_plugin_t*(const clap_host_t* host)>;

/// Build a CLAP slot around `create`'s instance. Returns nullptr when the
/// creator yields no plugin. No dlopen, no bundle, no entry point.
std::unique_ptr<PluginSlot> make_clap_slot(PluginInfo info, const ClapPluginCreator& create);

} // namespace pulp::host

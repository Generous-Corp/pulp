#pragma once

// AU v3 entry-point helper.
//
// Usage (in one .cpp file per AU v3 plugin):
//   #include "my_plugin.hpp"
//   #include <pulp/format/au_v3_entry.hpp>
//   PULP_AUV3_PLUGIN(my_namespace::create_my_plugin)
//
// The macro registers the plugin's processor factory at static init time so
// the shared `au_entry.mm` (compiled into every AU v3 .appex) can produce a
// `PulpAudioUnit` when the host calls `PulpAUFactory`. Symmetrical to
// `PULP_CLAP_PLUGIN` and `PULP_AU_INSTRUMENT`.

#include <pulp/format/registry.hpp>

#define PULP_AUV3_PLUGIN(factory_fn) PULP_REGISTER_PLUGIN(factory_fn)

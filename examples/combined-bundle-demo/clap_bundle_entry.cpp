// Combined-bundle demo — CLAP entry. Two plugins, one module entry.
// N × PULP_CLAP_BUNDLE_PLUGIN + exactly one PULP_CLAP_BUNDLE_ENTRY().

#include "bundle_plugins.hpp"
#include <pulp/format/clap_entry.hpp>

PULP_CLAP_BUNDLE_PLUGIN(Gain, pulp::examples::bundle::create_gain)
PULP_CLAP_BUNDLE_PLUGIN(Width, pulp::examples::bundle::create_width)
PULP_CLAP_BUNDLE_ENTRY()

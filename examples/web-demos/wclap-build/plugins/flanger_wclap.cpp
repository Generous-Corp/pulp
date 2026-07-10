// WebCLAP entry point for the Flanger demo.
//
// A ~4-line wrapper mirroring the WAM demo: include the plugin header, the
// standard CLAP entry, and the WebCLAP adapter, then emit the module via
// PULP_WCLAP_PLUGIN with the plugin's factory. id/name/vendor/version come
// from the Processor's descriptor(). Built by ../CMakeLists.txt (wasi-sdk).
#include "flanger.hpp"

#include <pulp/format/clap_entry.hpp>
#include <pulp/format/web/wclap_adapter.hpp>

PULP_WCLAP_PLUGIN(pulp::examples::classic::create_flanger)

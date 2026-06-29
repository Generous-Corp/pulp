// WebCLAP entry point for PulpGain.
//
// PULP_WCLAP_PLUGIN reuses the standard CLAP adapter/entry (PULP_CLAP_PLUGIN)
// and adds the malloc/free/cabi_realloc exports a WebCLAP host sandbox calls.
// The plugin id/name/vendor/version come from the Processor's descriptor().
//
// Built by examples/web-demos/wclap-build/CMakeLists.txt via pulp_add_wclap,
// which must be configured with the wasi-sdk toolchain
// (tools/cmake/wasi-toolchain.cmake).
#include "pulp_gain.hpp"

#include <pulp/format/clap_entry.hpp>
#include <pulp/format/web/wclap_adapter.hpp>

PULP_WCLAP_PLUGIN(pulp::examples::create_pulp_gain)

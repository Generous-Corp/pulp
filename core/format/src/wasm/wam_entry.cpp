// Shared WAMv2 C ABI entry point for Pulp WASM plugins (single processor).
//
// Every wam_* export is defined ONCE, in wam_entry_abi.inc, against the global
// bridge this file names via PULP_WAM_BRIDGE. That is what removed the ~150
// lines of init/process/param/midi/descriptor/state boilerplate this file used
// to copy-paste with wam_chain_entry.cpp. PulpWam.cmake compiles this TU into
// every single-plugin WAM build; each plugin supplies only the factory:
//
//     std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor();
//
// resolved at link time (exactly one definition per plugin executable). This is
// also why the WAMv2 state ABI is available to every plugin uniformly.
//
// The wam_* ABI must stay in sync with three OTHER lists (see PulpWam.cmake):
// the EXPORTED_FUNCTIONS allowlist, the makeBridge methods in wam-runtime.mjs,
// and the moduleExports adapter in wam-processor.js. The single-vs-rack drift
// the "four ABI sites" note warned about is now gone: both entries share
// wam_entry_abi.inc.

#include <pulp/format/web/wam_adapter.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Provided by each plugin's entry translation unit.
std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor();

namespace {
// ProcessorFactory is a plain function pointer; pulp_wam_make_processor decays
// into it. The factory is not invoked until wam_init().
pulp::format::wam::WamProcessorBridge g_bridge(pulp_wam_make_processor);
} // namespace

#define PULP_WAM_BRIDGE g_bridge
#include "wam_entry_abi.inc"
#undef PULP_WAM_BRIDGE

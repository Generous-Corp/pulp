// WebCLAP entry point for the SuperConvolver demo.
//
// Mirrors the WAM entry (super_convolver_wasm.cpp): the same headless processor
// behind the other ABI. PULP_WCLAP=1 PULP_WASM=1 PULP_HEADLESS=1 come from
// pulp_add_wclap, so the GPU engine, the file-backed IR loader, and the native
// editor are compiled out and the module is the CPU convolver against the
// built-in synthetic IR. id/name/vendor/version come from descriptor().
#include "super_convolver.hpp"

#include <pulp/format/clap_entry.hpp>
#include <pulp/format/web/wclap_adapter.hpp>

PULP_WCLAP_PLUGIN(pulp::examples::create_super_convolver)

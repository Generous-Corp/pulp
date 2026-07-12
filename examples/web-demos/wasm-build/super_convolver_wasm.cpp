// SuperConvolver WAMv2 factory. The wam_* C ABI lives in the shared
// core/format/src/wasm/wam_entry.cpp; this file only supplies the processor.
//
// The web build is headless (PULP_WASM=1 PULP_HEADLESS=1): the GPU engine, the
// file-backed IR loader, and the native editor are compiled out, so the module
// is the CPU PartitionedConvolver against the built-in synthetic IR — which is
// already SuperConvolver's default engine on the desktop.
#include "super_convolver.hpp"
#include <memory>

std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor() {
    return pulp::examples::create_super_convolver();
}

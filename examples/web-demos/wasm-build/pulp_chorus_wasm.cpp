// PulpChorus WAMv2 factory. The wam_* C ABI lives in the shared
// core/format/src/wasm/wam_entry.cpp; this file only supplies the processor.
#include "pulp_chorus.hpp"
#include <memory>

std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor() {
    return pulp::examples::create_pulp_chorus();
}

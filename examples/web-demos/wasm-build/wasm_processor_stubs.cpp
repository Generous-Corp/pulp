// Headless WASM DSP stubs for the Processor view/ARA factory defaults.
//
// The WAMv2 DSP module is headless: there is no Pulp view layer and no ARA SDK
// in the WebAssembly build. A plugin that does not override create_view() or
// create_ara_document_controller() still has those slots in its vtable, so the
// base definitions must be linked — but the real ones live in format.cpp (which
// pulls core/view -> canvas) and ara.cpp (which pulls the optional ARA SDK),
// neither of which belongs in a headless DSP module.
//
// processor.hpp only forward-declares both types, and both methods only ever
// return nullptr in this build, so we complete the types minimally and define
// the defaults here. This translation unit is the only one linked into the WASM
// module, so there is no ODR conflict with the real definitions used by native
// builds.
#include <pulp/format/processor.hpp>

namespace pulp::view { class View {}; }

namespace pulp::format {

class AraDocumentController {};

std::unique_ptr<view::View> Processor::create_view() { return nullptr; }

std::unique_ptr<AraDocumentController> Processor::create_ara_document_controller() {
    return nullptr;
}

} // namespace pulp::format

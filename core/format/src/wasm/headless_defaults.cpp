// Headless Processor view/ARA factory defaults for WebAssembly DSP builds.
//
// WHEN THIS IS LINKED: ONLY into WebAssembly (WAMv2/WebCLAP) DSP modules. It is
// NOT part of the native `pulp-format` library — core/format's CMakeLists does
// not compile the `wasm/` directory. A WASM DSP module is headless: there is no
// Pulp view layer (core/view -> canvas) and no ARA SDK.
//
// A plugin that does not override Processor::create_view() or
// create_ara_document_controller() still carries those slots in its vtable, so
// the base definitions must be linked. The real definitions live in format.cpp
// (pulls core/view -> canvas) and ara.cpp (pulls the optional ARA SDK), neither
// of which belongs in a headless DSP module.
//
// processor.hpp only forward-declares `pulp::view::View` and
// `pulp::format::AraDocumentController`, and both methods only ever return
// nullptr here. unique_ptr<T> needs T complete for its deleter, so we complete
// both minimally. This is sound because this TU is the ONLY one in the WASM
// link that defines these symbols — no native definition is in the same link,
// so there is no ODR conflict. Do NOT add this file to the native build, and do
// NOT link it alongside format.cpp / ara.cpp.
#include <pulp/format/processor.hpp>

namespace pulp::view { class View {}; }

namespace pulp::format {

class AraDocumentController {};

std::unique_ptr<view::View> Processor::create_view() { return nullptr; }

std::unique_ptr<AraDocumentController> Processor::create_ara_document_controller() {
    return nullptr;
}

} // namespace pulp::format

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
// and ara.cpp (the latter pulls the optional ARA SDK), neither of which belongs
// in a headless DSP module. Both are compiled into pulp-format-core, which the
// WASM lane does not link: PulpWclap.cmake and PulpWam.cmake compile the format
// sources they need directly.
//
// processor.hpp only forward-declares `pulp::view::View` and
// `pulp::format::AraDocumentController`, and both methods only ever return
// nullptr here. The minimal completions below give this TU its own definitions
// of both types; that is sound because this TU is the ONLY one in the WASM link
// that defines these symbols, so there is no ODR conflict. Do NOT add this file
// to the native build, and do NOT link it alongside pulp-format-core (which
// compiles format.cpp and ara.cpp, the native definitions of the same two
// methods).
//
// On completeness, measured rather than assumed: DEFINING one of these methods
// as `return nullptr;` does NOT require the type to be complete. The native
// pulp-format-core builds `Processor::create_view()` in format.cpp against
// nothing but processor.hpp's forward declaration, which is exactly what keeps
// that target free of the view layer. What DOES require completeness is
// CALLING such a method, because the caller destroys the returned
// unique_ptr and ~unique_ptr instantiates the deleter:
//
//   error: invalid application of 'sizeof' to an incomplete type 'pulp::view::View'
//
// That is a real diagnostic, from a consumer that linked pulp-format-core alone
// and tried to call create_view() (test/fixtures/format_core_only_consumer).
// So "unique_ptr needs a complete type" is true at the destruction site, not at
// the definition. The completions here are kept because this TU stands in for
// the whole view and ARA layers in a link that has neither, not because
// returning nullptr would otherwise fail to compile.
#include <pulp/format/processor.hpp>

namespace pulp::view { class View {}; }

namespace pulp::format {

class AraDocumentController {};

std::unique_ptr<view::View> Processor::create_view() { return nullptr; }

std::unique_ptr<AraDocumentController> Processor::create_ara_document_controller() {
    return nullptr;
}

} // namespace pulp::format

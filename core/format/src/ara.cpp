#include <pulp/format/ara.hpp>
#include <pulp/format/processor.hpp>

#ifdef PULP_HAS_ARA
#include <ARA_API/ARAInterface.h>
#endif

namespace pulp::format {

bool host_supports_ara() {
#ifdef PULP_HAS_ARA
    // SDK is compiled in; actual host-query lives in the format-adapter
    // companion factories (VST3 / AU / CLAP). Pulp-side availability is
    // gated by those slices landing — workstream 06 slices 6.3..6.5.
    return false;
#else
    return false;
#endif
}

bool ara_sdk_compiled_in() {
#ifdef PULP_HAS_ARA
    return true;
#else
    return false;
#endif
}

int ara_sdk_generation() {
#ifdef PULP_HAS_ARA
    // Newest generation defined by the headers. Callers compare to a
    // concrete constant like kARAAPIGeneration_2_3_Final (= 6) when
    // gating feature use. Touching the ARA::kARAAPIGeneration_2_3_Final
    // symbol here proves the SDK headers are reachable at compile time.
    return static_cast<int>(ARA::kARAAPIGeneration_2_3_Final);
#else
    return 0;
#endif
}

// Out-of-line default so processor.hpp can forward-declare
// AraDocumentController without forcing every TU to include ara.hpp.
// Defining it here ensures the unique_ptr deleter is instantiated
// in a TU where the class is complete.
std::unique_ptr<AraDocumentController>
Processor::create_ara_document_controller() { return nullptr; }

#ifndef PULP_HAS_ARA
// Without the SDK the factory must return nullptr; hosts will treat
// the plug-in as non-ARA. When PULP_HAS_ARA is on, ara_factory.cpp
// owns the real implementation and this definition is suppressed.
const void* ara_companion_factory_for(AraDocumentController* /*controller*/) {
    return nullptr;
}
#endif

}  // namespace pulp::format


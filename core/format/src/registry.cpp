#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>

namespace pulp::format {

static ProcessorFactory g_factory = nullptr;

ProcessorFactory& registered_factory() {
    return g_factory;
}

void register_plugin(ProcessorFactory factory) {
    g_factory = factory;
    // Don't test-create at static init time — std::string and other
    // globals may not be initialized yet when AU bundles are loaded.
}

} // namespace pulp::format

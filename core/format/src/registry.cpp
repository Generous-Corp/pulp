#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>

namespace pulp::format {

static ProcessorFactory g_factory = nullptr;

ProcessorFactory& registered_factory() {
    return g_factory;
}

void register_plugin(ProcessorFactory factory) {
    g_factory = factory;
    if (factory) {
        // Verify the factory works by creating a temporary instance
        auto test = factory();
        if (test) {
            runtime::log_info("Plugin registered: {}", test->descriptor().name);
        }
    }
}

} // namespace pulp::format

#include <pulp/format/registry.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>

namespace {
std::unique_ptr<pulp::format::Processor> no_processor() { return {}; }
}

int main() {
    pulp::format::StandaloneApp app(no_processor);
    return pulp::format::registered_plugins().empty() ? 0 : 1;
}

#include <pulp/format/registry.hpp>

int main() {
    return pulp::format::registered_plugins().empty() ? 0 : 1;
}

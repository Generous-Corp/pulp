#include <pulp/timeline/dawproject_import.hpp>

int main() {
    const auto project =
        pulp::timeline::import_dawproject_xml(R"(<Project version="1.0"/>)");
    return project ? 0 : 1;
}

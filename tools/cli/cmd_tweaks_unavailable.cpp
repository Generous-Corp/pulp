#include <iostream>
#include <string>
#include <vector>

int cmd_tweaks(const std::vector<std::string>&) {
    std::cerr
        << "Error: this Pulp SDK was built without the optional development "
           "inspector component (PULP_ENABLE_INSPECTOR=OFF)\n";
    return 1;
}

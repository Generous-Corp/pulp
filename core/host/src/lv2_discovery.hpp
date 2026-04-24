#pragma once

#include <string>
#include <vector>

namespace pulp::host::detail {

struct Lv2PortRole {
    int index = -1;
    bool is_audio = false;
    bool is_control = false;
    bool is_input = false;
    std::string name;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
};

std::vector<Lv2PortRole> discover_lv2_ports(const std::string& bundle_dir);
std::string resolve_lv2_binary(const std::string& bundle_dir);

} // namespace pulp::host::detail

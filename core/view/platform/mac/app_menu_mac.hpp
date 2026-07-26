#pragma once

#include <pulp/view/window_host.hpp>

#include <vector>

namespace pulp::view::mac_menu {

void append_commands(const std::vector<WindowOptions::MenuCommand>& commands);

} // namespace pulp::view::mac_menu

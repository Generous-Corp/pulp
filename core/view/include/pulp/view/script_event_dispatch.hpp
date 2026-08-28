#pragma once

#include <cstdint>

namespace pulp::view {
class View;
}

namespace pulp::view::script_events {

using GlobalKeyDispatcher = void (*)(int key_code, uint16_t modifiers, bool is_down);
using RootKeyDispatcher = bool (*)(View& root, int key_code,
                                   uint16_t modifiers, bool is_down);

void set_global_key_dispatcher(GlobalKeyDispatcher dispatcher) noexcept;
void dispatch_global_key(int key_code, uint16_t modifiers, bool is_down);
void set_root_key_dispatcher(RootKeyDispatcher dispatcher) noexcept;
bool dispatch_key_for_root(View& root, int key_code,
                           uint16_t modifiers, bool is_down);

}  // namespace pulp::view::script_events

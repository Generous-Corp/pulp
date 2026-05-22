#pragma once

#include <cstdint>

namespace pulp::view::script_events {

using GlobalKeyDispatcher = void (*)(int key_code, uint16_t modifiers, bool is_down);

void set_global_key_dispatcher(GlobalKeyDispatcher dispatcher) noexcept;
void dispatch_global_key(int key_code, uint16_t modifiers, bool is_down);

}  // namespace pulp::view::script_events

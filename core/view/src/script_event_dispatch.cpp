#include <pulp/view/script_event_dispatch.hpp>

#include <atomic>

namespace pulp::view::script_events {
namespace {

std::atomic<GlobalKeyDispatcher>& global_key_dispatcher_slot() {
    static std::atomic<GlobalKeyDispatcher> dispatcher{nullptr};
    return dispatcher;
}

std::atomic<RootKeyDispatcher>& root_key_dispatcher_slot() {
    static std::atomic<RootKeyDispatcher> dispatcher{nullptr};
    return dispatcher;
}

}  // namespace

void set_global_key_dispatcher(GlobalKeyDispatcher dispatcher) noexcept {
    global_key_dispatcher_slot().store(dispatcher, std::memory_order_release);
}

void dispatch_global_key(int key_code, uint16_t modifiers, bool is_down) {
    auto* dispatcher = global_key_dispatcher_slot().load(std::memory_order_acquire);
    if (dispatcher) dispatcher(key_code, modifiers, is_down);
}

void set_root_key_dispatcher(RootKeyDispatcher dispatcher) noexcept {
    root_key_dispatcher_slot().store(dispatcher, std::memory_order_release);
}

bool dispatch_key_for_root(View& root, int key_code,
                           uint16_t modifiers, bool is_down) {
    auto* dispatcher = root_key_dispatcher_slot().load(std::memory_order_acquire);
    return dispatcher && dispatcher(root, key_code, modifiers, is_down);
}

}  // namespace pulp::view::script_events

// Prints the vtable position of Processor's virtuals that follow its legacy
// view slots, plus sizeof(Processor) and the hot-reload fingerprint's ABI
// flags. The format-legacy-view-slot-layout test builds this once per value
// of PULP_FORMAT_LEGACY_VIEW and once against a processor.hpp with one slot
// removed, then compares the outputs: the two modes must agree, and the
// shifted header must not.
//
// A pointer to a virtual member function holds its vtable position on the
// Itanium C++ ABI (offset + 1, or the offset with the flag in the adjustment
// on ARM). Both words are printed, so either encoding compares exactly.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/build_fingerprint.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using pulp::format::Processor;

template <typename Member> void print_slot(const char* name, Member member) {
    static_assert(sizeof(member) == 2 * sizeof(std::uintptr_t),
                  "expects the two-word Itanium member function pointer");
    std::uintptr_t words[2] = {};
    std::memcpy(words, &member, sizeof words);
    std::printf("slot %s %llu %llu\n", name, static_cast<unsigned long long>(words[0]),
                static_cast<unsigned long long>(words[1]));
}

} // namespace

int main() {
    print_slot("has_editor", &Processor::has_editor);
    print_slot("on_host_transport_changed", &Processor::on_host_transport_changed);
    print_slot("estimate_prepare_resources", &Processor::estimate_prepare_resources);
    print_slot("supports_in_place_scripted_ui_reload",
               &Processor::supports_in_place_scripted_ui_reload);
    print_slot("editor_factory", &Processor::editor_factory);
    print_slot("plugin_state_carries_parameters", &Processor::plugin_state_carries_parameters);
    std::printf("size %zu\n", sizeof(Processor));
    std::printf("abi_flags %s\n", pulp::format::reload::current_build_fingerprint().abi_flags);
    return 0;
}

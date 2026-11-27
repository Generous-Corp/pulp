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

#if !PULP_FORMAT_LEGACY_VIEW
// The probe links no pulp-format-core; it stands in for one built with the
// macro 0, which defines this marker (processor.hpp).
extern "C" const char pulp_format_core_was_built_with_PULP_FORMAT_LEGACY_VIEW_0 = 0;
#endif

namespace {

using pulp::format::Processor;

// The first virtual a subclass adds takes the slot after Processor's last
// one, so its position is Processor's whole vtable length: a virtual one mode
// adds anywhere, the end included, moves it.
struct EndOfProcessorVtable : Processor {
    virtual void end_of_processor_vtable();
};

// The legacy settings tab type is Processor's public API with Pulp's view
// layer and private without it, where it only fills the reserved slot.
template <class P>
concept PublicSettingsSection = requires { typename P::SettingsSection; };
static_assert(PublicSettingsSection<Processor> == bool(PULP_FORMAT_LEGACY_VIEW),
              "Processor::SettingsSection is public exactly when Pulp's view layer is");

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
    print_slot("end_of_vtable", &EndOfProcessorVtable::end_of_processor_vtable);
    std::printf("size %zu\n", sizeof(Processor));
    std::printf("abi_flags %s\n", pulp::format::reload::current_build_fingerprint().abi_flags);
    return 0;
}

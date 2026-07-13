#include <pulp/format/registry.hpp>

#include <cstring>

namespace pulp::format {

namespace {

// Fixed-capacity, static-init-safe storage. No heap, no std::string, no
// logging on this path — registration runs before the C++ runtime's globals
// are reliably up (AU bundles load registrars very early).
// Generous headroom: a combined bundle registers one entry per plugin PER
// format-specific AU type (a Silent Way-style module is aumf + aumu + augn = 3),
// and VST3/CLAP bundle entries add more. 11 plugins × 3 ≈ 33 today; 256 leaves
// room to grow without the silent past-cap drop in register_plugin ever biting.
constexpr std::size_t kMaxPlugins = 256;
PluginRegistration g_entries[kMaxPlugins];
std::size_t g_count = 0;

ProcessorFactory g_factory = nullptr;

} // namespace

ProcessorFactory& registered_factory() {
    return g_factory;
}

void register_plugin(ProcessorFactory factory) {
    // Legacy single-plugin path: set the one global slot, last write wins.
    // Byte-identical to the pre-bundle behavior, so the save/restore + swap
    // idiom used by adapter tests keeps working. This path deliberately does
    // NOT append to the keyed table — `registered_plugins()` reflects only
    // multi-plugin-bundle (keyed) registrations. A pure bundle never calls this
    // overload, so its global slot stays null by construction and any stray
    // global read fails loudly instead of picking an arbitrary plugin.
    g_factory = factory;
}

void register_plugin(const PluginRegistration& reg) {
    // Keyed path — never touches the legacy global slot.
    if (g_count < kMaxPlugins) {
        g_entries[g_count++] = reg;
    }
}

const PluginRegistration* find_plugin(const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (std::size_t i = 0; i < g_count; ++i) {
        if (g_entries[i].id != nullptr && std::strcmp(g_entries[i].id, id) == 0) {
            return &g_entries[i];
        }
    }
    return nullptr;
}

std::span<const PluginRegistration> registered_plugins() {
    return std::span<const PluginRegistration>(g_entries, g_count);
}

void reset_registry_for_testing() {
    for (std::size_t i = 0; i < g_count; ++i) {
        g_entries[i] = PluginRegistration{};
    }
    g_count = 0;
    g_factory = nullptr;
}

} // namespace pulp::format

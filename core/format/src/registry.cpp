#include <pulp/format/registry.hpp>

#include <atomic>
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
// Publication count. Writes are single-writer — either C++ static init (AU/VST3
// registrars) or a format module's init entry point (CLAP's entry_init), which
// the host serializes and sequences before it uses the module. A release store
// here pairs with the acquire loads in find_plugin/registered_plugins so a
// reader on another thread sees the fully-written entry, giving the runtime
// (non-static-init) publication path a proper happens-before edge.
std::atomic<std::size_t> g_count{0};

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
    // Keyed path — never touches the legacy global slot. Write the entry, then
    // publish it with a release store so an acquiring reader sees a fully
    // constructed entry (see g_count's note on the single-writer contract).
    const std::size_t n = g_count.load(std::memory_order_relaxed);
    if (n < kMaxPlugins) {
        g_entries[n] = reg;
        g_count.store(n + 1, std::memory_order_release);
    }
}

const PluginRegistration* find_plugin(const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    const std::size_t n = g_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (g_entries[i].id != nullptr && std::strcmp(g_entries[i].id, id) == 0) {
            return &g_entries[i];
        }
    }
    return nullptr;
}

std::span<const PluginRegistration> registered_plugins() {
    return std::span<const PluginRegistration>(
        g_entries, g_count.load(std::memory_order_acquire));
}

void reset_registry_for_testing() {
    const std::size_t n = g_count.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < n; ++i) {
        g_entries[i] = PluginRegistration{};
    }
    g_count.store(0, std::memory_order_release);
    g_factory = nullptr;
}

} // namespace pulp::format

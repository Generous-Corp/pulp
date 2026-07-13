#pragma once

// Plugin registry — connects format adapters to processor factories.
//
// Two registration modes coexist:
//
//   * Single-plugin bundle (the default, unchanged): one binary hosts one
//     plugin. `PULP_REGISTER_PLUGIN(factory)` sets a single global slot that
//     `registered_factory()` returns; the format adapters read it at
//     construction.
//
//   * Multi-plugin bundle (Silent Way style): one binary hosts many plugins,
//     each exposed as its own component/class. Entries are registered with a
//     `PluginRegistration` (keyed by reverse-DNS id) and the per-component
//     entry macros bind each component to its OWN factory lexically, so no
//     global lookup happens on any instantiation path. The legacy global slot
//     is left null in this mode (a stray global read fails loudly rather than
//     silently building the wrong processor).
//
// Registration runs at static-init time, before the C++ runtime's own globals
// are reliably constructed (AU bundles load registrars very early). Storage is
// therefore a fixed-capacity array of trivially-copyable POD — no heap, no
// std::string, and no logging on the registration path.

#include <pulp/format/processor.hpp>

#include <cstdint>
#include <span>

namespace pulp::format {

/// One registered plugin. POD by design so it can be assigned at static init.
/// `id` is the reverse-DNS bundle id (== `PluginDescriptor::bundle_id`); it is
/// null for legacy single-plugin registrations that predate keyed lookup.
struct PluginRegistration {
    const char* id = nullptr;
    ProcessorFactory factory = nullptr;

    // AU 4-char codes, 0 when not applicable. One plugin may be registered
    // under multiple AU types (aumf + aumu + augn) — one entry per type, all
    // sharing the same factory.
    std::uint32_t au_type = 0;
    std::uint32_t au_subtype = 0;
    std::uint32_t au_manufacturer = 0;

    // Per-plugin editor assets for multi-plugin bundles, where the binary-wide
    // PULP_UI_* compile defines cannot distinguish plugins. Null = fall back to
    // the compile-time defines (the single-plugin path).
    const char* ui_script = nullptr;
    const char* ui_theme = nullptr;
    const char* ui_asset_roots = nullptr;
};

// The legacy single global factory. Set once per single-plugin binary; returns
// null in a multi-plugin bundle (see file header). Thread-safe: set at static
// init, read at runtime.
ProcessorFactory& registered_factory();

// Legacy single-plugin registration — sets the one global slot (last write
// wins), exactly as before keyed registration existed. Multi-plugin bundles do
// not use this overload: they register keyed entries and bind each component to
// its own factory lexically, so their global slot stays null by construction.
// (Use `PULP_REGISTER_PLUGIN` or call directly from a plugin entry.)
void register_plugin(ProcessorFactory factory);

// Keyed registration for multi-plugin bundles. Does not touch the global slot.
void register_plugin(const PluginRegistration& reg);

// Look up a registered plugin by its reverse-DNS id. Null if not found.
const PluginRegistration* find_plugin(const char* id);

// All registered plugins, in registration order. Valid after static init.
std::span<const PluginRegistration> registered_plugins();

// Test-only: clear all registrations and the legacy global slot back to the
// pristine static-init state. NOT for production use — registration is meant to
// happen once at static init and never be torn down. Exists so a single test
// binary can exercise the register/lookup/ambiguity logic deterministically
// across multiple cases without one case's registrations leaking into the next.
void reset_registry_for_testing();

} // namespace pulp::format

// Macro for plugin developers — place in ONE .cpp file per single-plugin bundle.
#define PULP_REGISTER_PLUGIN(factory_fn) \
    namespace { \
        struct PulpPluginRegistrar { \
            PulpPluginRegistrar() { ::pulp::format::register_plugin(factory_fn); } \
        } _pulp_registrar; \
    }

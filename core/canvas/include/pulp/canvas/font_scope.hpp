// font_scope.hpp
//
// Pulp #2163 follow-up — Phase 1 / Slice 1.1.a-b of the font-subsystem-
// hardening v2 roadmap.
//
// A `FontScope` is a named owner of font registrations. Three built-in
// scopes exist:
//
//   * `Scope::Global` — what `register_font()` writes today; behaves
//     identically for back-compat.
//   * `Scope::Plugin(id)` — per-plugin registrations. Two plugins loaded
//     into the same host see *only* their own registrations plus the
//     Global scope; they cannot pollute each other's resolution.
//   * `Scope::View(id)` — per-view overrides. Used by design-import hot
//     reload (Phase 2) and by `pulp-ui-preview --font` flags.
//
// Resolution (in `FontResolver::resolve_*`) walks scopes in this order:
//
//     View → Plugin → Global → Bundled → Platform
//
// Each scope tracks its own monotonic generation counter; the resolver
// merges them (`merged_generation_for(FontScopeId)`) into the
// `registry_generation` field of `FontOptions` so every downstream cache
// key is correctly invalidated when *any* applicable scope mutates.
//
// Phase 1 / Slice 1.1.b acceptance: a generation bump in Plugin(A) must
// not dirty measure callbacks belonging to Plugin(B). This is enforced
// by `merged_generation_for(FontScopeId)` returning a value that only
// changes when scopes the caller actually consults change.

#pragma once

#include "pulp/canvas/font_options.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pulp::canvas {

class FontScope {
public:
    explicit FontScope(FontScopeId id);
    ~FontScope();

    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;

    FontScopeId id() const noexcept { return id_; }

    /// Monotonic counter; incremented by every successful registration
    /// in this scope. Never resets across process lifetime.
    std::uint64_t generation() const noexcept;

    /// Forces a generation bump without a registration. Useful for
    /// integration tests and for scope-level "I know the underlying
    /// platform fonts changed" signals (e.g. macOS font activation).
    void bump_generation();

    /// Register a font from raw TTF/OTF bytes into THIS scope. Returns
    /// `true` if Skia accepted the bytes and the registration was
    /// stored. Idempotent on `family_override`.
    bool register_font(const std::uint8_t* data, std::size_t size,
                       const std::string& family_override = "");

    /// File variant.
    bool register_font_file(const std::string& path,
                            const std::string& family_override = "");

    /// Family-name lookup limited to *this* scope. Does not walk other
    /// scopes; that's the resolver's job.
    bool is_registered(const std::string& family) const;

    /// pulp #2163 — Phase 2 / Slice 2.7 implementation. Memory budget
    /// (in bytes) for caches owned by this scope: FontResolver
    /// typeface cache, TextShaper segment cache, Skia strike cache
    /// pressure. `0` (default) disables the budget. Setting a non-zero
    /// budget triggers an immediate `prune_to_budget()`.
    void set_memory_budget(std::size_t bytes);
    std::size_t memory_budget() const noexcept;

    /// Explicit prune-to-budget trigger. Called automatically by
    /// `set_memory_budget`; exposed so memory-pressure handlers can
    /// request a manual prune.
    void prune_to_budget();

private:
    FontScopeId id_;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::size_t> memory_budget_{0};

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Built-in scope accessors ─────────────────────────────────────────────

/// Process-wide global scope. `register_font(...)` (the free function in
/// `bundled_fonts.hpp`) writes here.
FontScope& global_scope();

/// Look up (or lazily create) the plugin scope for `plugin_id`. Plugin
/// IDs are opaque integers; the host assigns them at load time.
FontScope& plugin_scope(std::uint64_t plugin_id);

/// Look up (or lazily create) the view scope for `view_id`. View IDs are
/// opaque integers; the view layer assigns them. View scopes are short-
/// lived; call `release_view_scope(view_id)` when the view is destroyed
/// to avoid unbounded growth.
FontScope& view_scope(std::uint64_t view_id);

/// Tear down a view scope. After this call, future `view_scope(view_id)`
/// returns a fresh scope (generation reset to 0). The caller must
/// guarantee no resolver caches hold `ResolvedFont` values keyed on the
/// torn-down scope.
void release_view_scope(std::uint64_t view_id);

// ── Generation merging ───────────────────────────────────────────────────

/// Returns a generation value that is the combined monotonic counter of
/// every scope a resolver looking up `requesting_scope` would consult.
/// Used by downstream caches as part of their cache key.
///
/// For `requesting_scope.kind == Global`: returns `global_scope().generation()`.
/// For `Plugin(id)`: returns a value combining `plugin_scope(id).generation()`
///   and `global_scope().generation()`.
/// For `View(id)`: combines `view_scope(id)`, the active plugin scope
///   (if any), and global.
///
/// The returned value is monotonic — once it increases, it never goes
/// down. Two consecutive calls with the same input may return different
/// values if any consulted scope was bumped between them; that is the
/// signal downstream caches use to evict stale entries.
std::uint64_t merged_generation_for(FontScopeId requesting_scope);

} // namespace pulp::canvas

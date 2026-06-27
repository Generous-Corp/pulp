// font_scope.cpp
//
// Font scopes track a monotonic generation counter and the family names
// registered through each scope. The actual Skia typeface storage still
// lives in `bundled_fonts.cpp` through the global registry path; this
// file provides the named-scope seam used by callers and cache keys
// while preserving back-compat with existing global registrations.

#include "pulp/canvas/font_scope.hpp"
#include "pulp/canvas/bundled_fonts.hpp"
#include "pulp/canvas/font_resolver.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pulp::canvas {

// ── FontScope::Impl ──────────────────────────────────────────────────────

struct FontScope::Impl {
    mutable std::mutex mtx;
    std::unordered_set<std::string> registered_families;
};

FontScope::FontScope(FontScopeId id)
    : id_(id), impl_(std::make_unique<Impl>()) {}

FontScope::~FontScope() = default;

std::uint64_t FontScope::generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
}

void FontScope::bump_generation() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

bool FontScope::register_font(const std::uint8_t* data, std::size_t size,
                              const std::string& family_override) {
    // All scopes use the global typeface path so the font is loadable,
    // then record the family name in this scope. Typeface storage is
    // not scope-isolated yet.
    bool ok = false;
    if (id_.kind == FontScopeId::Kind::Global) {
        ok = ::pulp::canvas::register_font(data, size, family_override);
    } else {
        // Invoke the global path so the typeface is loadable, then
        // record the family name in this scope.
        ok = ::pulp::canvas::register_font(data, size, family_override);
    }
    if (ok && !family_override.empty()) {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->registered_families.insert(family_override);
    }
    if (ok) bump_generation();
    return ok;
}

bool FontScope::register_font_file(const std::string& path,
                                   const std::string& family_override) {
    bool ok = ::pulp::canvas::register_font_file(path, family_override);
    if (ok && !family_override.empty()) {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->registered_families.insert(family_override);
    }
    if (ok) bump_generation();
    return ok;
}

bool FontScope::is_registered(const std::string& family) const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->registered_families.find(family) != impl_->registered_families.end();
}

// Memory budget for this scope's font caches. A non-zero budget is a
// hint, not a hard cap. The only eviction performed today happens when
// set_memory_budget() runs: prune_to_budget() then clears the entire
// FontResolver cache (see below). register_*() does not prune. Per-entry
// accounting and the TextShaper / Skia strike caches are not yet
// factored in. A budget of 0 disables eviction.
void FontScope::set_memory_budget(std::size_t bytes) {
    memory_budget_.store(bytes, std::memory_order_release);
    prune_to_budget();
}

std::size_t FontScope::memory_budget() const noexcept {
    return memory_budget_.load(std::memory_order_acquire);
}

void FontScope::prune_to_budget() {
    const std::size_t budget = memory_budget_.load(std::memory_order_acquire);
    if (budget == 0) return;  // 0 disables the budget.

    // Resolver cache eviction goes through clear_cache() because the
    // resolver does not yet expose per-scope partial eviction.
    // Cost: full cache rebuild on next lookup; cheap because cascade is
    // small and already-cached at the Skia level.
    FontResolver::instance().clear_cache();
}

// ── Built-in scope registry ──────────────────────────────────────────────

namespace {

std::mutex& scope_table_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::uint64_t, std::unique_ptr<FontScope>>& plugin_scopes() {
    static std::unordered_map<std::uint64_t, std::unique_ptr<FontScope>> m;
    return m;
}

std::unordered_map<std::uint64_t, std::unique_ptr<FontScope>>& view_scopes() {
    static std::unordered_map<std::uint64_t, std::unique_ptr<FontScope>> m;
    return m;
}

} // namespace

FontScope& global_scope() {
    static FontScope inst{FontScopeId::global()};
    return inst;
}

FontScope& plugin_scope(std::uint64_t plugin_id) {
    std::lock_guard<std::mutex> lock(scope_table_mutex());
    auto& tbl = plugin_scopes();
    auto it = tbl.find(plugin_id);
    if (it == tbl.end()) {
        it = tbl.emplace(plugin_id,
                         std::make_unique<FontScope>(
                             FontScopeId::plugin(plugin_id))).first;
    }
    return *it->second;
}

FontScope& view_scope(std::uint64_t view_id) {
    std::lock_guard<std::mutex> lock(scope_table_mutex());
    auto& tbl = view_scopes();
    auto it = tbl.find(view_id);
    if (it == tbl.end()) {
        it = tbl.emplace(view_id,
                         std::make_unique<FontScope>(
                             FontScopeId::view(view_id))).first;
    }
    return *it->second;
}

void release_view_scope(std::uint64_t view_id) {
    std::lock_guard<std::mutex> lock(scope_table_mutex());
    view_scopes().erase(view_id);
}

std::uint64_t merged_generation_for(FontScopeId requesting) {
    // Always consult the global scope. Plugin/view requests additionally
    // mix in their own scope's generation. The merge is a plain 64-bit sum;
    // wraparound is unreachable in practice for these generation counters.
    std::uint64_t total = global_scope().generation();
    if (requesting.kind == FontScopeId::Kind::Plugin) {
        total += plugin_scope(requesting.id).generation();
    } else if (requesting.kind == FontScopeId::Kind::View) {
        total += view_scope(requesting.id).generation();
        // View scopes do not yet remember their owning plugin. Until
        // that link exists, view generation changes only observe the
        // view and global scopes.
    }
    return total;
}

} // namespace pulp::canvas

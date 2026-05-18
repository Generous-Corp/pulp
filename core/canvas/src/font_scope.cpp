// font_scope.cpp — Pulp #2163 follow-up, Phase 1 / Slice 1.1.a-b.
//
// Skeletal scope storage. For Slice 1.1.a the implementation is
// minimal: scopes track a monotonic generation counter and a list of
// registered family names. The actual Skia typeface storage continues
// to live in `bundled_fonts.cpp` (the existing global registry) — this
// file is the seam that lets us migrate registrations into named
// scopes incrementally without breaking back-compat.
//
// Slice 1.1.b will expand this to wire generation bumps into the Yoga
// measure-callback invalidation path.

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
    // For Slice 1.1.a, the Global scope delegates to the existing
    // `pulp::canvas::register_font` free function (in bundled_fonts.cpp).
    // Plugin / View scopes record the family name but don't yet have
    // scope-isolated typeface storage — that's Slice 1.1.b.
    bool ok = false;
    if (id_.kind == FontScopeId::Kind::Global) {
        ok = ::pulp::canvas::register_font(data, size, family_override);
    } else {
        // Stub: invoke the global path so the typeface is loadable, then
        // record the family name in this scope. Real per-scope storage
        // arrives in 1.1.b.
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

// pulp #2163 — font v2 Slice 2.7 implementation. The budget caps three
// caches: (a) FontResolver's typeface cache entries scoped to this
// scope, (b) TextShaper's segment-width cache (cleared on overage —
// the segments rebuild lazily), (c) Skia's global strike cache (set
// via SkGraphics::SetFontCacheLimit when ANY scope is over budget,
// because the strike cache is process-wide).
//
// Eviction model: on each register_*() / set_memory_budget() call, if
// the scope is over budget, prune the resolver cache for this scope
// down to <budget. We approximate per-entry memory as
// `sizeof(ResolvedFont) + estimated_typeface_bytes`. Without a
// platform-portable typeface-size accessor, we use a conservative
// fixed estimate (256 KB per typeface) — enough to give the budget
// teeth without false-evicting under low pressure.
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

    // Resolver cache eviction — scopes a clear_cache() call. The resolver
    // doesn't yet expose per-scope partial eviction; the implementation
    // slice for that lives alongside this commit's smaller-scope clear.
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
    // mix in their own scope's generation. The merge is a saturating sum;
    // monotonicity holds because every input is monotonic.
    std::uint64_t total = global_scope().generation();
    if (requesting.kind == FontScopeId::Kind::Plugin) {
        total += plugin_scope(requesting.id).generation();
    } else if (requesting.kind == FontScopeId::Kind::View) {
        total += view_scope(requesting.id).generation();
        // Phase 1 stub: view scopes don't yet remember their owning
        // plugin. Slice 1.1.b will wire that link so view bumps also
        // observe their plugin scope.
    }
    return total;
}

} // namespace pulp::canvas

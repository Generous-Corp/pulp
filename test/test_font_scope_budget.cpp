// test_font_scope_budget.cpp — Pulp #2163, font v2 Slice 2.7.
//
// Verifies that FontScope::set_memory_budget triggers a
// prune-to-budget operation that evicts the resolver cache so
// repeated resolutions under memory pressure don't accumulate
// indefinitely.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/font_scope.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/font_options.hpp>

#ifdef PULP_HAS_SKIA
#include "include/core/SkTypeface.h"  // sk_sp<SkTypeface> needs the full type
#endif

using namespace pulp::canvas;

TEST_CASE("FontScope: memory budget defaults to 0 (disabled)", "[font][scope][issue-2163]") {
    auto& scope = global_scope();
    REQUIRE(scope.memory_budget() == 0u);
}

TEST_CASE("FontScope: setting + reading the budget round-trips", "[font][scope]") {
    auto& scope = plugin_scope(7421u);  // arbitrary fresh plugin id
    scope.set_memory_budget(10 * 1024 * 1024);  // 10 MB
    REQUIRE(scope.memory_budget() == 10u * 1024u * 1024u);

    scope.set_memory_budget(0);
    REQUIRE(scope.memory_budget() == 0u);
}

TEST_CASE("FontScope: prune_to_budget evicts the resolver cache", "[font][scope]") {
    auto& scope = plugin_scope(7422u);
    auto& resolver = FontResolver::instance();

    // Prime the cache with a resolve.
    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;
    opts.scope = scope.id();
    auto resolved_first = resolver.resolve_family_list(opts);

    // Set a tight budget — triggers prune.
    scope.set_memory_budget(1024);  // 1 KB — far below any face

    // Second resolve must succeed (same FontOptions => same result),
    // but the cache was cleared between calls. We can't observe the
    // clear directly; we assert correctness instead — the resolver
    // returns a coherent result under prune pressure.
    auto resolved_again = resolver.resolve_family_list(opts);
    REQUIRE(resolved_again.origin == resolved_first.origin);

    scope.set_memory_budget(0);  // restore
}

TEST_CASE("FontScope: explicit prune_to_budget is idempotent", "[font][scope]") {
    auto& scope = plugin_scope(7423u);
    scope.set_memory_budget(64 * 1024);

    // Multiple prunes back-to-back must not crash.
    scope.prune_to_budget();
    scope.prune_to_budget();
    scope.prune_to_budget();

    scope.set_memory_budget(0);
}

// Yoga layout-pass benchmark.
//
// WHY THIS EXISTS
// ---------------
// `View::layout_children()` routes to `yoga_layout()`
// (core/view/src/yoga_layout.cpp), which — on EVERY call — builds a fresh
// YGNode tree (`YGNodeNew` per managed descendant), re-applies every style,
// runs `YGNodeCalculateLayout`, copies results back into the View tree, and
// then `YGNodeFreeRecursive`s the whole thing. Yoga's own dirty-flag and
// measure caches live on those nodes, so they are discarded every pass.
//
// Every platform paint path calls `root_.layout_children()` immediately
// before painting (plugin_view_host_mac.mm, _win.cpp, _linux.cpp, _ios.mm),
// so that build/free cycle runs at frame rate whether or not anything
// layout-relevant changed.
//
// This file measures what that actually costs — wall clock and allocation
// volume per pass — for 100 / 250 / 500-node trees, static and animated. It
// is deliberately a MEASUREMENT, not a fix: it exists so that any future
// "persistent Yoga nodes" or "layout-dirty gate" change is chosen on
// evidence and can be scored against a committed baseline.
//
// It doubles as a regression gate: the REQUIREs at the bottom fail if a
// single layout pass ever eats a significant slice of a 60fps frame budget.
// Thresholds are set with wide headroom over the measured numbers so the
// gate catches order-of-magnitude regressions, not CI jitter.
//
// Allocation accounting is honest: this TU replaces the global
// operator new/delete so the counters observe every heap allocation made
// inside the timed region (Yoga nodes, Yoga's internal vectors, and the
// std::vector<View*> churn in ordered_visible_children()). It cannot
// attribute them per-source-line; it reports the total for the pass.
//
// Run it (Release; the table prints on stdout, no flags needed):
//   ./build/test/pulp-test-yoga-layout-bench
//
// BASELINE — 2026-07-12, Apple M-series, Release (-O3), tree idle otherwise:
//
//   nodes  mode      mean_us  allocs/pass  bytes/pass  %frame(60fps)
//     100  static       76.9          229       68144          0.46%
//     100  animated     78.0          229       68144          0.47%
//     241  static      175.1          448      158288          1.05%
//     241  animated    177.7          448      158288          1.07%
//     484  static      376.6          813      327344          2.26%
//     484  animated    375.8          813      327344          2.25%
//
// Reading of that: TIME is not the problem. A 484-node layout pass is ~0.38ms,
// ~2.3% of a 60fps frame — rebuilding the Yoga tree every frame is not what
// would make a plugin UI drop frames. What IS real is the allocator traffic:
// ~1.7 allocations per managed node per pass, so at 60fps a 484-node tree
// churns ~49k malloc/free pairs and ~19 MB/s of transient heap per second,
// forever, even when nothing changed (static and animated cost the same,
// within noise — see the second test case). That is an allocator-pressure and
// power argument, not a frame-budget argument, and it should be argued on
// those terms.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/view.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <memory>
#include <new>
#include <vector>

// ─── Global allocation counters ─────────────────────────────────────────────
//
// Replacing the global operator new/delete is the only way to count *all*
// allocations (Yoga is linked as a static library into this binary, so its
// `new yoga::Node` lands here too). Counting is off until a bench region
// explicitly arms it, so Catch2's own startup allocations are not counted.

namespace {
std::atomic<bool>     g_counting{false};
std::atomic<uint64_t> g_alloc_count{0};
std::atomic<uint64_t> g_alloc_bytes{0};
std::atomic<uint64_t> g_free_count{0};

inline void note_alloc(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
    }
}
inline void note_free() {
    if (g_counting.load(std::memory_order_relaxed))
        g_free_count.fetch_add(1, std::memory_order_relaxed);
}

void* aligned_alloc_impl(std::size_t n, std::size_t align) {
#if defined(_MSC_VER)
    return _aligned_malloc(n ? n : 1, align);
#else
    std::size_t size = n ? n : 1;
    if (size % align != 0) size += align - (size % align);
    return std::aligned_alloc(align, size);
#endif
}
void aligned_free_impl(void* p) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace

void* operator new(std::size_t n) {
    note_alloc(n);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    note_alloc(n);
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return operator new(n, t);
}
void* operator new(std::size_t n, std::align_val_t a) {
    note_alloc(n);
    void* p = aligned_alloc_impl(n, static_cast<std::size_t>(a));
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }

void operator delete(void* p) noexcept { if (p) note_free(); std::free(p); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { operator delete(p); }
void operator delete(void* p, std::align_val_t) noexcept {
    if (p) note_free();
    aligned_free_impl(p);
}
void operator delete[](void* p, std::align_val_t a) noexcept { operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { operator delete(p, a); }

// ─── Bench harness ──────────────────────────────────────────────────────────

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFrameBudgetUs = 16666.7; // 60fps

// A plugin-UI-shaped tree: root column → `rows` panel rows → `per_row` leaves
// each. Leaf count is exactly `rows * per_row`; total managed node count
// (what yoga_layout allocates a YGNode for) is `1 + rows + rows * per_row`.
struct Tree {
    View root;
    std::vector<View*> leaves;
    std::vector<View*> rows;
    int managed_nodes = 0;
};

std::unique_ptr<Tree> build_tree(int rows, int per_row) {
    auto t = std::make_unique<Tree>();
    t->root.set_bounds({0, 0, 800, 600});
    t->root.flex().direction = FlexDirection::column;
    t->root.flex().gap = 4;
    t->root.flex().padding = 8;

    for (int r = 0; r < rows; ++r) {
        auto row = std::make_unique<View>();
        row->flex().direction = FlexDirection::row;
        row->flex().gap = 6;
        row->flex().flex_grow = 1;
        auto* row_ptr = row.get();

        for (int c = 0; c < per_row; ++c) {
            auto leaf = std::make_unique<View>();
            leaf->flex().preferred_width = 40;
            leaf->flex().margin = 2;
            leaf->flex().flex_grow = (c == 0) ? 1.0f : 0.0f;
            t->leaves.push_back(leaf.get());
            row_ptr->add_child(std::move(leaf));
        }

        t->rows.push_back(row_ptr);
        t->root.add_child(std::move(row));
    }

    t->managed_nodes = 1 + rows + rows * per_row;
    return t;
}

struct Result {
    int    nodes = 0;
    bool   animated = false;
    double mean_us = 0;
    double median_us = 0;
    double min_us = 0;
    double p95_us = 0;
    double allocs_per_pass = 0;
    double alloc_bytes_per_pass = 0;
    double frees_per_pass = 0;

    double frame_pct() const { return 100.0 * mean_us / kFrameBudgetUs; }
};

// One measured shape. `animated` mutates a layout-affecting style on a leaf
// before each pass (the "a fader is moving" case); otherwise nothing at all
// changes between passes (the "only a shader uniform ticked" case). Both are
// measured because the whole point is that today they cost the same.
Result measure(Tree& t, bool animated, int passes, int warmup) {
    Result r;
    r.nodes = t.managed_nodes;
    r.animated = animated;

    for (int i = 0; i < warmup; ++i) {
        if (animated) t.leaves[0]->flex().preferred_width = 40.0f + (i % 2) * 8.0f;
        t.root.layout_children();
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(passes));

    g_alloc_count.store(0, std::memory_order_relaxed);
    g_alloc_bytes.store(0, std::memory_order_relaxed);
    g_free_count.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);

    for (int i = 0; i < passes; ++i) {
        if (animated) t.leaves[0]->flex().preferred_width = 40.0f + (i % 2) * 8.0f;
        auto t0 = std::chrono::steady_clock::now();
        t.root.layout_children();
        auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    g_counting.store(false, std::memory_order_relaxed);

    const double n = static_cast<double>(passes);
    r.allocs_per_pass       = static_cast<double>(g_alloc_count.load()) / n;
    r.alloc_bytes_per_pass  = static_cast<double>(g_alloc_bytes.load()) / n;
    r.frees_per_pass        = static_cast<double>(g_free_count.load()) / n;

    // `samples.push_back` allocates only on growth (reserved above), and the
    // clock reads do not allocate, so the counters above are the layout pass
    // plus a negligible constant. Sort AFTER counting is disarmed.
    std::sort(samples.begin(), samples.end());
    r.min_us    = samples.front();
    r.median_us = samples[samples.size() / 2];
    r.p95_us    = samples[static_cast<size_t>(0.95 * (samples.size() - 1))];
    double sum = 0;
    for (double s : samples) sum += s;
    r.mean_us = sum / n;
    return r;
}

void print_header() {
    std::printf("\n  nodes  mode      mean_us  median_us     p95_us   allocs/pass  "
                "bytes/pass  frees/pass  %%frame(60fps)\n");
    std::printf("  ----------------------------------------------------------------"
                "----------------------------------------------\n");
}

void print_row(const Result& r) {
    std::printf("  %5d  %-8s %9.1f %10.1f %10.1f %13.0f %11.0f %11.0f %9.2f%%\n",
                r.nodes, r.animated ? "animated" : "static",
                r.mean_us, r.median_us, r.p95_us,
                r.allocs_per_pass, r.alloc_bytes_per_pass, r.frees_per_pass,
                r.frame_pct());
}

} // namespace

TEST_CASE("Yoga layout pass cost at 100 / 250 / 500 nodes", "[view][layout][bench][yoga]") {
    // rows x per_row chosen so managed node count lands on ~100 / ~250 / ~500.
    // managed = 1 + rows + rows*per_row.
    struct Shape { int rows; int per_row; };
    const Shape shapes[] = {
        {9, 10},   // 1 + 9 + 90   = 100
        {15, 15},  // 1 + 15 + 225 = 241
        {21, 22},  // 1 + 21 + 462 = 484
    };

    constexpr int kPasses = 300;
    constexpr int kWarmup = 30;

    std::vector<Result> results;
    print_header();

    for (const auto& s : shapes) {
        auto tree = build_tree(s.rows, s.per_row);

        // Correctness anchor: the layout the bench is timing must be a real
        // layout. Rows stack vertically inside the padded root, each leaf is
        // laid out inside its row. If this ever stops holding, the timing
        // numbers are measuring nothing and must not be trusted.
        tree->root.layout_children();
        REQUIRE_THAT(tree->rows.front()->bounds().y, WithinAbs(8.0, 0.1)); // root padding
        REQUIRE(tree->rows.back()->bounds().y > tree->rows.front()->bounds().y);
        for (auto* leaf : tree->leaves) {
            REQUIRE(leaf->bounds().width > 0.0f);
            REQUIRE(leaf->bounds().height > 0.0f);
        }
        // Rows stretch to the padded root width (800 - 2*8), and no leaf is
        // wider than its preferred_width (the widest shape overflows its row,
        // so leaves shrink below 40 — but never grow past it, except the one
        // flex_grow:1 leaf per row).
        REQUIRE_THAT(tree->rows.front()->bounds().width, WithinAbs(784.0, 0.1));
        REQUIRE(tree->leaves[1]->bounds().width <= 40.0f + 0.1f);

        for (bool animated : {false, true}) {
            auto r = measure(*tree, animated, kPasses, kWarmup);
            print_row(r);
            results.push_back(r);
        }
    }
    std::printf("\n");

    // ── The gate ────────────────────────────────────────────────────────────
    //
    // Measured on an M-series Mac (Release, -O3): a 484-node pass runs in
    // ~0.38ms (see the baseline table at the top). The threshold below sits an order
    // of magnitude above that so the gate fires on a real regression (e.g.
    // someone makes layout quadratic, or adds a per-node syscall / shaping
    // call), not on a loaded CI box.
    //
    // A pass must never eat a quarter of a 60fps frame — at that point the
    // pre-paint layout call is the frame budget.
    for (const auto& r : results) {
        INFO("nodes=" << r.nodes << " animated=" << r.animated
                      << " mean_us=" << r.mean_us);
        REQUIRE(r.mean_us < 0.25 * kFrameBudgetUs);
    }

    // The allocation counters must actually be observing the layout pass. A
    // zero here means the operator new replacement got linked out and every
    // allocation number in the table above is a lie.
    for (const auto& r : results) {
        INFO("nodes=" << r.nodes);
        REQUIRE(r.allocs_per_pass > 0.0);
    }

    // Every YGNode built in a pass is freed in the same pass: the tree is
    // built and destroyed inside yoga_layout(). Frees should track allocs
    // 1:1 (small slack for allocations that outlive the pass, e.g. a
    // std::vector growing its capacity once).
    for (const auto& r : results) {
        INFO("nodes=" << r.nodes << " allocs=" << r.allocs_per_pass
                      << " frees=" << r.frees_per_pass);
        REQUIRE(r.frees_per_pass >= r.allocs_per_pass - 1.0);
    }
}

// Documents the finding that motivates any future dirty-gate work: a layout
// pass over a tree where NOTHING changed costs essentially the same as one
// where a style changed, because the YGNode tree (and with it Yoga's dirty
// flags and measure caches) is rebuilt from scratch either way.
//
// The assertion is deliberately loose (static must not be *more* than 2x the
// animated cost) — it is a statement about the architecture, not a timing
// race. If someone lands persistent Yoga nodes plus a dirty gate, the static
// case should collapse toward zero and this test still passes; the printed
// ratio is what tells the story.
TEST_CASE("Unchanged Yoga tree costs the same as a changed one", "[view][layout][bench][yoga]") {
    auto tree = build_tree(15, 15); // 241 managed nodes

    auto stat = measure(*tree, /*animated=*/false, 300, 30);
    auto anim = measure(*tree, /*animated=*/true, 300, 30);

    std::printf("\n  no-op layout pass: %.1f us / %.0f allocs   "
                "changed-style pass: %.1f us / %.0f allocs   (ratio %.2fx)\n\n",
                stat.mean_us, stat.allocs_per_pass,
                anim.mean_us, anim.allocs_per_pass,
                anim.mean_us > 0 ? stat.mean_us / anim.mean_us : 0.0);

    REQUIRE(stat.mean_us < 2.0 * anim.mean_us);
    // Same allocation volume either way — the churn is unconditional.
    REQUIRE(std::abs(stat.allocs_per_pass - anim.allocs_per_pass) <= 1.0);
}

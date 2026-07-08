#include <catch2/catch_test_macros.hpp>

#include <pulp/view/view_pool.hpp>
#include <pulp/view/virtual_list.hpp>

#include <cstddef>
#include <memory>
#include <typeindex>
#include <typeinfo>

using namespace pulp::view;

namespace {

// A recyclable View that opts into pooling and instruments its lifecycle so a
// test can count live instances, destructions, and prepare_for_reuse() calls.
// Templated on an int tag so each concrete alias (RowA / RowB) is a DISTINCT
// most-derived type with its OWN static counters — exactly what the pool keys
// on via typeid.
template <int Kind>
struct Recyclable : View {
    static inline int live = 0;
    static inline int destroyed = 0;
    static inline int prepared = 0;

    Recyclable() { ++live; }
    ~Recyclable() override {
        --live;
        ++destroyed;
    }

    bool supports_reuse() const override { return true; }
    void prepare_for_reuse() override {
        ++prepared;
        View::prepare_for_reuse();
    }

    static void reset_counters() {
        live = 0;
        destroyed = 0;
        prepared = 0;
    }
};

using RowA = Recyclable<0>;
using RowB = Recyclable<1>;

// A plain View that never opts into reuse — the pool must destroy it, never
// park it.
struct Plain : View {
    static inline int live = 0;
    Plain() { ++live; }
    ~Plain() override { --live; }
};

template <class T>
std::unique_ptr<T> make() {
    return std::make_unique<T>();
}

}  // namespace

TEST_CASE("ViewPool acquire/release round-trip preserves identity", "[view-pool]") {
    RowA::reset_counters();
    ViewPool pool;

    auto first = pool.acquire<RowA>([] { return make<RowA>(); });
    View* raw = first.get();
    REQUIRE(raw != nullptr);
    REQUIRE(RowA::live == 1);

    pool.release(std::move(first));
    REQUIRE(pool.size(std::type_index(typeid(RowA))) == 1);
    REQUIRE(RowA::live == 1);  // parked, not destroyed

    // Second acquire must hand back the SAME object, not build a new one.
    bool factory_called = false;
    auto second = pool.acquire<RowA>([&] {
        factory_called = true;
        return make<RowA>();
    });
    REQUIRE(second.get() == raw);
    REQUIRE_FALSE(factory_called);
    REQUIRE(pool.total_size() == 0);
    REQUIRE(RowA::live == 1);
}

TEST_CASE("ViewPool per-class cap evicts (destroys) over-cap releases", "[view-pool]") {
    RowA::reset_counters();
    ViewPool pool(2);  // cap of 2 per class

    auto a = make<RowA>();
    auto b = make<RowA>();
    auto c = make<RowA>();
    REQUIRE(RowA::live == 3);
    REQUIRE(RowA::destroyed == 0);

    pool.release(std::move(a));
    pool.release(std::move(b));
    REQUIRE(pool.size(std::type_index(typeid(RowA))) == 2);
    REQUIRE(RowA::destroyed == 0);

    // Third release is over the cap → destroyed immediately, not parked.
    pool.release(std::move(c));
    REQUIRE(pool.size(std::type_index(typeid(RowA))) == 2);
    REQUIRE(RowA::destroyed == 1);
    REQUIRE(RowA::live == 2);

    // clear() drains and destroys the remaining parked instances.
    pool.clear();
    REQUIRE(pool.total_size() == 0);
    REQUIRE(RowA::live == 0);
    REQUIRE(RowA::destroyed == 3);
}

TEST_CASE("ViewPool calls prepare_for_reuse exactly once per reuse", "[view-pool]") {
    RowA::reset_counters();
    ViewPool pool;
    auto factory = [] { return make<RowA>(); };

    // Fresh construction goes through the factory — no reuse, no reset.
    auto a = pool.acquire<RowA>(factory);
    REQUIRE(RowA::prepared == 0);

    pool.release(std::move(a));
    auto b = pool.acquire<RowA>(factory);
    REQUIRE(RowA::prepared == 1);  // one reuse → one reset

    pool.release(std::move(b));
    auto c = pool.acquire<RowA>(factory);
    REQUIRE(RowA::prepared == 2);  // second reuse → second reset

    // A release that does NOT come back out again never gets a reset.
    pool.release(std::move(c));
    REQUIRE(RowA::prepared == 2);
}

TEST_CASE("ViewPool destroys non-opt-in views instead of pooling", "[view-pool]") {
    Plain::live = 0;
    ViewPool pool;

    auto p = make<Plain>();
    REQUIRE(Plain::live == 1);
    pool.release(std::move(p));

    REQUIRE(pool.total_size() == 0);
    REQUIRE(Plain::live == 0);  // destroyed on release, not parked
}

TEST_CASE("ViewPool never hands out a different class (no cross-class handout)",
          "[view-pool]") {
    RowA::reset_counters();
    RowB::reset_counters();
    ViewPool pool;

    pool.release(make<RowA>());
    REQUIRE(pool.size(std::type_index(typeid(RowA))) == 1);

    // Typed acquire<RowB> must NOT be satisfied by the parked RowA.
    bool factory_called = false;
    auto b = pool.acquire<RowB>([&] {
        factory_called = true;
        return make<RowB>();
    });
    REQUIRE(factory_called);
    REQUIRE(b != nullptr);
    // The RowA is still parked and untouched.
    REQUIRE(pool.size(std::type_index(typeid(RowA))) == 1);

    // Type-erased acquire for the wrong key returns nullptr, not the RowA.
    REQUIRE(pool.acquire(std::type_index(typeid(RowB))) == nullptr);
    REQUIRE(pool.size(std::type_index(typeid(RowA))) == 1);
}

// Codex must-fix #5: a recycled view must not carry a stale callback capturing
// freed state. prepare_for_reuse() must clear every base callback so re-using
// the view can never fire the previous occupant's closure.
TEST_CASE("ViewPool clears poisoned base callbacks on reuse", "[view-pool]") {
    RowA::reset_counters();
    ViewPool pool;
    auto factory = [] { return make<RowA>(); };

    bool fired = false;
    auto a = pool.acquire<RowA>(factory);
    a->on_click = [&fired] { fired = true; };
    a->on_drag = [&fired](Point) { fired = true; };
    a->on_hover_enter = [&fired] { fired = true; };
    REQUIRE(static_cast<bool>(a->on_click));

    pool.release(std::move(a));
    auto b = pool.acquire<RowA>(factory);

    // The stale callbacks must be gone. Invoking through them (which the widget
    // machinery would do) must NOT fire the previous occupant's closure.
    REQUIRE_FALSE(static_cast<bool>(b->on_click));
    REQUIRE_FALSE(static_cast<bool>(b->on_drag));
    REQUIRE_FALSE(static_cast<bool>(b->on_hover_enter));
    if (b->on_click) b->on_click();
    REQUIRE_FALSE(fired);
}

TEST_CASE("View::prepare_for_reuse resets per-instance base state", "[view-pool]") {
    RowA::reset_counters();
    ViewPool pool;
    auto factory = [] { return make<RowA>(); };

    auto a = pool.acquire<RowA>(factory);
    a->set_bounds({10, 20, 30, 40});
    a->set_visible(false);
    a->set_opacity(0.25f);
    a->set_access_label("row 7");
    a->set_access_value("value");
    a->set_hovered(true);
    a->set_focus(true);

    pool.release(std::move(a));
    auto b = pool.acquire<RowA>(factory);

    REQUIRE(b->bounds().width == 0);
    REQUIRE(b->bounds().height == 0);
    REQUIRE(b->visible());
    REQUIRE(b->opacity() == 1.0f);
    REQUIRE(b->access_label().empty());
    REQUIRE(b->access_value().empty());
    REQUIRE_FALSE(b->is_hovered());
    REQUIRE_FALSE(b->has_focus());
}

// Integration: VirtualList adopts the pool. Shrinking then re-growing the
// realized window must recycle opt-in rows (park on release, re-acquire on
// growth) rather than destroy + rebuild them.
TEST_CASE("VirtualList recycles opt-in rows through its ViewPool", "[view-pool][virtual-list]") {
    RowA::reset_counters();
    int factory_calls = 0;

    VirtualList list;
    list.set_row_height(24.0f);
    list.set_overscan(3);
    list.set_row_count(100);
    list.set_row_factory([&](std::size_t) -> std::unique_ptr<View> {
        ++factory_calls;
        return make<RowA>();
    });

    // Initial realize at a tall viewport.
    list.set_bounds({0, 0, 200, 240});
    const int initial_factory_calls = factory_calls;
    const std::size_t realized_tall = list.realized_row_count();
    REQUIRE(realized_tall > 0);
    REQUIRE(RowA::destroyed == 0);

    // Shrink the viewport: excess rows are released. Because RowA opts into
    // reuse they are parked in the pool, not destroyed.
    list.set_bounds({0, 0, 200, 48});
    REQUIRE(list.realized_row_count() < realized_tall);
    REQUIRE(RowA::destroyed == 0);

    // Grow back: the released rows are re-acquired from the pool. The factory is
    // NOT called again, proving recycling rather than reconstruction.
    list.set_bounds({0, 0, 200, 240});
    REQUIRE(list.realized_row_count() == realized_tall);
    REQUIRE(factory_calls == initial_factory_calls);
    REQUIRE(RowA::destroyed == 0);
}

// Swapping the row factory must not recycle rows built by the previous factory
// into the new one — the pool is dropped so the new factory always runs.
TEST_CASE("VirtualList drops recycled rows when the factory changes",
          "[view-pool][virtual-list]") {
    RowA::reset_counters();
    RowB::reset_counters();

    VirtualList list;
    list.set_row_height(24.0f);
    list.set_overscan(3);
    list.set_row_count(100);
    list.set_row_factory([](std::size_t) -> std::unique_ptr<View> { return make<RowA>(); });
    list.set_bounds({0, 0, 200, 240});
    REQUIRE(RowA::live > 0);

    // Shrink to park some RowA in the pool, then swap in a RowB factory.
    list.set_bounds({0, 0, 200, 48});
    REQUIRE(RowA::destroyed == 0);  // parked, not destroyed

    int b_calls = 0;
    list.set_row_factory([&](std::size_t) -> std::unique_ptr<View> {
        ++b_calls;
        return make<RowB>();
    });
    // The factory swap must have drained the RowA pool (parked rows destroyed).
    REQUIRE(RowA::live == 0);

    // Re-realize: only RowB is ever produced; no RowA is resurrected.
    list.set_bounds({0, 0, 200, 240});
    REQUIRE(b_calls > 0);
    REQUIRE(RowA::live == 0);
    REQUIRE(RowB::live > 0);
}

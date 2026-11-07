#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/format/background_task_lane.hpp>
#include <pulp/runtime/fixed_function.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

using pulp::runtime::FixedFunction;

namespace {

int free_add(int a, int b) {
    return a + b;
}

struct Counter {
    int* destroyed;
    int value;
    Counter(int* d, int v) : destroyed(d), value(v) {}
    Counter(Counter&& o) noexcept : destroyed(o.destroyed), value(o.value) {
        o.destroyed = nullptr;
    }
    ~Counter() {
        if (destroyed)
            ++*destroyed;
    }
    int operator()() {
        return value;
    }
};

} // namespace

TEST_CASE("FixedFunction is empty by default and after reset", "[fixed_function]") {
    FixedFunction<void()> fn;
    CHECK_FALSE(static_cast<bool>(fn));
    CHECK(fn == nullptr);

    fn = [] {};
    CHECK(static_cast<bool>(fn));
    CHECK(fn != nullptr);
    fn.reset();
    CHECK_FALSE(static_cast<bool>(fn));

    FixedFunction<void()> from_null = nullptr;
    CHECK_FALSE(static_cast<bool>(from_null));
}

TEST_CASE("FixedFunction forwards arguments and returns results", "[fixed_function]") {
    FixedFunction<int(int, int)> add = [](int a, int b) { return a + b; };
    CHECK(add(2, 3) == 5);

    int offset = 10;
    FixedFunction<int(int)> with_capture = [&offset](int v) { return v + offset; };
    CHECK(with_capture(1) == 11);
    offset = 20;
    CHECK(with_capture(1) == 21);

    FixedFunction<int(int, int)> pointer = &free_add;
    CHECK(pointer(4, 5) == 9);

    // A target returning a value can be stored as returning void.
    int sink = 0;
    FixedFunction<void(int)> discard = [&sink](int v) { return sink = v; };
    discard(8);
    CHECK(sink == 8);
}

TEST_CASE("FixedFunction passes references and move-only arguments through", "[fixed_function]") {
    FixedFunction<void(int&)> by_ref = [](int& v) { v *= 2; };
    int value = 21;
    by_ref(value);
    CHECK(value == 42);

    FixedFunction<int(std::unique_ptr<int>)> consume = [](std::unique_ptr<int> p) { return *p; };
    CHECK(consume(std::make_unique<int>(7)) == 7);
}

TEST_CASE("FixedFunction is move-only and relocates its target", "[fixed_function]") {
    static_assert(!std::is_copy_constructible_v<FixedFunction<void()>>);
    static_assert(!std::is_copy_assignable_v<FixedFunction<void()>>);
    static_assert(std::is_nothrow_move_constructible_v<FixedFunction<void()>>);
    static_assert(std::is_nothrow_move_assignable_v<FixedFunction<void()>>);

    int destroyed = 0;
    {
        FixedFunction<int()> a = Counter(&destroyed, 5);
        CHECK(destroyed == 0); // the temporary's destructor ran on a moved-from object
        FixedFunction<int()> b = std::move(a);
        CHECK_FALSE(static_cast<bool>(a));
        CHECK(b() == 5);
        FixedFunction<int()> c;
        c = std::move(b);
        CHECK_FALSE(static_cast<bool>(b));
        CHECK(c() == 5);
        CHECK(destroyed == 0);
    }
    CHECK(destroyed == 1); // exactly one live target was destroyed
}

TEST_CASE("FixedFunction destroys the old target on reassignment", "[fixed_function]") {
    int destroyed = 0;
    FixedFunction<int()> fn = Counter(&destroyed, 1);
    fn = Counter(&destroyed, 2);
    CHECK(destroyed == 1);
    CHECK(fn() == 2);
    fn = nullptr;
    CHECK(destroyed == 2);
}

TEST_CASE("FixedFunction holds move-only captures", "[fixed_function]") {
    auto owned = std::make_unique<int>(99);
    FixedFunction<int(), 32> fn = [p = std::move(owned)] { return *p; };
    CHECK(fn() == 99);
    FixedFunction<int(), 32> moved = std::move(fn);
    CHECK(moved() == 99);
}

TEST_CASE("FixedFunction swaps", "[fixed_function]") {
    FixedFunction<int()> a = [] { return 1; };
    FixedFunction<int()> b = [] { return 2; };
    a.swap(b);
    CHECK(a() == 2);
    CHECK(b() == 1);
    FixedFunction<int()> empty;
    a.swap(empty);
    CHECK_FALSE(static_cast<bool>(a));
    CHECK(empty() == 2);
}

TEST_CASE("FixedFunction reports at compile time what fits", "[fixed_function]") {
    using Small = FixedFunction<void(), 16>;
    struct TwoPointers {
        void* a;
        void* b;
        void operator()() const {}
    };
    struct ThreePointers {
        void* a;
        void* b;
        void* c;
        void operator()() const {}
    };
    static_assert(Small::fits<TwoPointers>);
    static_assert(!Small::fits<ThreePointers>);

    struct ThrowingMove {
        ThrowingMove() = default;
        ThrowingMove(ThrowingMove&&) noexcept(false) {}
        void operator()() const {}
    };
    static_assert(!Small::fits<ThrowingMove>);
    static_assert(!std::is_constructible_v<FixedFunction<void(int)>, int>);
    SUCCEED();
}

TEST_CASE("FixedFunction construction, move, call and destruction never allocate",
          "[fixed_function][rt]") {
    std::atomic<int> hits{0};
    std::atomic<int>* target = &hits;
    int offset = 3;

    pulp::test::RtAllocationProbe probe;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        FixedFunction<void(int), 32> a = [target, offset](int v) { target->fetch_add(v + offset); };
        FixedFunction<void(int), 32> b = std::move(a);
        FixedFunction<void(int), 32> c;
        c = std::move(b);
        c(1);
        c(2);
        c.swap(a);
        a(3);
        c.reset();
    }
    CHECK_FALSE(probe.saw_allocation());
    CHECK(probe.allocation_count() == 0);
    CHECK(hits.load() == (1 + 3) + (2 + 3) + (3 + 3));
}

// A std::function with a capture larger than its inline buffer allocates, which
// is why it is not used on realtime paths. This keeps the probe honest: if it
// could not observe an allocation, the checks above would prove nothing.
TEST_CASE("the allocation probe observes a std::function that outgrows its buffer",
          "[fixed_function][rt]") {
    struct Big {
        char bytes[256];
    };
    Big big{};
    pulp::test::RtAllocationProbe probe;
    std::function<void()> fn = [big] { (void)big; };
    fn();
    CHECK(probe.saw_allocation());
}

// ── BackgroundTaskLane: the audio-to-worker command lane ─────────────────────

TEST_CASE("BackgroundTaskLane runs a capturing handler and spawns without allocating",
          "[fixed_function][rt][background-task]") {
    struct Seen {
        std::atomic<int> count{0};
        std::atomic<int> sum{0};
    } seen;
    pulp::format::BackgroundTaskLane<int, 8> lane;
    // A capturing lambda needs no void* context and is stored inline.
    REQUIRE(lane.start([&seen](const int& v) {
        seen.sum.fetch_add(v, std::memory_order_relaxed);
        seen.count.fetch_add(1, std::memory_order_release);
    }));

    {
        pulp::test::RtAllocationProbe probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (int i = 1; i <= 4; ++i)
            REQUIRE(lane.try_spawn(i));
        CHECK_FALSE(probe.saw_allocation());
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (seen.count.load(std::memory_order_acquire) != 4 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    lane.stop();
    CHECK(seen.count.load() == 4);
    CHECK(seen.sum.load() == 10);
}

TEST_CASE("BackgroundTaskLane still accepts a function pointer and context",
          "[fixed_function][background-task]") {
    std::atomic<int> got{0};
    pulp::format::BackgroundTaskLane<int> lane;
    REQUIRE(lane.start(
        [](void* ctx, const int& v) noexcept { static_cast<std::atomic<int>*>(ctx)->store(v); },
        &got));
    REQUIRE(lane.try_spawn(7));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (got.load() != 7 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    lane.stop();
    CHECK(got.load() == 7);

    pulp::format::BackgroundTaskLane<int> empty;
    CHECK_FALSE(empty.start(pulp::format::BackgroundTaskLane<int>::HandlerFunction{}));
    CHECK_FALSE(empty.start(static_cast<pulp::format::BackgroundTaskLane<int>::Handler>(nullptr)));
}

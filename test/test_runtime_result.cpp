// Regression for Result<T, E> (gap-doc Runtime row "Result"). Pure
// header-only value type — no platform calls.

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/result.hpp>

#include <stdexcept>
#include <string>

using pulp::runtime::Result;
using pulp::runtime::Ok;
using pulp::runtime::Err;

namespace {

struct InstanceCounter {
    static inline int alive = 0;
    int payload = 0;
    InstanceCounter() : payload(0) { ++alive; }
    explicit InstanceCounter(int p) : payload(p) { ++alive; }
    InstanceCounter(const InstanceCounter& o) : payload(o.payload) { ++alive; }
    InstanceCounter(InstanceCounter&& o) noexcept : payload(o.payload) { ++alive; }
    InstanceCounter& operator=(const InstanceCounter& o) { payload = o.payload; return *this; }
    InstanceCounter& operator=(InstanceCounter&& o) noexcept { payload = o.payload; return *this; }
    ~InstanceCounter() { --alive; }
};

}  // namespace

TEST_CASE("Result holds Ok value", "[runtime][result]") {
    Result<int, std::string> r(Ok(42));
    REQUIRE(r.has_value());
    REQUIRE(static_cast<bool>(r));
    REQUIRE_FALSE(r.is_err());
    REQUIRE(*r == 42);
    REQUIRE(r.value() == 42);
    REQUIRE(r.value_or(0) == 42);
}

TEST_CASE("Result holds Err value", "[runtime][result]") {
    Result<int, std::string> r(Err(std::string("boom")));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.is_err());
    REQUIRE(r.error() == "boom");
    REQUIRE(r.value_or(99) == 99);
}

TEST_CASE("Result default-constructs T", "[runtime][result]") {
    Result<int, std::string> r;
    REQUIRE(r.has_value());
    REQUIRE(*r == 0);
}

TEST_CASE("Result move and copy preserve state", "[runtime][result]") {
    Result<std::string, int> ok(Ok(std::string("hi")));
    auto copy = ok;
    REQUIRE(copy.has_value());
    REQUIRE(*copy == "hi");
    auto moved = std::move(ok);
    REQUIRE(moved.has_value());
    REQUIRE(*moved == "hi");

    Result<std::string, int> err(Err(7));
    auto copy_e = err;
    REQUIRE(copy_e.is_err());
    REQUIRE(copy_e.error() == 7);
}

TEST_CASE("Result destructor releases held type", "[runtime][result]") {
    InstanceCounter::alive = 0;
    {
        Result<InstanceCounter, std::string> r(Ok(InstanceCounter(11)));
        REQUIRE(r.has_value());
        REQUIRE(InstanceCounter::alive == 1);
    }
    REQUIRE(InstanceCounter::alive == 0);
    {
        Result<int, InstanceCounter> r(Err(InstanceCounter(22)));
        REQUIRE(r.is_err());
        REQUIRE(InstanceCounter::alive == 1);
    }
    REQUIRE(InstanceCounter::alive == 0);
}

TEST_CASE("Result assignment swaps stored category", "[runtime][result]") {
    Result<int, std::string> r(Ok(1));
    r = Result<int, std::string>(Err(std::string("nope")));
    REQUIRE(r.is_err());
    REQUIRE(r.error() == "nope");
    r = Result<int, std::string>(Ok(99));
    REQUIRE(r.has_value());
    REQUIRE(*r == 99);
}

TEST_CASE("Result operator-> reaches T members", "[runtime][result]") {
    Result<std::string, int> r(Ok(std::string("hello")));
    REQUIRE(r->size() == 5);
    REQUIRE(r->front() == 'h');
}

// Regression for the Codex P2 review comment "Preserve Result object
// when assignment construction throws" — earlier implementation
// destroyed the current member before constructing the new one, so a
// throwing copy ctor left the union in an indeterminate state.
namespace {

struct Throwing {
    static inline bool should_throw = false;
    int payload = 0;
    Throwing() = default;
    explicit Throwing(int p) : payload(p) {}
    Throwing(const Throwing& o) : payload(o.payload) {
        if (should_throw) throw std::runtime_error("boom");
    }
    Throwing(Throwing&& o) noexcept : payload(o.payload) {}
    Throwing& operator=(const Throwing&) = default;
    Throwing& operator=(Throwing&&) noexcept = default;
};

}  // namespace

TEST_CASE("Result assignment preserves destination on throw",
          "[runtime][result][codex-p2]") {
    Result<Throwing, std::string> dst(Ok(Throwing(11)));
    Result<Throwing, std::string> src(Ok(Throwing(99)));

    Throwing::should_throw = true;
    try {
        dst = src;
        FAIL("expected throw");
    } catch (const std::runtime_error&) {
        // expected
    }
    Throwing::should_throw = false;

    // Strong guarantee: dst must still hold its original value, and be
    // safe to access (no UB on destruction at end of scope).
    REQUIRE(dst.has_value());
    REQUIRE(dst->payload == 11);
}

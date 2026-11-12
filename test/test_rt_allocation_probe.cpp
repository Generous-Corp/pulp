// The RT allocation probe counts what it should: its self-check, each
// replaced allocation operator with its size, nested probes, and only the
// calling thread. Tests that assert an absence of allocations rely on these.
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <new>
#include <thread>

using pulp::test::RtAllocationProbe;

TEST_CASE("RtAllocationProbe self-check sees an escaping allocation", "[rt-safety][probe]") {
    REQUIRE(RtAllocationProbe::self_check());
    REQUIRE_FALSE(pulp::test::rt_allocation_probe_active());
}

TEST_CASE("RtAllocationProbe counts each allocation operator with its size", "[rt-safety][probe]") {
    RtAllocationProbe probe;
    REQUIRE(pulp::test::rt_allocation_probe_active());
    int* volatile one = new int(1);
    delete one;
    char* volatile many = new char[40];
    delete[] many;
    int* volatile nothrow = new (std::nothrow) int(2);
    delete nothrow;
    struct alignas(64) Wide {
        char bytes[64];
    };
    Wide* volatile wide = new Wide;
    delete wide;
    REQUIRE(probe.allocation_count() == 4);
    REQUIRE(probe.allocated_bytes() == sizeof(int) + 40 + sizeof(int) + sizeof(Wide));
}

TEST_CASE("RtAllocationProbe nests: an inner probe sees only its own scope", "[rt-safety][probe]") {
    RtAllocationProbe outer;
    {
        RtAllocationProbe inner;
        int* volatile p = new int(3);
        delete p;
        REQUIRE(inner.allocation_count() == 1);
    }
    REQUIRE(outer.allocation_count() == 0);
    int* volatile q = new int(4);
    delete q;
    REQUIRE(outer.allocation_count() == 1);
}

TEST_CASE("RtAllocationProbe ignores other threads", "[rt-safety][probe]") {
    RtAllocationProbe probe;
    std::thread other([] {
        int* volatile p = new int(5);
        delete p;
    });
    other.join();
    // Starting the thread may allocate on this thread (its state); count only
    // what happens after, on the other thread.
    const auto before = probe.allocation_count();
    std::thread second([] {
        int* volatile p = new int(6);
        delete p;
    });
    const auto after_start = probe.allocation_count();
    second.join();
    REQUIRE(probe.allocation_count() == after_start);
    REQUIRE(after_start >= before);
}

// Tests for RetireList — the grace-period reclamation that keeps the audio thread
// from freeing (or using-after-free) an engine it swapped away from.

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "nam_retire_list.hpp"

using pulp::examples::nam::RetireList;

namespace {
// A payload that flips a flag when destroyed, so the test can observe exactly when
// a retiree is actually freed.
struct Tracer {
    bool* dead;
    explicit Tracer(bool* d) : dead(d) {}
    ~Tracer() { *dead = true; }
};
} // namespace

TEST_CASE("retiree is held until the audio epoch advances by the grace window", "[nam][retire]") {
    RetireList list;
    bool dead = false;
    // Retire at epoch 10.
    list.retire(std::shared_ptr<void>(std::make_shared<Tracer>(&dead)), 10);
    REQUIRE(list.size() == 1);

    // Same epoch, and one short of the grace window: still alive.
    list.reclaim(10);
    CHECK_FALSE(dead);
    list.reclaim(10 + RetireList::kGrace - 1);
    CHECK_FALSE(dead);
    REQUIRE(list.size() == 1);

    // Grace window reached: freed.
    list.reclaim(10 + RetireList::kGrace);
    CHECK(dead);
    CHECK(list.size() == 0);
}

TEST_CASE("retirees free independently by their own retirement epoch", "[nam][retire]") {
    RetireList list;
    bool a = false, b = false;
    list.retire(std::shared_ptr<void>(std::make_shared<Tracer>(&a)), 0);
    list.retire(std::shared_ptr<void>(std::make_shared<Tracer>(&b)), 100);

    // Advance past the first's grace but not the second's.
    list.reclaim(RetireList::kGrace);
    CHECK(a);
    CHECK_FALSE(b);
    CHECK(list.size() == 1);

    list.reclaim(100 + RetireList::kGrace);
    CHECK(b);
    CHECK(list.size() == 0);
}

TEST_CASE("clear frees everything (audio stopped)", "[nam][retire]") {
    RetireList list;
    bool dead = false;
    list.retire(std::shared_ptr<void>(std::make_shared<Tracer>(&dead)), 5);
    list.clear();
    CHECK(dead);
    CHECK(list.size() == 0);
}

TEST_CASE("a null retiree is ignored", "[nam][retire]") {
    RetireList list;
    list.retire(nullptr, 0);
    CHECK(list.size() == 0);
}

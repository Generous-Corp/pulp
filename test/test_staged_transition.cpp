// StagedTransition: the schedule arithmetic and the run calls that spread a
// bounded piece of transition work over several audio callbacks.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/staged_transition.hpp>

#include <vector>

using pulp::signal::StagedTransition;

TEST_CASE("StagedTransition::due_steps is ceil(total * elapsed / horizon), clamped",
          "[signal][staged-transition]") {
    CHECK(StagedTransition::due_steps(24, 0, 512) == 0);
    CHECK(StagedTransition::due_steps(24, 1, 512) == 1);    // any progress runs a step
    CHECK(StagedTransition::due_steps(24, 128, 512) == 6);
    CHECK(StagedTransition::due_steps(24, 129, 512) == 7);
    CHECK(StagedTransition::due_steps(24, 512, 512) == 24);
    CHECK(StagedTransition::due_steps(24, 9000, 512) == 24);
    CHECK(StagedTransition::due_steps(24, 5, 0) == 24);     // no horizon: all now
    CHECK(StagedTransition::due_steps(0, 5, 10) == 0);
    CHECK(StagedTransition::due_steps(24, -3, 512) == 0);
}

TEST_CASE("StagedTransition runs each step once, in order, when due",
          "[signal][staged-transition]") {
    StagedTransition staged;
    std::vector<int> ran;
    const auto step = [&](int index) { ran.push_back(index); };

    CHECK_FALSE(staged.active());
    staged.begin(10);
    CHECK(staged.active());
    CHECK(ran.empty()); // begin schedules; it runs nothing

    // Four callbacks of 128 frames spread over a 512-frame horizon.
    CHECK(staged.run_due(128, 512, step) == 3);
    CHECK(staged.run_due(256, 512, step) == 2);
    CHECK(staged.run_due(256, 512, step) == 0); // nothing newly due
    CHECK(staged.run_due(384, 512, step) == 3);
    CHECK(staged.remaining() == 2);
    CHECK(staged.run_due(512, 512, step) == 2);
    CHECK_FALSE(staged.active());
    CHECK(staged.run_due(640, 512, step) == 0);
    CHECK(ran == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
}

TEST_CASE("StagedTransition run_next, finish_now, cancel and restart",
          "[signal][staged-transition]") {
    StagedTransition staged;
    std::vector<int> ran;
    const auto step = [&](int index) { ran.push_back(index); };

    staged.begin(5);
    CHECK(staged.run_next(2, step) == 2);
    CHECK(staged.completed() == 2);
    CHECK(staged.finish_now(step) == 3);
    CHECK(ran == std::vector<int>{0, 1, 2, 3, 4});
    CHECK(staged.run_next(1, step) == 0);

    ran.clear();
    staged.begin(4);
    staged.run_next(1, step);
    staged.begin(3); // a new schedule drops the old remainder
    CHECK(staged.total() == 3);
    CHECK(staged.completed() == 0);
    staged.run_next(10, step);
    CHECK(ran == std::vector<int>{0, 0, 1, 2});

    staged.begin(6);
    staged.cancel();
    CHECK_FALSE(staged.active());
    CHECK(staged.finish_now(step) == 0);

    // A step observes the schedule already advanced past itself.
    staged.begin(2);
    std::vector<int> remaining_seen;
    staged.finish_now([&](int) { remaining_seen.push_back(staged.remaining()); });
    CHECK(remaining_seen == std::vector<int>{1, 0});
}

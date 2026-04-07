#include <catch2/catch_test_macros.hpp>
#include <pulp/state/edit_history.hpp>

using namespace pulp::state;

TEST_CASE("EditHistory undo/redo", "[state][undo]") {
    EditHistory history;
    int value = 0;

    history.perform(
        [&]() { value = 10; },
        [&]() { value = 0; },
        "Set to 10");

    REQUIRE(value == 10);
    REQUIRE(history.can_undo());
    REQUIRE(history.undo_description() == "Set to 10");

    history.undo();
    REQUIRE(value == 0);
    REQUIRE(history.can_redo());

    history.redo();
    REQUIRE(value == 10);
}

TEST_CASE("EditHistory clear", "[state][undo]") {
    EditHistory history;
    int v = 0;
    history.perform([&]() { v = 1; }, [&]() { v = 0; });
    REQUIRE(history.can_undo());

    history.clear();
    REQUIRE_FALSE(history.can_undo());
    REQUIRE_FALSE(history.can_redo());
}

TEST_CASE("EditHistory depth limit", "[state][undo]") {
    EditHistory history(3);
    int v = 0;

    for (int i = 1; i <= 5; ++i) {
        int prev = v;
        int next = i;
        history.perform([&v, next]() { v = next; }, [&v, prev]() { v = prev; });
    }

    REQUIRE(history.undo_count() == 3);  // oldest 2 dropped
}

TEST_CASE("EditHistory redo cleared on new action", "[state][undo]") {
    EditHistory history;
    int v = 0;
    history.perform([&]() { v = 1; }, [&]() { v = 0; });
    history.perform([&]() { v = 2; }, [&]() { v = 1; });
    history.undo();
    REQUIRE(history.can_redo());

    // New action clears redo stack
    history.perform([&]() { v = 3; }, [&]() { v = 1; });
    REQUIRE_FALSE(history.can_redo());
}

TEST_CASE("EditHistory empty undo/redo returns false", "[state][undo]") {
    EditHistory history;
    REQUIRE_FALSE(history.undo());
    REQUIRE_FALSE(history.redo());
}

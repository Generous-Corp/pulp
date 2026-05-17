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

TEST_CASE("EditHistory coalesces matching non-empty descriptions", "[state][undo]") {
    EditHistory history;
    int v = 0;

    history.perform([&]() { v = 1; }, [&]() { v = 0; }, "Drag");
    history.perform([&]() { v = 2; }, [&]() { v = 1; }, "Drag");

    REQUIRE(v == 2);
    REQUIRE(history.undo_count() == 1);
    REQUIRE(history.undo_description() == "Drag");

    REQUIRE(history.undo());
    REQUIRE(v == 1);
    REQUIRE_FALSE(history.can_undo());
}

TEST_CASE("EditHistory does not coalesce empty descriptions", "[state][undo]") {
    EditHistory history;
    int v = 0;

    history.perform([&]() { v = 1; }, [&]() { v = 0; });
    history.perform([&]() { v = 2; }, [&]() { v = 1; });

    REQUIRE(v == 2);
    REQUIRE(history.undo_count() == 2);
    REQUIRE(history.undo_description().empty());
}

TEST_CASE("EditHistory coalescing can be disabled", "[state][undo]") {
    EditHistory history;
    int v = 0;
    history.set_coalesce(false);

    history.perform([&]() { v = 1; }, [&]() { v = 0; }, "Drag");
    history.perform([&]() { v = 2; }, [&]() { v = 1; }, "Drag");

    REQUIRE(v == 2);
    REQUIRE(history.undo_count() == 2);

    REQUIRE(history.undo());
    REQUIRE(v == 1);
    REQUIRE(history.can_undo());
}

TEST_CASE("EditHistory coalesced new action clears redo stack", "[state][undo]") {
    EditHistory history;
    int v = 0;

    history.perform([&]() { v = 1; }, [&]() { v = 0; }, "Drag");
    history.perform([&]() { v = 10; }, [&]() { v = 1; }, "Commit");
    REQUIRE(history.undo());
    REQUIRE(v == 1);
    REQUIRE(history.can_redo());
    REQUIRE(history.redo_count() == 1);

    history.perform([&]() { v = 2; }, [&]() { v = 1; }, "Drag");

    REQUIRE(v == 2);
    REQUIRE(history.undo_count() == 1);
    REQUIRE_FALSE(history.can_redo());
    REQUIRE(history.redo_count() == 0);
}

TEST_CASE("EditHistory empty undo/redo returns false", "[state][undo]") {
    EditHistory history;
    REQUIRE_FALSE(history.undo());
    REQUIRE_FALSE(history.redo());
}

TEST_CASE("EditHistory lowering max depth trims existing undo stack",
          "[state][undo][codecov]") {
    EditHistory history(5);
    int v = 0;

    for (int i = 1; i <= 4; ++i) {
        const int previous = v;
        const int next = i;
        history.perform([&v, next]() { v = next; },
                        [&v, previous]() { v = previous; },
                        "Step " + std::to_string(i));
    }

    REQUIRE(history.undo_count() == 4);
    history.set_max_depth(2);

    REQUIRE(history.max_depth() == 2);
    REQUIRE(history.undo_count() == 2);
    REQUIRE(history.undo_description() == "Step 4");

    REQUIRE(history.undo());
    REQUIRE(v == 3);
    REQUIRE(history.undo());
    REQUIRE(v == 2);
    REQUIRE_FALSE(history.can_undo());
}

TEST_CASE("EditHistory zero max depth performs actions without retaining history",
          "[state][undo][codecov]") {
    EditHistory history(0);
    int v = 0;

    history.perform([&] { v = 1; }, [&] { v = 0; }, "No history");

    REQUIRE(v == 1);
    REQUIRE(history.max_depth() == 0);
    REQUIRE(history.undo_count() == 0);
    REQUIRE_FALSE(history.can_undo());
    REQUIRE_FALSE(history.undo());
}

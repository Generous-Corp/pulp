#include <catch2/catch_test_macros.hpp>
#include <pulp/state/undo_manager.hpp>

using namespace pulp::state;

TEST_CASE("UndoManager perform and undo", "[state][undo]") {
    UndoManager um;
    int value = 0;

    um.perform(UndoAction::create("Set to 5",
        [&] { value = 0; },
        [&] { value = 5; }));

    REQUIRE(value == 5);
    REQUIRE(um.can_undo());

    um.undo();
    REQUIRE(value == 0);
    REQUIRE_FALSE(um.can_undo());
}

TEST_CASE("UndoManager empty undo and redo are no-ops", "[state][undo][coverage][phase3]") {
    UndoManager um;
    int changes = 0;
    um.on_state_changed = [&] { ++changes; };

    REQUIRE_FALSE(um.undo());
    REQUIRE_FALSE(um.redo());
    REQUIRE_FALSE(um.can_undo());
    REQUIRE_FALSE(um.can_redo());
    REQUIRE(um.undo_name().empty());
    REQUIRE(um.redo_name().empty());
    REQUIRE(changes == 0);
}

TEST_CASE("UndoManager redo after undo", "[state][undo]") {
    UndoManager um;
    int value = 0;

    um.perform(UndoAction::create("Set to 10",
        [&] { value = 0; },
        [&] { value = 10; }));

    um.undo();
    REQUIRE(value == 0);
    REQUIRE(um.can_redo());

    um.redo();
    REQUIRE(value == 10);
    REQUIRE_FALSE(um.can_redo());
}

TEST_CASE("UndoManager new action clears redo stack", "[state][undo]") {
    UndoManager um;
    int value = 0;

    um.perform(UndoAction::create("A", [&] { value = 0; }, [&] { value = 1; }));
    um.undo();
    REQUIRE(um.can_redo());

    um.perform(UndoAction::create("B", [&] { value = 0; }, [&] { value = 2; }));
    REQUIRE_FALSE(um.can_redo()); // redo cleared
}

TEST_CASE("UndoManager transaction groups actions", "[state][undo]") {
    UndoManager um;
    int x = 0, y = 0;

    um.begin_transaction("Move");
    um.perform(UndoAction::create("Set X", [&] { x = 0; }, [&] { x = 10; }));
    um.perform(UndoAction::create("Set Y", [&] { y = 0; }, [&] { y = 20; }));
    um.end_transaction();

    REQUIRE(x == 10);
    REQUIRE(y == 20);
    REQUIRE(um.undo_count() == 1); // single undo step

    um.undo(); // undoes both
    REQUIRE(x == 0);
    REQUIRE(y == 0);
}

TEST_CASE("UndoManager empty transaction end and cancel are no-ops", "[state][undo]") {
    UndoManager um;
    int change_count = 0;
    um.on_state_changed = [&] { ++change_count; };

    um.begin_transaction("Empty end");
    um.end_transaction();
    REQUIRE_FALSE(um.can_undo());
    REQUIRE(um.undo_count() == 0);
    REQUIRE(change_count == 0);

    um.begin_transaction("Empty cancel");
    um.cancel_transaction();
    REQUIRE_FALSE(um.can_undo());
    REQUIRE(um.undo_count() == 0);
    REQUIRE(change_count == 0);
}

TEST_CASE("UndoManager cancel_transaction reverts", "[state][undo]") {
    UndoManager um;
    int value = 0;

    um.begin_transaction("Cancelled");
    um.perform(UndoAction::create("Set", [&] { value = 0; }, [&] { value = 42; }));
    REQUIRE(value == 42);

    um.cancel_transaction();
    REQUIRE(value == 0); // reverted
    REQUIRE_FALSE(um.can_undo()); // not added to history
}

TEST_CASE("UndoManager undo_name and redo_name", "[state][undo]") {
    UndoManager um;
    int v = 0;

    REQUIRE(um.undo_name().empty());

    um.perform(UndoAction::create("Change color",
        [&] { v = 0; }, [&] { v = 1; }));

    REQUIRE(um.undo_name() == "Change color");

    um.undo();
    REQUIRE(um.redo_name() == "Change color");
}

TEST_CASE("UndoManager max_history trims oldest", "[state][undo]") {
    UndoManager um;
    um.set_max_history(3);
    int v = 0;

    for (int i = 1; i <= 5; ++i) {
        int old = v;
        int next = i;
        um.perform(UndoAction::create("Step " + std::to_string(i),
            [&v, old] { v = old; },
            [&v, next] { v = next; }));
    }

    REQUIRE(um.undo_count() == 3); // only last 3 kept
}

TEST_CASE("UndoManager zero max history performs without retaining undo",
          "[state][undo][coverage][phase3]") {
    UndoManager um;
    um.set_max_history(0);
    int state_changes = 0;
    int value = 0;
    um.on_state_changed = [&] { ++state_changes; };

    um.perform(UndoAction::create("Transient",
        [&] { value = 0; },
        [&] { value = 5; }));

    REQUIRE(value == 5);
    REQUIRE(um.max_history() == 0);
    REQUIRE(um.undo_count() == 0);
    REQUIRE_FALSE(um.can_undo());
    REQUIRE_FALSE(um.undo());
    REQUIRE(state_changes == 1);
}

TEST_CASE("UndoManager lowering max_history trims existing history",
          "[state][undo][coverage][phase3]") {
    UndoManager um;
    int v = 0;

    for (int i = 1; i <= 4; ++i) {
        const int old = v;
        const int next = i;
        um.perform(UndoAction::create("Step " + std::to_string(i),
            [&v, old] { v = old; },
            [&v, next] { v = next; }));
    }

    REQUIRE(um.undo_count() == 4);
    REQUIRE(um.undo_name() == "Step 4");

    um.set_max_history(2);

    REQUIRE(um.undo_count() == 2);
    REQUIRE(um.undo_name() == "Step 4");
    REQUIRE(um.undo());
    REQUIRE(v == 3);
    REQUIRE(um.undo());
    REQUIRE(v == 2);
    REQUIRE_FALSE(um.can_undo());
}

TEST_CASE("UndoManager zero and negative max_history disable retention",
          "[state][undo][coverage][phase3]") {
    UndoManager um;
    int v = 0;

    um.perform(UndoAction::create("A", [&] { v = 0; }, [&] { v = 1; }));
    REQUIRE(um.undo_count() == 1);

    um.set_max_history(0);
    REQUIRE(um.max_history() == 0);
    REQUIRE_FALSE(um.can_undo());

    um.perform(UndoAction::create("B", [&] { v = 1; }, [&] { v = 2; }));
    REQUIRE(v == 2);
    REQUIRE_FALSE(um.can_undo());

    um.set_max_history(-10);
    REQUIRE(um.max_history() == 0);
    um.perform(UndoAction::create("C", [&] { v = 2; }, [&] { v = 3; }));
    REQUIRE(v == 3);
    REQUIRE_FALSE(um.can_undo());
}

TEST_CASE("UndoManager clear removes all history", "[state][undo]") {
    UndoManager um;
    int v = 0;

    um.perform(UndoAction::create("A", [&] { v = 0; }, [&] { v = 1; }));
    um.perform(UndoAction::create("B", [&] { v = 1; }, [&] { v = 2; }));

    um.clear();
    REQUIRE_FALSE(um.can_undo());
    REQUIRE_FALSE(um.can_redo());
    REQUIRE(um.undo_count() == 0);
}

TEST_CASE("UndoManager on_state_changed callback", "[state][undo]") {
    UndoManager um;
    int change_count = 0;
    um.on_state_changed = [&] { ++change_count; };

    int v = 0;
    um.perform(UndoAction::create("X", [&] { v = 0; }, [&] { v = 1; }));
    REQUIRE(change_count == 1);

    um.undo();
    REQUIRE(change_count == 2);

    um.redo();
    REQUIRE(change_count == 3);
}

TEST_CASE("UndoManager actions with missing callbacks are no-ops", "[state][undo]") {
    UndoManager um;
    int redo_count = 0;

    um.perform(UndoAction::create("Redo only",
        {},
        [&] { ++redo_count; }));

    REQUIRE(redo_count == 1);
    REQUIRE(um.can_undo());
    REQUIRE(um.undo());
    REQUIRE(redo_count == 1);
    REQUIRE(um.can_redo());
    REQUIRE(um.redo());
    REQUIRE(redo_count == 2);

    um.perform(UndoAction::create("Undo only",
        [&] { --redo_count; },
        {}));

    REQUIRE(redo_count == 2);
    REQUIRE(um.undo());
    REQUIRE(redo_count == 1);
}

TEST_CASE("UndoManager missing callbacks inside cancel_transaction are no-ops", "[state][undo]") {
    UndoManager um;
    int value = 0;

    um.begin_transaction("Partial callbacks");
    um.perform(UndoAction::create("Redo only",
        {},
        [&] { value = 7; }));
    um.perform(UndoAction::create("Undo only",
        [&] { value = 3; },
        {}));

    REQUIRE(value == 7);
    um.cancel_transaction();
    REQUIRE(value == 3);
    REQUIRE_FALSE(um.can_undo());
}

TEST_CASE("UndoManager add_without_executing", "[state][undo]") {
    UndoManager um;
    int v = 42; // already set

    um.add_without_executing(UndoAction::create("Already done",
        [&] { v = 0; },
        [&] { v = 42; }));

    REQUIRE(v == 42); // not re-executed
    REQUIRE(um.can_undo());

    um.undo();
    REQUIRE(v == 0);
}

TEST_CASE("UndoManager add_without_executing participates in transactions",
          "[state][undo][coverage][phase3]") {
    UndoManager um;
    int x = 10;
    int y = 20;
    int redo_count = 0;

    um.begin_transaction("Already moved");
    um.add_without_executing(UndoAction::create("X",
        [&] { x = 0; },
        [&] { x = 10; ++redo_count; }));
    um.add_without_executing(UndoAction::create("Y",
        [&] { y = 0; },
        [&] { y = 20; ++redo_count; }));
    um.end_transaction();

    REQUIRE(x == 10);
    REQUIRE(y == 20);
    REQUIRE(redo_count == 0);
    REQUIRE(um.undo_count() == 1);
    REQUIRE(um.undo_name() == "Already moved");

    REQUIRE(um.undo());
    REQUIRE(x == 0);
    REQUIRE(y == 0);
    REQUIRE(um.redo_count() == 1);
    REQUIRE(um.redo_name() == "Already moved");

    REQUIRE(um.redo());
    REQUIRE(x == 10);
    REQUIRE(y == 20);
    REQUIRE(redo_count == 2);
}

TEST_CASE("UndoManager add_without_executing mixes with performed transaction actions",
          "[state][undo][coverage][phase3]") {
    UndoManager um;
    std::vector<int> values;
    values.push_back(1);

    um.begin_transaction("Batch");
    um.add_without_executing(UndoAction::create("Already pushed",
        [&] { values.pop_back(); },
        [&] { values.push_back(1); }));
    um.perform(UndoAction::create("Push two",
        [&] { values.pop_back(); },
        [&] { values.push_back(2); }));
    um.end_transaction();

    REQUIRE(values == std::vector<int>{1, 2});
    REQUIRE(um.undo_count() == 1);
    REQUIRE(um.undo_name() == "Batch");

    REQUIRE(um.undo());
    REQUIRE(values.empty());
    REQUIRE(um.redo_name() == "Batch");

    REQUIRE(um.redo());
    REQUIRE(values == std::vector<int>{1, 2});
}

TEST_CASE("UndoManager committed transaction after undo clears redo history",
          "[state][undo][coverage][phase3]") {
    UndoManager um;
    int value = 0;

    um.perform(UndoAction::create("A", [&] { value = 0; }, [&] { value = 1; }));
    um.perform(UndoAction::create("B", [&] { value = 1; }, [&] { value = 2; }));

    REQUIRE(value == 2);
    REQUIRE(um.undo_count() == 2);
    REQUIRE(um.undo());
    REQUIRE(value == 1);
    REQUIRE(um.can_redo());
    REQUIRE(um.redo_count() == 1);

    um.begin_transaction("Batch");
    um.perform(UndoAction::create("C", [&] { value = 1; }, [&] { value = 3; }));
    um.perform(UndoAction::create("D", [&] { value = 3; }, [&] { value = 4; }));
    um.end_transaction();

    REQUIRE(value == 4);
    REQUIRE(um.undo_count() == 2);
    REQUIRE_FALSE(um.can_redo());
    REQUIRE(um.redo_count() == 0);
    REQUIRE(um.undo_name() == "Batch");

    REQUIRE(um.undo());
    REQUIRE(value == 1);
    REQUIRE(um.redo_name() == "Batch");
}

TEST_CASE("UndoManager add_without_executing outside transaction clears redo history",
          "[state][undo][coverage][phase3]") {
    UndoManager um;
    int value = 0;

    um.perform(UndoAction::create("A", [&] { value = 0; }, [&] { value = 1; }));
    um.perform(UndoAction::create("B", [&] { value = 1; }, [&] { value = 2; }));

    REQUIRE(um.undo());
    REQUIRE(value == 1);
    REQUIRE(um.can_redo());
    REQUIRE(um.redo_count() == 1);

    value = 3;
    um.add_without_executing(
        UndoAction::create("C", [&] { value = 1; }, [&] { value = 3; }));

    REQUIRE(value == 3);
    REQUIRE(um.can_undo());
    REQUIRE(um.undo_name() == "C");
    REQUIRE_FALSE(um.can_redo());
    REQUIRE(um.redo_count() == 0);

    REQUIRE(um.undo());
    REQUIRE(value == 1);
    REQUIRE(um.redo());
    REQUIRE(value == 3);
}

TEST_CASE("UndoManager open transaction publishes history only when ended",
          "[state][undo][coverage][phase3]") {
    UndoManager um;
    int value = 0;
    int state_changes = 0;
    um.on_state_changed = [&] { ++state_changes; };

    um.begin_transaction("Deferred");
    um.perform(UndoAction::create("A", [&] { value = 0; }, [&] { value = 1; }));
    um.perform(UndoAction::create("B", [&] { value = 1; }, [&] { value = 2; }));

    REQUIRE(value == 2);
    REQUIRE(um.undo_count() == 0);
    REQUIRE_FALSE(um.can_undo());
    REQUIRE(state_changes == 0);

    um.end_transaction();

    REQUIRE(um.undo_count() == 1);
    REQUIRE(um.can_undo());
    REQUIRE(um.undo_name() == "Deferred");
    REQUIRE(state_changes == 1);

    REQUIRE(um.undo());
    REQUIRE(value == 0);
    REQUIRE(state_changes == 2);
}

TEST_CASE("UndoManager max history trims whole transactions",
          "[state][undo][coverage][phase3]") {
    UndoManager um;
    um.set_max_history(2);
    int value = 0;

    for (int tx = 1; tx <= 3; ++tx) {
        um.begin_transaction("T" + std::to_string(tx));
        um.perform(UndoAction::create("first",
            [&value, tx] { value = tx - 1; },
            [&value, tx] { value = tx * 10; }));
        um.perform(UndoAction::create("second",
            [&value, tx] { value = tx - 1; },
            [&value, tx] { value = tx; }));
        um.end_transaction();
    }

    REQUIRE(um.undo_count() == 2);
    REQUIRE(um.undo_name() == "T3");
    REQUIRE(um.undo());
    REQUIRE(value == 2);
    REQUIRE(um.undo_name() == "T2");
    REQUIRE(um.undo());
    REQUIRE(value == 1);
    REQUIRE_FALSE(um.can_undo());
}

TEST_CASE("UndoManager multiple undo/redo sequence", "[state][undo]") {
    UndoManager um;
    int v = 0;

    um.perform(UndoAction::create("1", [&] { v = 0; }, [&] { v = 1; }));
    um.perform(UndoAction::create("2", [&] { v = 1; }, [&] { v = 2; }));
    um.perform(UndoAction::create("3", [&] { v = 2; }, [&] { v = 3; }));

    REQUIRE(v == 3);
    um.undo(); REQUIRE(v == 2);
    um.undo(); REQUIRE(v == 1);
    um.redo(); REQUIRE(v == 2);
    um.undo(); REQUIRE(v == 1);
    um.undo(); REQUIRE(v == 0);
    REQUIRE_FALSE(um.can_undo());
}

TEST_CASE("UndoManager transaction redo preserves action order",
          "[state][undo][coverage][phase3-large]") {
    UndoManager um;
    std::vector<int> events;

    um.begin_transaction("Ordered");
    um.perform(UndoAction::create("A",
        [&] { events.push_back(-1); },
        [&] { events.push_back(1); }));
    um.perform(UndoAction::create("B",
        [&] { events.push_back(-2); },
        [&] { events.push_back(2); }));
    um.end_transaction();

    REQUIRE(events == std::vector<int>{1, 2});
    REQUIRE(um.undo_name() == "Ordered");

    REQUIRE(um.undo());
    REQUIRE(events == std::vector<int>{1, 2, -2, -1});
    REQUIRE(um.redo_name() == "Ordered");

    REQUIRE(um.redo());
    REQUIRE(events == std::vector<int>{1, 2, -2, -1, 1, 2});
}

TEST_CASE("UndoManager clear notifies even when history is empty",
          "[state][undo][coverage][phase3-large]") {
    UndoManager um;
    int changes = 0;
    um.on_state_changed = [&] { ++changes; };

    um.clear();

    REQUIRE(changes == 1);
    REQUIRE_FALSE(um.can_undo());
    REQUIRE_FALSE(um.can_redo());
    REQUIRE(um.undo_name().empty());
    REQUIRE(um.redo_name().empty());
}

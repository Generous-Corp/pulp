#include <catch2/catch_test_macros.hpp>
#include <pulp/view/drag_drop.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/view_pool.hpp>

#include <functional>
#include <memory>
#include <string>

using namespace pulp::view;

namespace {

DropData make_text(std::string text) {
    DropData data;
    data.type = DropData::Type::text;
    data.text = std::move(text);
    return data;
}

}  // namespace

TEST_CASE("external drag session ignores a receiver destroyed by realm replacement",
          "[view][dnd][runtime-eval]") {
    class RetiredReceiver : public View, public DropReceiver {
    public:
        explicit RetiredReceiver(int& leaves) : leaves_(leaves) {}
        bool accept_drag(const DropData&, Point) override { return true; }
        void leave_drag() override { ++leaves_; }
        bool accept_drop(const DropData&, Point) override { return true; }
    private:
        int& leaves_;
    };

    View root;
    root.set_bounds({0, 0, 100, 100});
    int leaves = 0;
    auto receiver = std::make_unique<RetiredReceiver>(leaves);
    receiver->set_bounds({0, 0, 100, 100});
    auto* raw = receiver.get();
    root.add_child(std::move(receiver));

    DragSession session;
    REQUIRE(dispatch_drag_enter(root, session, make_text("payload"), {5, 5}));
    REQUIRE(session.hover != nullptr);

    auto retired = root.remove_child(raw);
    retired.reset();
    dispatch_drag_exit(root, session);

    CHECK(leaves == 0);
    CHECK(session.hover == nullptr);
}

TEST_CASE("aggregate-initialized DragSession preserves its leave notification",
          "[view][dnd][lifetime]") {
    class Receiver : public View, public DropReceiver {
    public:
        bool accept_drop(const DropData&, Point) override { return false; }
        void leave_drag() override { ++leaves; }
        int leaves = 0;
    };

    View root;
    auto receiver = std::make_unique<Receiver>();
    auto* raw = receiver.get();
    root.add_child(std::move(receiver));
    DragSession session{raw};

    dispatch_drag_exit(root, session);
    CHECK(raw->leaves == 1);
    CHECK(session.hover == nullptr);
}

TEST_CASE("designated DragSession initialization preserves the public hover API",
          "[view][dnd][lifetime][compatibility]") {
    class Receiver : public View, public DropReceiver {
    public:
        bool accept_drop(const DropData&, Point) override { return false; }
        void leave_drag() override { ++leaves; }
        int leaves = 0;
    };

    View root;
    auto receiver = std::make_unique<Receiver>();
    auto* raw = receiver.get();
    root.add_child(std::move(receiver));
    DragSession session{.hover = raw};
    auto* inferred = session.hover;
    DropReceiver** pointer_slot = &session.hover;
    DropReceiver*& pointer_alias = session.hover;
    CHECK(inferred == raw);
    CHECK(*pointer_slot == raw);
    CHECK(pointer_alias == raw);
    CHECK(&*session.hover == raw);

    dispatch_drag_exit(root, session);
    CHECK(raw->leaves == 1);
    CHECK(session.hover == nullptr);
}

TEST_CASE("invalidated drag identity rejects same-address allocator reuse",
          "[view][dnd][lifetime][compatibility]") {
    class Receiver : public View, public DropReceiver {
    public:
        bool accept_drag(const DropData&, Point) override { return true; }
        bool accept_drop(const DropData&, Point) override { return false; }
        void leave_drag() override { ++leaves; }
        int leaves = 0;
    };

    View root;
    auto receiver = std::make_unique<Receiver>();
    auto* raw = receiver.get();
    raw->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(receiver));

    DragSession session;
    REQUIRE(dispatch_drag_enter(root, session, make_text("payload"), {5, 5}));
    REQUIRE(session.hover == raw);
    // Simulates pooled address reuse: detach, rotate the logical identity, then
    // reattach the same allocation before the stale drag session is closed.
    auto reused = root.remove_child(raw);
    REQUIRE(reused != nullptr);
    raw->prepare_for_reuse();
    raw->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(reused));

    dispatch_drag_exit(root, session);
    CHECK(raw->leaves == 0);
    CHECK(session.hover == nullptr);
}

TEST_CASE("drag enter survives a receiver detaching itself from its callback",
          "[view][dnd][runtime-eval]") {
    class DetachingReceiver : public View, public DropReceiver {
    public:
        std::function<void()> detach;
        bool accept_drag(const DropData&, Point) override {
            if (detach) detach();
            return true;
        }
        bool accept_drop(const DropData&, Point) override { return false; }
    };

    View root;
    root.set_bounds({0, 0, 100, 100});
    auto receiver = std::make_unique<DetachingReceiver>();
    auto* raw = receiver.get();
    raw->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(receiver));
    std::unique_ptr<View> retired;
    raw->detach = [&] { retired = root.remove_child(raw); };

    DragSession session;
    CHECK(dispatch_drag_enter(root, session, make_text("payload"), {5, 5}));
    CHECK(session.hover == nullptr);
}

TEST_CASE("reentrant leave preserves the nested hover owner",
          "[view][dnd][reentrancy]") {
    class ReentrantReceiver : public View, public DropReceiver {
    public:
        bool accept_drag(const DropData&, Point) override {
            ++enters;
            return true;
        }
        bool accept_drop(const DropData&, Point) override { return false; }
        void leave_drag() override {
            ++leaves;
            auto callback = on_leave;
            if (callback) callback();
        }
        int enters = 0;
        int leaves = 0;
        std::function<void()> on_leave;
    };

    View root;
    root.set_bounds({0, 0, 300, 100});
    auto make_receiver = [&](float x) {
        auto receiver = std::make_unique<ReentrantReceiver>();
        receiver->set_bounds({x, 0, 100, 100});
        auto* raw = receiver.get();
        root.add_child(std::move(receiver));
        return raw;
    };
    auto* a = make_receiver(0);
    auto* b = make_receiver(100);
    auto* c = make_receiver(200);

    DragSession session;
    const auto data = make_text("payload");
    REQUIRE(dispatch_drag_enter(root, session, data, {10, 10}));
    REQUIRE(session.hover == a);
    a->on_leave = [&] {
        dispatch_drag_enter(root, session, data, {210, 10});
    };

    REQUIRE(dispatch_drag_enter(root, session, data, {110, 10}));

    CHECK(session.hover == c);
    CHECK(a->leaves == 1);
    CHECK(b->enters == 1);
    CHECK(b->leaves == 1);
    CHECK(c->enters == 1);
    CHECK(c->leaves == 0);
}

TEST_CASE("reentrant leave aborts the stale outer drop",
          "[view][dnd][reentrancy]") {
    class ReentrantReceiver : public View, public DropReceiver {
    public:
        bool accept_drag(const DropData&, Point) override { return true; }
        bool accept_drop(const DropData&, Point) override {
            ++drops;
            return true;
        }
        void leave_drag() override {
            auto callback = on_leave;
            if (callback) callback();
        }
        int drops = 0;
        std::function<void()> on_leave;
    };

    View root;
    root.set_bounds({0, 0, 300, 100});
    auto make_receiver = [&](float x) {
        auto receiver = std::make_unique<ReentrantReceiver>();
        receiver->set_bounds({x, 0, 100, 100});
        auto* raw = receiver.get();
        root.add_child(std::move(receiver));
        return raw;
    };
    auto* first = make_receiver(0);
    auto* nested = make_receiver(200);
    int root_drops = 0;
    root.on_drop = [&](const std::string&, const std::string&, float, float) {
        ++root_drops;
    };

    DragSession session;
    const auto data = make_text("payload");
    REQUIRE(dispatch_drag_enter(root, session, data, {10, 10}));
    first->on_leave = [&] {
        dispatch_drag_enter(root, session, data, {210, 10});
    };

    CHECK_FALSE(dispatch_drop(root, session, data, {150, 10}));
    CHECK(session.hover == nested);
    CHECK(first->drops == 0);
    CHECK(root_drops == 0);
}

TEST_CASE("reentrant accept preserves the nested hover owner",
          "[view][dnd][reentrancy]") {
    class ReentrantReceiver : public View, public DropReceiver {
    public:
        bool accept_drag(const DropData&, Point) override {
            ++enters;
            auto callback = on_accept;
            if (callback) callback();
            return true;
        }
        bool accept_drop(const DropData&, Point) override { return false; }
        void leave_drag() override { ++leaves; }
        int enters = 0;
        int leaves = 0;
        std::function<void()> on_accept;
    };

    View root;
    root.set_bounds({0, 0, 300, 100});
    auto make_receiver = [&](float x) {
        auto receiver = std::make_unique<ReentrantReceiver>();
        receiver->set_bounds({x, 0, 100, 100});
        auto* raw = receiver.get();
        root.add_child(std::move(receiver));
        return raw;
    };
    auto* a = make_receiver(0);
    auto* b = make_receiver(100);
    auto* c = make_receiver(200);

    DragSession session;
    const auto data = make_text("payload");
    REQUIRE(dispatch_drag_enter(root, session, data, {10, 10}));
    REQUIRE(session.hover == a);
    b->on_accept = [&] {
        dispatch_drag_enter(root, session, data, {210, 10});
    };

    REQUIRE(dispatch_drag_enter(root, session, data, {110, 10}));

    CHECK(session.hover == c);
    CHECK(a->leaves == 1);
    CHECK(b->enters == 1);
    CHECK(b->leaves == 1);
    CHECK(c->enters == 1);
    CHECK(c->leaves == 0);
}

TEST_CASE("reentrant exit during first accept cancels the outer hover",
          "[view][dnd][reentrancy]") {
    class CancellingReceiver : public View, public DropReceiver {
    public:
        DragSession* session = nullptr;
        View* root = nullptr;
        bool accept_drag(const DropData&, Point) override {
            ++enters;
            dispatch_drag_exit(*root, *session);
            return true;
        }
        bool accept_drop(const DropData&, Point) override { return false; }
        void leave_drag() override { ++leaves; }
        int enters = 0;
        int leaves = 0;
    };

    View root;
    root.set_bounds({0, 0, 100, 100});
    auto receiver = std::make_unique<CancellingReceiver>();
    auto* raw = receiver.get();
    raw->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(receiver));

    DragSession session;
    raw->session = &session;
    raw->root = &root;

    REQUIRE(dispatch_drag_enter(root, session, make_text("payload"), {10, 10}));
    CHECK(session.hover == nullptr);
    CHECK(session.hover == nullptr);
    CHECK(raw->enters == 1);
    CHECK(raw->leaves == 1);
}

TEST_CASE("reentrant exit after a rejected accept skips stale ancestors",
          "[view][dnd][reentrancy]") {
    class ExitingChild : public View, public DropReceiver {
    public:
        DragSession* session = nullptr;
        View* root = nullptr;
        bool accept_drag(const DropData&, Point) override {
            dispatch_drag_exit(*root, *session);
            return false;
        }
        bool accept_drop(const DropData&, Point) override { return false; }
    };
    class CountingRoot : public View, public DropReceiver {
    public:
        bool accept_drag(const DropData&, Point) override {
            ++accepts;
            return true;
        }
        bool accept_drop(const DropData&, Point) override { return false; }
        int accepts = 0;
    } root;

    root.set_bounds({0, 0, 100, 100});
    auto child = std::make_unique<ExitingChild>();
    auto* raw = child.get();
    raw->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(child));
    DragSession session;
    raw->session = &session;
    raw->root = &root;

    CHECK_FALSE(dispatch_drag_enter(root, session, make_text("payload"),
                                    {10, 10}));
    CHECK(root.accepts == 0);
    CHECK(session.hover == nullptr);
}

TEST_CASE("direct hover replacement during accept is revisioned",
          "[view][dnd][reentrancy][compatibility]") {
    class ReassigningReceiver : public View, public DropReceiver {
    public:
        DragSession* session = nullptr;
        DropReceiver* replacement = nullptr;
        bool accept_drag(const DropData&, Point) override {
            session->hover = replacement;
            return true;
        }
        bool accept_drop(const DropData&, Point) override { return false; }
        void leave_drag() override { ++leaves; }
        int leaves = 0;
    };
    class Receiver : public View, public DropReceiver {
    public:
        bool accept_drop(const DropData&, Point) override { return false; }
    };

    View root;
    root.set_bounds({0, 0, 200, 100});
    auto source = std::make_unique<ReassigningReceiver>();
    auto* source_raw = source.get();
    source->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(source));
    auto replacement = std::make_unique<Receiver>();
    auto* replacement_raw = replacement.get();
    replacement->set_bounds({100, 0, 100, 100});
    root.add_child(std::move(replacement));

    DragSession session;
    source_raw->session = &session;
    source_raw->replacement = replacement_raw;

    REQUIRE(dispatch_drag_enter(root, session, make_text("payload"), {10, 10}));
    CHECK(session.hover == replacement_raw);
    CHECK(source_raw->leaves == 1);
}

TEST_CASE("direct hover assignment cannot revive same-address view reuse",
          "[view][dnd][lifetime][compatibility]") {
    class ReusableReceiver : public View, public DropReceiver {
    public:
        bool supports_reuse() const override { return true; }
        bool accept_drag(const DropData&, Point) override { return true; }
        bool accept_drop(const DropData&, Point) override { return false; }
        void leave_drag() override { ++leaves; }
        int leaves = 0;
    };

    View root;
    root.set_bounds({0, 0, 100, 100});
    ViewPool pool;
    auto receiver = pool.acquire<ReusableReceiver>(
        [] { return std::make_unique<ReusableReceiver>(); });
    auto* allocation = receiver.get();
    receiver->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(receiver));

    DragSession session;
    REQUIRE(dispatch_drag_enter(root, session, make_text("payload"), {10, 10}));
    REQUIRE(session.hover == allocation);

    pool.release(root.remove_child(allocation));
    auto replacement = pool.acquire<ReusableReceiver>(
        [] { return std::make_unique<ReusableReceiver>(); });
    REQUIRE(replacement.get() == allocation);
    replacement->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(replacement));

    // Reassigning the identical pointer value is unobservable. Safety wins:
    // only a distinct public value can be imported as a new logical owner.
    session.hover = allocation;
    dispatch_drag_exit(root, session);

    CHECK(allocation->leaves == 0);
    CHECK(session.hover == nullptr);
}

TEST_CASE("reentrant rejected drop does not continue into stale ancestors",
          "[view][dnd][reentrancy]") {
    class ReentrantDropReceiver : public View, public DropReceiver {
    public:
        std::function<void()> on_drop;
        bool accept_drop(const DropData&, Point) override {
            if (on_drop) on_drop();
            return false;
        }
    };
    class HoverReceiver : public View, public DropReceiver {
    public:
        bool accept_drag(const DropData&, Point) override { return true; }
        bool accept_drop(const DropData&, Point) override { return false; }
    };

    View root;
    root.set_bounds({0, 0, 300, 100});
    int ancestor_drops = 0;
    root.on_drop = [&](const std::string&, const std::string&, float, float) {
        ++ancestor_drops;
    };
    auto drop_target = std::make_unique<ReentrantDropReceiver>();
    auto* drop_raw = drop_target.get();
    drop_target->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(drop_target));
    auto nested_target = std::make_unique<HoverReceiver>();
    auto* nested_raw = nested_target.get();
    nested_target->set_bounds({200, 0, 100, 100});
    root.add_child(std::move(nested_target));

    DragSession session;
    const auto data = make_text("payload");
    drop_raw->on_drop = [&] {
        dispatch_drag_enter(root, session, data, {210, 10});
    };

    CHECK_FALSE(dispatch_drop(root, session, data, {10, 10}));
    CHECK(session.hover == nested_raw);
    CHECK(ancestor_drops == 0);
}

TEST_CASE("drop continues through a captured ancestor after child detaches",
          "[view][dnd][runtime-eval]") {
    class DetachingReceiver : public View, public DropReceiver {
    public:
        std::function<void()> detach;
        bool accept_drop(const DropData&, Point) override {
            if (detach) detach();
            return false;
        }
    };

    View root;
    root.set_bounds({0, 0, 100, 100});
    int ancestor_drops = 0;
    root.on_drop = [&](const std::string&, const std::string&, float, float) {
        ++ancestor_drops;
    };
    auto receiver = std::make_unique<DetachingReceiver>();
    auto* raw = receiver.get();
    raw->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(receiver));
    std::unique_ptr<View> retired;
    raw->detach = [&] { retired = root.remove_child(raw); };

    DragSession session;
    CHECK(dispatch_drop(root, session, make_text("payload"), {5, 5}));
    CHECK(ancestor_drops == 1);
}

// The View mutation/lifetime contract: identity across callbacks, transactional
// attach, root-owned retirement, and exactly-once frame-clock propagation.
// Contract: core/view/include/pulp/view/view_lifecycle.hpp.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/view_lifecycle.hpp>
#include <pulp/view/window_host.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

using namespace pulp::view;

namespace {

// ── A View whose storage address is deterministically recycled ──────────────
//
// The ABA hazard needs a NEW view to land at the exact address a destroyed one
// occupied. Real allocators usually do that, but "usually" is not a test. This
// class-level operator new/delete keeps freed blocks on a LIFO free list, so
// the next construction provably reuses the last destroyed slot and the test
// can assert that it did.
struct RecycledView : View {
    static std::vector<void*>& free_list() {
        static std::vector<void*> blocks;
        return blocks;
    }
    static std::vector<void*>& all_blocks() {
        static std::vector<void*> blocks;
        return blocks;
    }
    static void* operator new(std::size_t size) {
        auto& free_blocks = free_list();
        if (!free_blocks.empty()) {
            void* reused = free_blocks.back();
            free_blocks.pop_back();
            return reused;
        }
        void* fresh = ::operator new(size);
        all_blocks().push_back(fresh);
        return fresh;
    }
    static void operator delete(void* block) noexcept {
        if (block) free_list().push_back(block);
    }
    // Hand every block back to the real allocator so the recycling pool is not
    // reported as a leak by a sanitizer build.
    static void release_pool() {
        for (void* block : all_blocks()) ::operator delete(block);
        all_blocks().clear();
        free_list().clear();
    }
};

// Records every lifecycle event and can run one scripted mutation. Uses the
// recycling allocator so a test can tell "this storage was freed and handed
// out again" from "this storage is still held".
struct Probe : RecycledView {
    int attaches = 0;
    int detaches = 0;
    int clock_notifications = 0;
    int* destructions = nullptr;
    std::function<void(Probe&)> on_clock;
    std::function<void(Probe&)> on_attach;

    ~Probe() override {
        if (destructions) ++*destructions;
    }
    void on_attached() override {
        ++attaches;
        if (on_attach) {
            auto action = std::move(on_attach);
            on_attach = nullptr;
            action(*this);
        }
    }
    void on_detached() override { ++detaches; }
    void on_frame_clock_changed() override {
        ++clock_notifications;
        if (on_clock) {
            auto action = std::move(on_clock);
            on_clock = nullptr;
            action(*this);
        }
    }
};

// Rejects the attach from inside a virtual host setter — the propagation step
// that used to run BEFORE the transaction's try block.
struct RejectsWindowHost : View {
    void set_window_host(WindowHost* host) override {
        if (host) throw std::runtime_error("host propagation rejected");
        View::set_window_host(host);
    }
};

} // namespace

TEST_CASE("the recycling allocator really does reuse a freed address",
          "[view][lifecycle][aba]") {
    // POSITIVE CONTROL for the test below. If this fails, the allocator is not
    // recycling and the "address was not reused" assertion downstream would
    // pass for the wrong reason — a blind oracle rather than a green one.
    {
        View parent;
        auto first = std::make_unique<RecycledView>();
        void* first_address = first.get();
        parent.add_child(std::move(first));
        auto owned = parent.remove_child(static_cast<View*>(first_address));
        REQUIRE(owned != nullptr);
        owned.reset();  // no lease open, so this really frees the block

        auto second = std::make_unique<RecycledView>();
        REQUIRE(static_cast<void*>(second.get()) == first_address);
    }
    RecycledView::release_pool();
}

TEST_CASE("a view retired mid-callback keeps its storage, so no sibling can "
          "land on its address",
          "[view][lifecycle][aba]") {
    // This is the property that actually closes the ABA hole. Identity is
    // address + import_binding_instance_id() at every re-find, but the stronger
    // guarantee is that while a callback is running on a view, that view's
    // storage cannot be handed to anyone else at all.
    //
    // Note the limit of the id check, so nobody reads more into it than it
    // gives: remove_child reads the id from the pointer it is handed, so a
    // caller whose pointer went stale BEFORE the call cannot be detected — a
    // raw View* carries no identity. Hold a ViewCapture across a callback
    // instead. The id check defends the case the contract does promise: the
    // identity at a slot changing DURING the call.
    int destructions = 0;
    void* retired_address = nullptr;
    void* sibling_address = nullptr;
    {
        View root;
        auto probe = std::make_unique<Probe>();
        probe->destructions = &destructions;
        Probe* raw = probe.get();
        retired_address = raw;
        raw->on_clock = [&](Probe& self) {
            View* parent = self.parent();
            REQUIRE(parent != nullptr);
            root.retire(parent->remove_child(&self));
            // Allocate a fresh view while the retired one is still parked. The
            // recycling allocator hands back the most recently freed block, so
            // if retirement had freed `self` this would land on its address.
            auto sibling = std::make_unique<Probe>();
            sibling_address = sibling.get();
            root.add_child(std::move(sibling));
        };
        root.add_child(std::move(probe));

        FrameClock clock;
        root.set_frame_clock(&clock);
        root.set_frame_clock(nullptr);

        REQUIRE(sibling_address != nullptr);
        REQUIRE(sibling_address != retired_address);
        REQUIRE(destructions == 1);
    }
    RecycledView::release_pool();
}

TEST_CASE("a throwing host setter leaves no half-linked child",
          "[view][lifecycle][attach]") {
    View parent;
    std::unique_ptr<View> child = std::make_unique<RejectsWindowHost>();
    View* raw = child.get();
    // The parent must have a host for propagation to reach the throwing setter.
    parent.set_window_host(reinterpret_cast<WindowHost*>(0x1));

    REQUIRE_THROWS_AS(parent.add_child_transactional(child), std::runtime_error);

    // Ownership came back to the caller, and nothing about the child claims it
    // is attached. Before the fix the setters ran outside the transaction, so a
    // throw left parent_ pointing at `parent` with no unwind.
    REQUIRE(child != nullptr);
    REQUIRE(child.get() == raw);
    REQUIRE(child->parent() == nullptr);
    REQUIRE(parent.child_count() == 0);
    parent.set_window_host(nullptr);
}

TEST_CASE("a view retired from inside its own callback outlives that callback",
          "[view][lifecycle][retire]") {
    int destructions = 0;
    int destructions_seen_inside_callback = -1;
    int clock_notifications_after_retire = -1;

    View root;
    auto probe = std::make_unique<Probe>();
    probe->destructions = &destructions;
    Probe* raw = probe.get();
    raw->on_clock = [&](Probe& self) {
        // Remove and retire the very view whose callback is running.
        View* parent = self.parent();
        REQUIRE(parent != nullptr);
        auto owned = parent->remove_child(&self);
        REQUIRE(owned != nullptr);
        root.retire(std::move(owned));
        // Still alive: the lease defers destruction to the end of the pass.
        destructions_seen_inside_callback = destructions;
        clock_notifications_after_retire = self.clock_notifications;
    };
    root.add_child(std::move(probe));

    FrameClock clock;
    root.set_frame_clock(&clock);

    REQUIRE(destructions_seen_inside_callback == 0);
    // Touching the retired view's own state after retiring it must be safe.
    //
    // TWO notifications, not one, and that is correct: the first is this walk,
    // the second is remove_child telling the now-detached subtree its clock went
    // away — which is the whole reason remove_child refreshes it. Counting one
    // here would mean the detached refresh had been swallowed.
    REQUIRE(clock_notifications_after_retire == 2);
    // ...and the deferral must actually end. A lease that never lowers its
    // depth would leave this at 0.
    REQUIRE(destructions == 1);
    REQUIRE(root.child_count() == 0);
    root.set_frame_clock(nullptr);
}

TEST_CASE("retire outside any callback destroys immediately",
          "[view][lifecycle][retire]") {
    int destructions = 0;
    View root;
    auto probe = std::make_unique<Probe>();
    probe->destructions = &destructions;
    Probe* raw = probe.get();
    root.add_child(std::move(probe));

    auto owned = root.remove_child(raw);
    REQUIRE(owned != nullptr);
    root.retire(std::move(owned));
    REQUIRE(destructions == 1);
}

TEST_CASE("frame-clock propagation visits every original child exactly once",
          "[view][lifecycle][frame-clock]") {
    View root;
    FrameClock clock;

    std::vector<Probe*> kids;
    for (int i = 0; i < 4; ++i) {
        auto probe = std::make_unique<Probe>();
        kids.push_back(probe.get());
        root.add_child(std::move(probe));
    }
    Probe* a = kids[0];
    Probe* b = kids[1];
    Probe* c = kids[2];
    Probe* d = kids[3];

    // The documented counterexample: while B's hook is running (slot 1), remove
    // BOTH A and B. children_ becomes [C, D], so slot 1 now holds D. An index
    // walker that re-reads slot 1 visits D next and then runs out of its
    // bounded count, silently never notifying C.
    b->on_clock = [&](Probe& self) {
        root.retire(root.remove_child(a));
        root.retire(root.remove_child(&self));
    };

    root.set_frame_clock(&clock);

    // A and B are REMOVED by the hook, so each is notified twice: once by this
    // walk, once by remove_child refreshing the detached subtree whose clock is
    // now null. Asserting one here would be asserting that the detached refresh
    // never happened.
    REQUIRE(a->clock_notifications == 2);
    REQUIRE(b->clock_notifications == 2);
    // C and D are untouched bystanders. Exactly one each — no skip, no repeat.
    // This is the regression the contract exists to prevent: the old bounded
    // index walk never notified C at all.
    REQUIRE(c->clock_notifications == 1);
    REQUIRE(d->clock_notifications == 1);
    REQUIRE(root.child_count() == 2);
    root.set_frame_clock(nullptr);
}

TEST_CASE("frame-clock propagation does not revisit a reordered child",
          "[view][lifecycle][frame-clock]") {
    View root;
    FrameClock clock;

    std::vector<Probe*> kids;
    for (int i = 0; i < 3; ++i) {
        auto probe = std::make_unique<Probe>();
        kids.push_back(probe.get());
        root.add_child(std::move(probe));
    }
    // The first child moves the LAST child to the front of the vector by
    // detaching and re-attaching it. A walker that tracked position rather than
    // identity would deliver to it twice.
    kids[0]->on_clock = [&](Probe&) {
        auto moved = root.remove_child(kids[2]);
        REQUIRE(moved != nullptr);
        root.add_child(std::move(moved));
    };

    root.set_frame_clock(&clock);

    // The two untouched children see exactly one notification each — a walk
    // that tracked position rather than identity would have delivered to the
    // moved child twice within this pass and skipped one of these.
    REQUIRE(kids[0]->clock_notifications == 1);
    REQUIRE(kids[1]->clock_notifications == 1);
    // The moved child sees exactly two, and both are real clock changes rather
    // than a double visit: null when it was detached, then the clock again when
    // it was re-attached. It is never delivered twice for the same state.
    REQUIRE(kids[2]->clock_notifications == 2);
    root.set_frame_clock(nullptr);
}

TEST_CASE("a child attached during a frame-clock walk is notified exactly once",
          "[view][lifecycle][frame-clock]") {
    View root;
    FrameClock clock;

    auto first = std::make_unique<Probe>();
    Probe* a = first.get();
    root.add_child(std::move(first));

    Probe* late = nullptr;
    a->on_clock = [&](Probe&) {
        auto added = std::make_unique<Probe>();
        late = added.get();
        root.add_child(std::move(added));
    };

    root.set_frame_clock(&clock);

    REQUIRE(a->clock_notifications == 1);
    REQUIRE(late != nullptr);
    // Deferred out of the pass that was running when it was attached, but not
    // dropped: the follow-up pass delivers it, and only once.
    REQUIRE(late->clock_notifications == 1);
    root.set_frame_clock(nullptr);
}

TEST_CASE("a detached subtree is notified even when it inherits a root's stamps",
          "[view][lifecycle][frame-clock]") {
    // The regression this guards is a silent one, and the ordering matters:
    // the whole tree is assembled BEFORE the clock is installed, which is what
    // hosts actually do (set_frame_clock's own comment says so). Every node is
    // therefore stamped by the root's FIRST walk.
    //
    // remove_child then hands the detached subtree its own root and notifies it
    // so self-subscribing descendants drop a clock they can no longer reach.
    // With per-root counters that fresh root's first walk drew the SAME token
    // the old root's first walk had already written, so every descendant read
    // as already-visited and the notification was skipped entirely — leaving a
    // live subscription on a clock the subtree cannot see. Process-global
    // tokens make a stamp mean the same thing in every tree.
    View root;
    FrameClock clock;

    auto container = std::make_unique<Probe>();
    Probe* container_raw = container.get();
    auto leaf = std::make_unique<Probe>();
    Probe* leaf_raw = leaf.get();
    container_raw->add_child(std::move(leaf));
    root.add_child(std::move(container));

    root.set_frame_clock(&clock);
    // Control: the first walk must actually have stamped them, or the collision
    // this test exists for could never arise and the assertions below are void.
    REQUIRE(container_raw->clock_notifications == 1);
    REQUIRE(leaf_raw->clock_notifications == 1);
    REQUIRE(leaf_raw->frame_clock() == &clock);

    auto removed = root.remove_child(container_raw);
    REQUIRE(removed != nullptr);

    // Both the detached root AND its descendant must be told, exactly once.
    REQUIRE(container_raw->clock_notifications == 2);
    REQUIRE(leaf_raw->clock_notifications == 2);
    REQUIRE(leaf_raw->frame_clock() == nullptr);
    root.set_frame_clock(nullptr);
}

TEST_CASE("a hook that attaches on every notification still terminates",
          "[view][lifecycle][frame-clock]") {
    View root;
    FrameClock clock;

    auto first = std::make_unique<Probe>();
    Probe* a = first.get();
    root.add_child(std::move(first));

    // Every notification attaches another clock-aware child. Without the
    // deferral-plus-bounded-passes rule this walk would never end; the contract
    // trades completeness for liveness here on purpose.
    int spawned = 0;
    std::function<void(Probe&)> spawn = [&](Probe&) {
        if (spawned >= 64) return;
        ++spawned;
        auto added = std::make_unique<Probe>();
        added->on_clock = spawn;
        root.add_child(std::move(added));
    };
    a->on_clock = spawn;

    root.set_frame_clock(&clock);

    REQUIRE(a->clock_notifications == 1);
    REQUIRE(spawned > 0);
    root.set_frame_clock(nullptr);
}

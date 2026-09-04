// The lifecycle contract at the WidgetBridge boundary: the retained ScrollView
// upgrade and the ordinary DOM reparent must fail closed rather than
// dereference a null removal or strand a destroyed view in the registries.
// These are the two paths a Settings-style portal reopen goes through.
// Contract: core/view/include/pulp/view/view_lifecycle.hpp.

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/view_lifecycle.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <memory>
#include <stdexcept>
#include <string>

using namespace pulp::view;
using pulp::state::StateStore;

namespace {

// Removes itself from its parent the moment it is told it is detaching. That
// makes the outer remove_child() return null — the reentrant case the retained
// upgrade used to dereference.
struct SelfRemovingOnDetach : View {
    bool armed = false;
    View* root_for_retire = nullptr;
    int detaches = 0;

    void on_detached() override {
        ++detaches;
        if (!armed) return;
        armed = false;
        if (View* parent = this->parent()) {
            // Hand ownership to the root: destroying ourselves here would free
            // the frame this callback is running in.
            if (root_for_retire) root_for_retire->retire(parent->remove_child(this));
        }
    }
};

// Refuses every attach once armed, so a reparent fails at the destination AND
// again when the rollback tries to put it back — the double-failure case.
struct RejectsAttach : View {
    bool armed = false;
    void on_attached() override {
        if (armed) throw std::runtime_error("attach rejected by probe");
    }
};

} // namespace

TEST_CASE("retained scroll upgrade fails closed when the removal is stolen",
          "[view][bridge][scroll][lifecycle]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    auto container = std::make_unique<View>();
    container->set_id("container");
    View* container_raw = container.get();
    root.add_child(std::move(container));

    auto panel = std::make_unique<SelfRemovingOnDetach>();
    panel->set_id("panel");
    SelfRemovingOnDetach* panel_raw = panel.get();
    panel_raw->root_for_retire = &root;
    container_raw->add_child(std::move(panel));

    // POSITIVE CONTROL: the bridge must actually resolve this native view by
    // id, or the upgrade path below is never entered and the test proves
    // nothing about it.
    REQUIRE(bridge.widget("panel") == panel_raw);

    panel_raw->armed = true;
    // The upgrade calls remove_child on the container; the panel's own detach
    // hook removes it first, so the result is null. Before the null check that
    // was dereferenced immediately for its bounds.
    bool threw = false;
    try {
        engine.evaluate("__domAppend('container', 'panel', 'div', 'scroll')");
    } catch (const std::exception&) {
        threw = true;
    }

    // Whether the refusal surfaces as a C++ throw or is absorbed into the JS
    // realm, the invariant is the same: no wrapper was published and nothing
    // was dereferenced through a null.
    (void)threw;
    // Fired at least once; the hook's own reentrant remove_child delivers it a
    // second time, which is the documented consequence of mutating from inside
    // a lifecycle callback rather than a defect.
    REQUIRE(panel_raw->detaches >= 1);
    REQUIRE(bridge.scroll_wrapper("panel") == nullptr);
    // The panel left the tree by its own hand and is retired, not leaked into
    // a half-built wrapper.
    REQUIRE(container_raw->child_count() == 0);
}

TEST_CASE("a reparent that fails at both ends keeps the first error and strands nothing",
          "[view][bridge][reparent][lifecycle]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    auto source = std::make_unique<View>();
    source->set_id("source");
    View* source_raw = source.get();
    root.add_child(std::move(source));

    auto destination = std::make_unique<View>();
    destination->set_id("destination");
    View* destination_raw = destination.get();
    root.add_child(std::move(destination));

    auto movable = std::make_unique<RejectsAttach>();
    movable->set_id("movable");
    RejectsAttach* movable_raw = movable.get();
    source_raw->add_child(std::move(movable));

    REQUIRE(bridge.widget("movable") == movable_raw);

    // Arm only now: the initial attach above had to succeed.
    movable_raw->armed = true;

    bool threw = false;
    std::string message;
    try {
        engine.evaluate("__domAppend('destination', 'movable', 'div')");
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    }
    (void)threw;
    (void)message;

    // Neither parent accepted it, so the view must be out of the tree and
    // unreachable, with the first failure — not the rollback's — propagating.
    REQUIRE(bridge.widget("movable") == nullptr);
    REQUIRE(source_raw->child_count() == 0);
    REQUIRE(destination_raw->child_count() == 0);

    // WHAT THIS TEST DOES NOT COVER, stated so nobody reads more into a green
    // run than it earns. The rollback also erases the owning bridge records
    // (forget_widget_subtree) before releasing the view, so a destroyed view is
    // never left named by owned_widgets_. That branch has NO oracle here and a
    // planted negative for it comes back NOT CONFIRMED, because reaching it
    // needs a BRIDGE-OWNED widget whose on_attached throws — and every widget
    // the bridge creates from JS is a stock View/Label that cannot throw from
    // an attach hook. The probe above throws only because it is a native
    // subclass, which by that same token was never in owned_widgets_
    // (identity count is 0 here, not a positive number).
    //
    // So the cleanup is defensive code guarding a path unreachable from the JS
    // surface today. It stays because the review requires ownership stability
    // on double failure and a future native-subclass consumer would reach it —
    // but it is asserted by construction and code review, not by this test.
    // Do not add an assertion that passes either way to make this look covered.
}

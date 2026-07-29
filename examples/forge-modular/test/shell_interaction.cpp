// Does the shell actually DO anything when you click it?
//
// This file exists because the answer was no and nobody noticed. The shell
// rendered correctly in a screenshot and was completely inert: wire() casts
// each control to ToggleButton, the controls had been rebuilt as styled rows,
// every cast returned null, and hook() reported the failure by returning false
// to nobody.
//
// A screenshot is proof of paint. It says nothing about whether a control is
// connected to anything. So this drives the real widgets at real coordinates
// and asserts on what the engine was asked to do.

#include <catch2/catch_test_macros.hpp>

#include "forge_modular/shell.hpp"

#include <pulp/state/store.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

using pulp::view::Rect;
using pulp::view::View;

/// Records what the shell asked for instead of spawning a compiler.
struct RecordingEngine final : forge_modular::EngineClient {
    struct Call {
        std::string prompt;
        bool patch_mode = false;
    };
    std::vector<Call> calls;
    bool startable = true;
    int ensure_calls = 0;

    bool available() const override { return startable; }
    bool ensure_running() override { ++ensure_calls; return startable; }
    void submit(const std::string& prompt, bool patch_mode) override {
        calls.push_back({prompt, patch_mode});
    }
};

View* find_by_id(View& root, const std::string& id) {
    if (root.id() == id) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (auto* hit = find_by_id(*root.child_at(i), id)) return hit;
    }
    return nullptr;
}

Rect absolute_bounds(const View& view) {
    auto out = view.bounds();
    for (auto* p = view.parent(); p != nullptr; p = p->parent()) {
        out.x += p->bounds().x;
        out.y += p->bounds().y;
    }
    return out;
}

/// The shell with its view built and laid out, plus what a test needs to drive
/// it. The size matches the standalone's default so controls land where they
/// land in the real window.
struct Harness {
    pulp::state::StateStore store;
    RecordingEngine engine;
    forge_modular::Shell shell;
    std::unique_ptr<View> root;

    Harness() {
        shell.set_engine(&engine);
        shell.define_parameters(store);
        root = shell.create_view();
        REQUIRE(root != nullptr);
        root->set_bounds({0.0f, 0.0f, 1280.0f, 800.0f});
        root->layout_children();
    }

    View* control(const std::string& id) const {
        return root ? find_by_id(*root, id) : nullptr;
    }

    void type(const std::string& text) {
        auto* ed = dynamic_cast<pulp::view::TextEditor*>(control("prompt"));
        REQUIRE(ed != nullptr);
        ed->set_text(text);
    }

    /// Click through the root at the control's centre, the way a mouse would.
    /// Calling the handler directly would pass even for a control that is
    /// unreachable, invisible or zero-sized -- the exact class of bug this file
    /// exists to catch -- so the geometry is asserted before the click.
    void click(const std::string& id) {
        auto* v = control(id);
        INFO("control: " << id);
        REQUIRE(v != nullptr);
        const auto b = absolute_bounds(*v);
        REQUIRE(b.width > 0.0f);
        REQUIRE(b.height > 0.0f);
        root->simulate_click({b.x + b.width * 0.5f, b.y + b.height * 0.5f});
    }

    std::string label(const std::string& id) const {
        auto* l = dynamic_cast<pulp::view::Label*>(control(id));
        REQUIRE(l != nullptr);
        return l->text();
    }
};

}  // namespace

TEST_CASE("every control the shell paints is a real, reachable widget",
          "[forge-modular][shell][interaction]") {
    Harness h;

    // These are the ids wire() looks for. If one is not a ToggleButton the
    // cast fails and that control is dead -- silently, which is how it shipped.
    for (const char* id : {"btn-build", "btn-ask", "btn-random", "btn-mention",
                           "tab-module", "tab-patch",
                           "rail-home", "rail-module", "rail-patch",
                           "rail-settings"}) {
        auto* v = h.control(id);
        INFO("control: " << id);
        REQUIRE(v != nullptr);
        REQUIRE(dynamic_cast<pulp::view::ToggleButton*>(v) != nullptr);

        // A control nothing can hit is not a control.
        const auto b = absolute_bounds(*v);
        REQUIRE(b.width > 0.0f);
        REQUIRE(b.height > 0.0f);
    }
}

TEST_CASE("Build submits exactly what was typed, once",
          "[forge-modular][shell][interaction]") {
    Harness h;
    h.type("a 12 hp wavefolder with drive and symmetry");
    h.click("btn-build");

    REQUIRE(h.engine.calls.size() == 1);
    REQUIRE(h.engine.calls[0].prompt == "a 12 hp wavefolder with drive and symmetry");
}

TEST_CASE("Build on an empty composer submits nothing",
          "[forge-modular][shell][interaction]") {
    Harness h;
    h.click("btn-build");
    REQUIRE(h.engine.calls.empty());
}

TEST_CASE("Ask can never rewrite the artifact",
          "[forge-modular][shell][interaction]") {
    // Ask and Build differ in one bit and it is carried, not inferred. An Ask
    // turn able to rewrite a patch would destroy work on a misread intent, so
    // the flag is asserted rather than the label trusted.
    Harness h;
    h.type("why did you pick a wavefolder here");
    h.click("btn-ask");

    REQUIRE(h.engine.calls.size() == 1);
    REQUIRE(h.engine.calls[0].patch_mode == false);
}

TEST_CASE("a submit starts the engine rather than assuming it is up",
          "[forge-modular][shell][interaction]") {
    Harness h;
    h.engine.startable = false;
    h.type("a 12 hp wavefolder");
    h.click("btn-build");

    REQUIRE(h.engine.ensure_calls == 1);
    REQUIRE(h.engine.calls.empty());   // could not start, so nothing submitted
}

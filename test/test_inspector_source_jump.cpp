// Inspector source-jump tests: view provenance, overlay J hotkey, and
// DomainHandler config propagation.

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/view/inspector.hpp>

#include <cstdlib>
#include <string>

using namespace pulp::view;
using namespace pulp::inspect;

namespace {

// Intentionally local: keeps source-jump env state isolated to this test binary.
struct ScopedEnv {
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* prev = std::getenv(name_.c_str())) {
            prev_ = std::string(prev);
            had_prev_ = true;
        }
    }

    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), /*overwrite=*/1);
#endif
    }

    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }

    ~ScopedEnv() {
        if (had_prev_) set(prev_);
        else unset();
    }

private:
    std::string name_;
    std::string prev_;
    bool had_prev_ = false;
};

}  // namespace

#include <pulp/inspect/source_jump.hpp>
#include <choc/text/choc_JSON.h>

namespace {

// Scoped guard that sets PULP_INSPECTOR_NO_LAUNCH for the duration of a
// test, restoring the prior value on destruction. The overlay's J
// hotkey resolves with dry_run=false and would otherwise spawn a real
// editor (and pop the macOS open-confirmation dialog). The CTest target
// sets this env var globally, but a test that exercises the real-launch
// path sets it in-process too so it is safe when the binary is run
// directly. With the guard in place, launch_editor_url() never spawns a
// process and reports launched=false.
struct ScopedNoLaunch {
    ScopedNoLaunch() {
        if (const char* prev = std::getenv("PULP_INSPECTOR_NO_LAUNCH")) {
            prev_ = std::string(prev);
            had_prev_ = true;
        }
#if defined(_WIN32)
        _putenv_s("PULP_INSPECTOR_NO_LAUNCH", "1");
#else
        ::setenv("PULP_INSPECTOR_NO_LAUNCH", "1", /*overwrite=*/1);
#endif
    }
    ~ScopedNoLaunch() {
#if defined(_WIN32)
        _putenv_s("PULP_INSPECTOR_NO_LAUNCH", had_prev_ ? prev_.c_str() : "");
#else
        if (had_prev_) ::setenv("PULP_INSPECTOR_NO_LAUNCH", prev_.c_str(), 1);
        else ::unsetenv("PULP_INSPECTOR_NO_LAUNCH");
#endif
    }
    std::string prev_;
    bool had_prev_ = false;
};

} // namespace

TEST_CASE("View::source_loc round-trips a file:line:col record",
          "[inspect][source-jump]") {
    View v;
    REQUIRE_FALSE(v.has_source_loc());
    REQUIRE_FALSE(v.source_loc().valid());

    v.set_source_loc({"src/Synth.jsx", 42, 7});
    REQUIRE(v.has_source_loc());
    REQUIRE(v.source_loc().valid());
    REQUIRE(v.source_loc().file == "src/Synth.jsx");
    REQUIRE(v.source_loc().line == 42);
    REQUIRE(v.source_loc().col == 7);

    v.clear_source_loc();
    REQUIRE_FALSE(v.has_source_loc());
}

TEST_CASE("ViewInspector::find_by_anchor locates a view by its anchor id",
          "[inspect][source-jump]") {
    View root;
    auto a = std::make_unique<View>();
    a->set_anchor_id("figma:a");
    auto* a_ptr = a.get();
    auto b = std::make_unique<View>();
    b->set_anchor_id("figma:b");
    auto* b_ptr = b.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));

    REQUIRE(ViewInspector::find_by_anchor(root, "figma:a") == a_ptr);
    REQUIRE(ViewInspector::find_by_anchor(root, "figma:b") == b_ptr);
    REQUIRE(ViewInspector::find_by_anchor(root, "missing") == nullptr);
    // An empty anchor never matches a (default-empty-anchor) view.
    REQUIRE(ViewInspector::find_by_anchor(root, "") == nullptr);
}

TEST_CASE("InspectorOverlay: J jumps to source for a view with provenance",
          "[inspect][overlay][source-jump]") {
    // The J hotkey resolves with dry_run=false — the real-launch path.
    // Without the no-launch guard this would spawn `open vscode://...`
    // and pop the macOS open-confirmation dialog. The guard makes
    // launch_editor_url() a no-op so the test verifies the *constructed*
    // URL, never an actual editor launch.
    ScopedNoLaunch no_launch;

    View root;
    root.set_bounds({0, 0, 500, 300});
    auto child = std::make_unique<View>();
    child->set_id("knob");
    child->set_bounds({10, 10, 80, 40});
    child->set_source_loc({"src/Panel.jsx", 24, 3});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    // Select the child via a click.
    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE(overlay.selected_view() == child_ptr);

    // Dry-run jump — resolves the URL but never spawns the editor.
    auto result = overlay.jump_to_selection_source(/*dry_run=*/true);
    REQUIRE(result.ok);
    REQUIRE(result.url == "vscode://file/src/Panel.jsx:24");
    REQUIRE_FALSE(result.launched);

    // Real-launch path (dry_run=false), as the J hotkey runs it: the URL
    // still resolves, but the no-launch guard suppresses the spawn so
    // `launched` stays false. This is the regression assertion for the
    // unexpected-editor-launch bug.
    auto live = overlay.jump_to_selection_source(/*dry_run=*/false);
    REQUIRE(live.ok);
    REQUIRE(live.url == "vscode://file/src/Panel.jsx:24");
    REQUIRE_FALSE(live.launched);

    // launch_editor_url() itself is a no-op under the guard.
    REQUIRE_FALSE(launch_editor_url("vscode://file/src/Panel.jsx:24"));

    // The J hotkey is consumed while the inspector is active. The
    // handler runs the dry_run=false path; the guard keeps it from
    // spawning a real editor process.
    KeyEvent jk;
    jk.key = KeyCode::j;
    jk.modifiers = 0;
    jk.is_down = true;
    REQUIRE(overlay.handle_key_event(jk));
}

TEST_CASE("InspectorOverlay source jump helper defaults to dry-run",
          "[inspect][overlay][source-jump][issue-2515]") {
    ScopedEnv headless("PULP_HEADLESS");
    headless.set("1");

    View root;
    root.set_bounds({0, 0, 500, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 80, 40});
    child->set_source_loc({"src/Panel.jsx", 24, 3});
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);

    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    REQUIRE(overlay.handle_mouse_event(click));
    REQUIRE(overlay.selected_view() != nullptr);

    auto result = overlay.jump_to_selection_source();
    REQUIRE(result.ok);
    REQUIRE(result.url == "vscode://file/src/Panel.jsx:24");
    REQUIRE_FALSE(result.launched);
    REQUIRE(result.error.empty());
}

TEST_CASE("InspectorOverlay: J is a graceful no-op without a selection",
          "[inspect][overlay][source-jump]") {
    // No selection means jump_to_source resolves ok==false and never
    // reaches launch_editor_url(); the guard is belt-and-suspenders so
    // the key event below stays inert when the binary is run directly.
    ScopedNoLaunch no_launch;

    View root;
    InspectorOverlay overlay(root);
    overlay.set_active(true);
    REQUIRE(overlay.selected_view() == nullptr);

    auto result = overlay.jump_to_selection_source(/*dry_run=*/true);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.url.empty());

    // The key is still consumed (does not fall through to the view tree),
    // but no jump happens.
    KeyEvent jk;
    jk.key = KeyCode::j;
    jk.modifiers = 0;
    jk.is_down = true;
    REQUIRE(overlay.handle_key_event(jk));
}

TEST_CASE("InspectorOverlay: J is a graceful no-op for a non-imported view",
          "[inspect][overlay][source-jump]") {
    View root;
    root.set_bounds({0, 0, 500, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 80, 40});
    // deliberately NO set_source_loc — a user-authored, non-JSX view.
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    overlay.handle_mouse_event(click);
    REQUIRE(overlay.selected_view() != nullptr);

    auto result = overlay.jump_to_selection_source(/*dry_run=*/true);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("no source location") != std::string::npos);
}

TEST_CASE("InspectorOverlay: J does nothing when the inspector is inactive",
          "[inspect][overlay][source-jump]") {
    View root;
    InspectorOverlay overlay(root);
    // inspector NOT active
    KeyEvent jk;
    jk.key = KeyCode::j;
    jk.modifiers = 0;
    jk.is_down = true;
    REQUIRE_FALSE(overlay.handle_key_event(jk));
}

TEST_CASE("InspectorOverlay: config swap changes the source-jump template",
          "[inspect][overlay][source-jump]") {
    View root;
    root.set_bounds({0, 0, 500, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 80, 40});
    child->set_source_loc({"a/B.tsx", 11, 2});
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    overlay.set_active(true);
    MouseEvent click;
    click.position = {20, 20};
    click.is_down = true;
    overlay.handle_mouse_event(click);

    InspectorConfig cfg;
    cfg.editor_url_template = "zed://file/{path}:{line}:{col}";
    overlay.set_config(cfg);

    auto result = overlay.jump_to_selection_source(/*dry_run=*/true);
    REQUIRE(result.ok);
    REQUIRE(result.url == "zed://file/a/B.tsx:11:2");
}

TEST_CASE("DomainHandler propagates config to the attached overlay",
          "[inspect][source-jump][domain]") {
    View root;
    root.set_bounds({0, 0, 500, 300});
    auto child = std::make_unique<View>();
    child->set_anchor_id("anchor-1");
    child->set_bounds({10, 10, 80, 40});
    child->set_source_loc({"x/Y.jsx", 5, 0});
    root.add_child(std::move(child));

    InspectorOverlay overlay(root);
    DomainHandler handler;
    handler.set_root_view(&root);
    handler.set_overlay(&overlay);

    InspectorConfig cfg;
    cfg.editor_url_template = "cursor://file/{path}:{line}";
    handler.set_config(cfg);

    // The overlay's config now matches the handler's.
    REQUIRE(overlay.config().editor_url_template
            == "cursor://file/{path}:{line}");

    // And Inspector.jumpToSource via the anchor resolves with it.
    auto resp = handler.handle(make_request(
        1, methods::kInspectorJumpToSource,
        R"({"anchorId":"anchor-1","dryRun":true})"));
    REQUIRE_FALSE(resp.is_error);
    auto obj = choc::json::parse(resp.params_json);
    REQUIRE(obj["ok"].getBool());
    REQUIRE(std::string(obj["url"].getString())
            == "cursor://file/x/Y.jsx:5");
}

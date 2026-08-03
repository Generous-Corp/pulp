#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::canvas;
using namespace pulp::inspect;
using namespace pulp::view;

TEST_CASE("InspectorOverlay hooks route text and cursor events by root",
          "[inspect][overlay][text-tool][hooks]") {
    View inspected_root;
    inspected_root.set_bounds({0, 0, 400, 300});
    auto label = std::make_unique<Label>("Type here");
    label->set_anchor_id("figma:label-1");
    label->set_bounds({20, 20, 120, 30});
    auto* label_ptr = label.get();
    inspected_root.add_child(std::move(label));

    View other_root;
    other_root.set_bounds({0, 0, 340, 300});

    InspectorOverlay overlay(inspected_root);
    overlay.set_active(true);
    overlay.set_tool(InspectorOverlay::Tool::text);
    REQUIRE(overlay.begin_text_edit(label_ptr));
    install_inspector_hooks(overlay);

    TextInputEvent inspected_text;
    inspected_text.text = "A";
    REQUIRE(View::call_inspector_text_hook(inspected_text, &inspected_root));
    REQUIRE(overlay.text_edit_buffer() == "Type hereA");

    TextInputEvent other_text;
    other_text.text = "Z";
    REQUIRE_FALSE(View::call_inspector_text_hook(other_text, &other_root));
    REQUIRE(overlay.text_edit_buffer() == "Type hereA");

    TextInputEvent legacy_text;
    legacy_text.text = "B";
    REQUIRE(View::call_inspector_text_hook(legacy_text, nullptr));
    REQUIRE(overlay.text_edit_buffer() == "Type hereAB");

    MouseEvent mouse;
    mouse.position = {200, 200};
    (void)View::call_inspector_cursor_hook(mouse, &inspected_root);
    REQUIRE(View::call_inspector_cursor_hook(mouse, &other_root) == -1);
}

TEST_CASE("InspectorOverlay hook uninstall clears every global slot",
          "[inspect][overlay][hooks][teardown]") {
    View inspected_root;
    inspected_root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<View>();
    child->set_bounds({20, 20, 120, 30});
    auto* child_ptr = child.get();
    inspected_root.add_child(std::move(child));

    InspectorOverlay overlay(inspected_root);
    overlay.set_active(true);
    overlay.set_selected_view(child_ptr);
    install_inspector_hooks(overlay);
    REQUIRE(g_active_inspector == &overlay);

    RecordingCanvas before_canvas;
    View::paint_overlays(before_canvas, &inspected_root);
    REQUIRE(before_canvas.command_count() > 0);
    uninstall_inspector_hooks();

    REQUIRE(g_active_inspector == nullptr);
    RecordingCanvas after_canvas;
    View::paint_overlays(after_canvas, &inspected_root);
    REQUIRE(after_canvas.command_count() == 0);
    KeyEvent key;
    REQUIRE_FALSE(View::call_inspector_key_hook(key));
    MouseEvent mouse;
    mouse.position = {40, 40};
    REQUIRE_FALSE(View::call_inspector_mouse_hook(mouse, &inspected_root));
    TextInputEvent text;
    text.text = "x";
    REQUIRE_FALSE(View::call_inspector_text_hook(text, &inspected_root));
    REQUIRE(View::call_inspector_cursor_hook(mouse, &inspected_root) == -1);
}

TEST_CASE("InspectorOverlay destruction uninstalls hooks it still owns",
          "[inspect][overlay][hooks][teardown]") {
    View inspected_root;
    inspected_root.set_bounds({0, 0, 400, 300});

    {
        InspectorOverlay overlay(inspected_root);
        install_inspector_hooks(overlay);
        REQUIRE(g_active_inspector == &overlay);
    }

    REQUIRE(g_active_inspector == nullptr);
    KeyEvent key;
    REQUIRE_FALSE(View::call_inspector_key_hook(key));
    MouseEvent mouse;
    REQUIRE_FALSE(View::call_inspector_mouse_hook(mouse, &inspected_root));
    REQUIRE(View::call_inspector_cursor_hook(mouse, &inspected_root) == -1);
}

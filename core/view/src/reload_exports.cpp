// pulp-view-reload-exports — force-retain + export the editor ABI surface that a
// hot-reload logic's create_view() resolves FROM THE HOST at dlopen (live-swap
// M2b). A UI-carrying logic is a thin (RESOLVE_FROM_HOST) dylib: it references
// pulp::view symbols (View/Label/widgets) but the host normally dead-strips them
// (nothing in the host references them — the editor is built in the logic). This
// TU references the public editor surface under [[gnu::used]]; a host that
// force-loads this object (via pulp_reload_host_ui) therefore keeps those symbols
// and, with -export_dynamic, makes them resolvable by the dlopened logic.
//
// SELF-CONTAINED: only the editor widgets + View/Label — deliberately NO window/
// SDL host TUs (whole-archive force-load of pulp-view-core drags in
// sdl_window_host.cpp.o, which references init/shutdown_accessibility that have
// no macOS impl → link failure; that is exactly what this narrow lib avoids).

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>

namespace pulp::view::reload_exports {

// Never called. [[gnu::used]] keeps the compiler from discarding it; force-loading
// this object keeps the linker from discarding it; together the symbols it
// references (ctors/dtors/vtables/methods below) stay in + are exported by the host.
[[gnu::used, gnu::noinline]] void* keep_editor_symbols() {
    auto* root = new View();
    root->set_bounds({0, 0, 1, 1});
    (void)root->flex();
    root->set_background_color(canvas::Color::rgba8(0, 0, 0, 255));  // token-lint:allow symbol-retention filler, not paint (never called)

    auto label = std::make_unique<Label>("x");
    label->set_text("y");
    label->set_font_size(12.0f);
    label->set_text_color(canvas::Color::rgba8(255, 255, 255, 255));  // token-lint:allow symbol-retention filler, not paint (never called)
    label->set_text_align(LabelAlign::center);
    root->add_child(std::move(label));

    // Common widgets a create_view editor may build (all default-constructible).
    root->add_child(std::make_unique<Knob>());
    root->add_child(std::make_unique<Fader>());
    root->add_child(std::make_unique<Toggle>());
    root->add_child(std::make_unique<Checkbox>());
    root->add_child(std::make_unique<ToggleButton>());
    root->add_child(std::make_unique<ComboBox>());
    root->add_child(std::make_unique<SegmentedControl>());
    root->add_child(std::make_unique<XYPad>());
    return root;   // returned so the objects aren't provably dead
}

}  // namespace pulp::view::reload_exports

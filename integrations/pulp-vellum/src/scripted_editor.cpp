#include <pulp/vellum/scripted_editor.hpp>

#include "scripted_editor_root.hpp"

#include <pulp/format/editor_ui_source.hpp>

#include <vellum/view/scripted_ui.hpp>

#include <string>

namespace pulp::vellum {
namespace detail {

::vellum::view::ScriptedUiOptions script_options(const format::Processor& processor,
                                                 format::EditorHostKind host_kind) {
    const bool hot_reload =
        host_kind == format::EditorHostKind::standalone || format::dev_editor_hot_reload_enabled();
    const auto id = processor.descriptor().bundle_id;
    const auto assets = format::editor_assets_for(id.c_str());
    auto options =
        format::select_editor_ui_source_as<::vellum::view::ScriptedUiOptions>(assets, hot_reload);
    if (!options.script_source.empty()) {
        options.storage_identity = (id.empty() ? "embedded/" : id + "/") + options.script_name;
    } else if (const auto theme = format::configured_ui_theme_path(assets)) {
        options.theme_path = *theme;
    }
    options.enable_hot_reload = hot_reload;
    options.enable_theme_reload =
        hot_reload || !options.theme_path.empty() || !options.theme_source.empty();
    options.asset_roots = format::configured_ui_asset_roots(assets);
    return options;
}

} // namespace detail

format::EditorFactory* scripted_editor_factory() {
    return editor_factory<detail::ScriptedEditorRoot<::vellum::view::ScriptedUiSession>>();
}

} // namespace pulp::vellum

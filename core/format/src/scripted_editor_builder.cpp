// The scripted editor builder: the one place outside a processor's own
// create_view() that constructs a ScriptedUiSession for a plug-in editor.
//
// It lives in its own translation unit of pulp-format-view and is installed
// into scripted_editor_builder_slot() by install_scripted_editor_builder(), so
// only a binary that calls that function (a target that declares or embeds a
// UI script, through its generated editor-assets source) links the script
// runtime.

#include <pulp/format/editor_assets.hpp>
#include <pulp/format/editor_ui.hpp>

namespace pulp::format {
namespace {

EditorUiInstance build_scripted_editor_ui(const EditorAssets& assets, state::StateStore& store,
                                          bool enable_hot_reload, std::string* error,
                                          view::ValueChannelAccess value_channel_access,
                                          std::string_view storage_identity) {
    auto source = select_editor_ui_source(assets, enable_hot_reload);
    if (source.available()) {
        auto root = std::make_unique<view::View>();
        root->set_theme(view::Theme::dark());
        root->flex().direction = view::FlexDirection::column;
        // The scripted UI paints through the Skia/Dawn pipeline; tag the
        // root so adapters auto-select the GPU PluginViewHost. (Adapters
        // also check ViewBridge::uses_script_ui(); this keeps the flag
        // correct when the view is inspected outside a bridge.)
        root->set_requires_gpu_host(true);

        view::ScriptedUiOptions options;
        const bool on_disk = !source.embedded();
        options.script_path = source.script_path;
        options.script_source = std::move(source.script_source);
        options.theme_source = std::move(source.theme_source);
        options.script_name = source.script_name;
        options.embedded_assets = std::move(source.embedded_assets);
        // The on-disk theme path belongs to the on-disk document; an embedded
        // one carries its own override bytes.
        const auto theme_path = on_disk ? configured_ui_theme_path(assets) : std::nullopt;
        const bool has_configured_theme = theme_path.has_value();
        if (theme_path) {
            options.theme_path = *theme_path;
        }
        options.asset_roots = configured_ui_asset_roots(assets);
        if (!on_disk) {
            options.storage_identity = storage_identity.empty() ? "embedded/" + options.script_name
                                                                : std::string(storage_identity) +
                                                                      "/" + options.script_name;
        }
        options.enable_hot_reload = enable_hot_reload;
        options.enable_theme_reload =
            enable_hot_reload || has_configured_theme || !options.theme_source.empty();
        options.value_channel_access = std::move(value_channel_access);
        auto scripted_ui = std::make_unique<view::ScriptedUiSession>(*root, store, std::move(options));
        // A deferred load (view::ScopedDeferredDocumentLoad) reports failure
        // here, after open() has handed the host this root, so the AutoUi
        // fallback goes INTO the root instead of replacing it.
        if (view::ScopedDeferredDocumentLoad::active()) {
            scripted_ui->set_document_loaded_callback(
                [root_ptr = root.get(), &store,
                 label = source.label()](bool loaded, const std::string& load_error) {
                    if (loaded)
                        return;
                    runtime::log_error("Scripted editor UI failed to load from '{}': {}. "
                                       "Falling back to AutoUi.",
                                       label, load_error);
                    if (auto fallback = view::AutoUi::build(store))
                        root_ptr->add_child(std::move(fallback));
                });
        }

        std::string load_error;
        if (scripted_ui->load_deferrable(&load_error)) {
            return {std::move(root), std::move(scripted_ui), true};
        }

        runtime::log_error(
            "Scripted editor UI failed to load from '{}': {}. Falling back to AutoUi.",
            source.label(), load_error);
        if (error && error->empty()) {
            *error = load_error;
        }
    }

    return {};
}

} // namespace

void install_scripted_editor_builder() noexcept {
    scripted_editor_builder_slot().store(&build_scripted_editor_ui, std::memory_order_release);
}

} // namespace pulp::format

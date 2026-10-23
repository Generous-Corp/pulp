#pragma once

#include <pulp/format/editor_assets.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/alive_token.hpp>
#include <pulp/runtime/exceptions.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/auto_ui.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::format {

/// Build the UI-thread accessor used by scripted editors. The liveness handle
/// deliberately does not own the Processor; once its owner retires, every
/// future visit fails closed without dereferencing the captured pointer.
/// Visits and owner retirement/destruction must remain serialized on the host
/// UI thread. AliveToken prevents deferred post-destruction calls; it is not a
/// cross-thread reclamation or hazard-pointer mechanism.
inline view::ValueChannelAccess processor_value_channel_access(
    Processor& processor, runtime::AliveToken::Handle owner_alive) {
    return [processor = &processor, owner = std::move(owner_alive)](
               const view::ValueChannelVisitor& visitor) {
        if (!runtime::AliveToken::is_alive(owner)) {
            visitor(nullptr);
            return;
        }
        PULP_TRY { processor->visit_value_channels(visitor); }
        PULP_CATCH_ALL { visitor(nullptr); }
    };
}

struct EditorUiInstance {
    std::unique_ptr<view::View> root;
    std::unique_ptr<view::ScriptedUiSession> scripted_ui;
    bool uses_script_ui = false;
};

/// The scripted-UI entry file `assets` declares, if any.
inline std::optional<std::filesystem::path> configured_ui_script_path(const EditorAssets& assets) {
    if (assets.script_path == nullptr || assets.script_path[0] == '\0')
        return std::nullopt;
    return std::filesystem::path{assets.script_path};
}

/// The theme / design-token file `assets` declares, if any.
inline std::optional<std::filesystem::path> configured_ui_theme_path(const EditorAssets& assets) {
    if (assets.theme_path == nullptr || assets.theme_path[0] == '\0')
        return std::nullopt;
    return std::filesystem::path{assets.theme_path};
}

/// The asset directories `assets` declares, split on '|'.
inline std::vector<std::filesystem::path> configured_ui_asset_roots(const EditorAssets& assets) {
    std::vector<std::filesystem::path> roots;
    if (assets.asset_roots == nullptr)
        return roots;
    std::string_view raw{assets.asset_roots};
    while (!raw.empty()) {
        const auto split = raw.find('|');
        const auto item = raw.substr(0, split);
        if (!item.empty()) roots.emplace_back(std::string(item));
        if (split == std::string_view::npos) break;
        raw.remove_prefix(split + 1);
    }
    return roots;
}

/// This binary's scripted-UI entry file (see register_editor_assets()).
inline std::optional<std::filesystem::path> configured_ui_script_path() {
    return configured_ui_script_path(registered_editor_assets());
}

/// This binary's theme / design-token file.
inline std::optional<std::filesystem::path> configured_ui_theme_path() {
    return configured_ui_theme_path(registered_editor_assets());
}

/// This binary's asset directories.
inline std::vector<std::filesystem::path> configured_ui_asset_roots() {
    return configured_ui_asset_roots(registered_editor_assets());
}

/// Build the editor `assets` declares: a scripted UI when it names a script,
/// AutoUi otherwise (or when the script fails to load). `value_channel_access`
/// leases the hosting processor's current named value channels while scripted
/// UI code resolves and snapshots them.
inline EditorUiInstance build_editor_ui_for_assets(const EditorAssets& assets,
                                                   state::StateStore& store, bool enable_hot_reload,
                                                   std::string* error,
                                                   view::ValueChannelAccess value_channel_access) {
    if (auto script_path = configured_ui_script_path(assets)) {
        auto root = std::make_unique<view::View>();
        root->set_theme(view::Theme::dark());
        root->flex().direction = view::FlexDirection::column;
        // The scripted UI paints through the Skia/Dawn pipeline; tag the
        // root so adapters auto-select the GPU PluginViewHost. (Adapters
        // also check ViewBridge::uses_script_ui(); this keeps the flag
        // correct when the view is inspected outside a bridge.)
        root->set_requires_gpu_host(true);

        view::ScriptedUiOptions options;
        options.script_path = *script_path;
        const auto theme_path = configured_ui_theme_path(assets);
        const bool has_configured_theme = theme_path.has_value();
        if (theme_path) {
            options.theme_path = *theme_path;
        }
        options.asset_roots = configured_ui_asset_roots(assets);
        options.enable_hot_reload = enable_hot_reload;
        options.enable_theme_reload = enable_hot_reload || has_configured_theme;
        options.value_channel_access = std::move(value_channel_access);
        auto scripted_ui = std::make_unique<view::ScriptedUiSession>(*root, store, std::move(options));
        // A deferred load (view::ScopedDeferredDocumentLoad) reports failure
        // here, after open() has handed the host this root, so the AutoUi
        // fallback goes INTO the root instead of replacing it.
        if (view::ScopedDeferredDocumentLoad::active()) {
            scripted_ui->set_document_loaded_callback(
                [root_ptr = root.get(), &store, path = *script_path](
                    bool loaded, const std::string& load_error) {
                    if (loaded) return;
                    runtime::log_error(
                        "Scripted editor UI failed to load from '{}': {}. "
                        "Falling back to AutoUi.", path.string(), load_error);
                    if (auto fallback = view::AutoUi::build(store))
                        root_ptr->add_child(std::move(fallback));
                });
        }

        std::string load_error;
        if (scripted_ui->load_deferrable(&load_error)) {
            return {std::move(root), std::move(scripted_ui), true};
        }

        runtime::log_error("Scripted editor UI failed to load from '{}': {}. Falling back to AutoUi.",
                           script_path->string(), load_error);
        if (error && error->empty()) {
            *error = load_error;
        }
    }

    auto root = view::AutoUi::build(store);
    if (!root && error) {
        *error = "AutoUi::build() returned nullptr";
    }
    return {std::move(root), nullptr, false};
}

/// The editor this binary declares (see register_editor_assets()).
inline EditorUiInstance
build_editor_ui_with_value_channel_access(state::StateStore& store, bool enable_hot_reload,
                                          std::string* error,
                                          view::ValueChannelAccess value_channel_access) {
    return build_editor_ui_for_assets(registered_editor_assets(), store, enable_hot_reload, error,
                                      std::move(value_channel_access));
}

/// Compatibility entry point for stable processors and callers without a
/// processor. Reloadable processors use build_editor_ui_with_value_channel_access().
inline EditorUiInstance build_editor_ui(state::StateStore& store,
                                        bool enable_hot_reload,
                                        std::string* error = nullptr,
                                        view::ValueChannelSet* value_channels = nullptr) {
    view::ValueChannelAccess access;
    if (value_channels != nullptr) {
        access = [value_channels](const view::ValueChannelVisitor& visitor) {
            visitor(value_channels);
        };
    }
    return build_editor_ui_with_value_channel_access(
        store, enable_hot_reload, error, std::move(access));
}

} // namespace pulp::format

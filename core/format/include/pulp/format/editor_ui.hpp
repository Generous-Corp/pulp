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
#include <atomic>
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

/// Builds the scripted editor that `assets` declares (its script path is
/// set). Returns an instance whose root is null when the script failed to
/// load; the failure is already logged and copied into `error`.
using ScriptedEditorBuilder = EditorUiInstance (*)(const EditorAssets& assets,
                                                   state::StateStore& store, bool enable_hot_reload,
                                                   std::string* error,
                                                   view::ValueChannelAccess value_channel_access);

/// The scripted editor builder this binary links, or null.
/// install_scripted_editor_builder() fills it. Editor glue that every plug-in
/// links calls through this slot, so a binary whose editor never runs a script
/// does not link the script runtime (QuickJS, the widget bridge, Skia's text
/// and canvas code it draws through) just to be able to construct a session
/// it never constructs.
inline std::atomic<ScriptedEditorBuilder>& scripted_editor_builder_slot() noexcept {
    static std::atomic<ScriptedEditorBuilder> builder{nullptr};
    return builder;
}

/// Build the editor `assets` declares: a scripted UI when it names a script
/// and the binary links the scripted editor builder, AutoUi otherwise (or when
/// the script fails to load). `value_channel_access` leases the hosting
/// processor's current named value channels while scripted UI code resolves
/// and snapshots them.
inline EditorUiInstance build_editor_ui_for_assets(const EditorAssets& assets,
                                                   state::StateStore& store, bool enable_hot_reload,
                                                   std::string* error,
                                                   view::ValueChannelAccess value_channel_access) {
    if (auto script_path = configured_ui_script_path(assets)) {
        if (auto builder = scripted_editor_builder_slot().load(std::memory_order_acquire)) {
            auto instance =
                builder(assets, store, enable_hot_reload, error, std::move(value_channel_access));
            if (instance.root)
                return instance;
        } else {
            const std::string missing =
                "this binary declares a scripted editor but does not link the script runtime; "
                "declare UI_SCRIPT on the plug-in target or call "
                "pulp::format::install_scripted_editor_builder()";
            runtime::log_error("Scripted editor UI '{}' not built: {}. Falling back to AutoUi.",
                               script_path->string(), missing);
            if (error && error->empty())
                *error = missing;
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

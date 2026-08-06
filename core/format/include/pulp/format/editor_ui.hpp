#pragma once

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

inline std::optional<std::filesystem::path> configured_ui_script_path() {
#ifdef PULP_UI_SCRIPT_PATH
    return std::filesystem::path{PULP_UI_SCRIPT_PATH};
#else
    return std::nullopt;
#endif
}

inline std::optional<std::filesystem::path> configured_ui_theme_path() {
#ifdef PULP_UI_THEME_PATH
    return std::filesystem::path{PULP_UI_THEME_PATH};
#else
    return std::nullopt;
#endif
}

inline std::vector<std::filesystem::path> configured_ui_asset_roots() {
    std::vector<std::filesystem::path> roots;
#ifdef PULP_UI_ASSET_ROOTS
    std::string_view raw{PULP_UI_ASSET_ROOTS};
    while (!raw.empty()) {
        const auto split = raw.find('|');
        const auto item = raw.substr(0, split);
        if (!item.empty()) roots.emplace_back(std::string(item));
        if (split == std::string_view::npos) break;
        raw.remove_prefix(split + 1);
    }
#endif
    return roots;
}

/// `value_channel_access` leases the hosting processor's current named value
/// channels while scripted UI code resolves and snapshots them.
inline EditorUiInstance build_editor_ui_with_value_channel_access(
    state::StateStore& store, bool enable_hot_reload, std::string* error,
    view::ValueChannelAccess value_channel_access) {
    if (auto script_path = configured_ui_script_path()) {
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
        const bool has_configured_theme = configured_ui_theme_path().has_value();
        if (auto theme_path = configured_ui_theme_path()) {
            options.theme_path = *theme_path;
        }
        options.asset_roots = configured_ui_asset_roots();
        options.enable_hot_reload = enable_hot_reload;
        options.enable_theme_reload = enable_hot_reload || has_configured_theme;
        options.value_channel_access = std::move(value_channel_access);
        auto scripted_ui = std::make_unique<view::ScriptedUiSession>(*root, store, std::move(options));

        std::string load_error;
        if (scripted_ui->load(&load_error)) {
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

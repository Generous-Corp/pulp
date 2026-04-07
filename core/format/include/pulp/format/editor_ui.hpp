#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/auto_ui.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace pulp::format {

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

inline EditorUiInstance build_editor_ui(state::StateStore& store,
                                        bool enable_hot_reload,
                                        std::string* error = nullptr) {
    if (auto script_path = configured_ui_script_path()) {
        auto root = std::make_unique<view::View>();
        root->set_theme(view::Theme::dark());
        root->flex().direction = view::FlexDirection::column;

        view::ScriptedUiOptions options;
        options.script_path = *script_path;
        options.enable_hot_reload = enable_hot_reload;
        options.enable_theme_reload = enable_hot_reload;  // Only poll theme.json when JS reload is active
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

} // namespace pulp::format

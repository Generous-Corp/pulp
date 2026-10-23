#pragma once

#include <pulp/format/editor_ui_embed.hpp>
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

/// The on-disk script pulp_add_plugin compiles in (`PULP_UI_SCRIPT_PATH`),
/// read only by a hot-reloading editor; every other editor comes from
/// `embedded_editor_ui()`.
inline std::optional<std::filesystem::path> configured_ui_script_path() {
#ifdef PULP_UI_SCRIPT_PATH
    return std::filesystem::path{PULP_UI_SCRIPT_PATH};
#else
    return std::nullopt;
#endif
}

/// Where the default scripted editor's document comes from.
struct EditorUiSource {
    /// Set when the editor reads this file (and its sibling theme.json) from
    /// disk; empty when it evaluates `script_source`.
    std::filesystem::path script_path;
    std::string script_source;
    std::string theme_source;
    std::string script_name;
    bool available() const noexcept {
        return !script_path.empty() || !script_source.empty();
    }
    /// What log lines call the document.
    std::string label() const {
        return script_path.empty() ? script_name : script_path.string();
    }
};

/// Pick the document for a default scripted editor. The embedded script wins
/// whenever one is linked; the compiled-in on-disk path is only honored for a
/// hot-reloading editor (the standalone host, or a DAW launched with
/// `PULP_DEV_HOT_RELOAD=1`), and only while that file exists, so the
/// edit-and-see loop keeps reading the file the build points at while a
/// shipped image never depends on the developer's source tree. Without an
/// embedded script the path is used as before.
inline EditorUiSource select_editor_ui_source(
    const EmbeddedEditorUi* embedded,
    const std::optional<std::filesystem::path>& dev_script_path,
    bool enable_hot_reload) {
    EditorUiSource source;
    // The dev-build profile gate wraps this decision once it lands: a
    // release-profile build then compiles in no path for this to honor.
    const bool dev_file_present = [&] {
        if (!dev_script_path || dev_script_path->empty()) return false;
        std::error_code ec;
        return std::filesystem::is_regular_file(*dev_script_path, ec) && !ec;
    }();
    if (embedded != nullptr && embedded->script != nullptr && embedded->script_size > 0
        && !(enable_hot_reload && dev_file_present)) {
        source.script_source.assign(reinterpret_cast<const char*>(embedded->script),
                                    embedded->script_size);
        if (embedded->theme != nullptr && embedded->theme_size > 0)
            source.theme_source.assign(reinterpret_cast<const char*>(embedded->theme),
                                       embedded->theme_size);
        source.script_name = embedded->script_name ? embedded->script_name : "ui.js";
        return source;
    }
    if (dev_script_path) source.script_path = *dev_script_path;
    return source;
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
    auto source = select_editor_ui_source(
        embedded_editor_ui(), configured_ui_script_path(), enable_hot_reload);
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
        const bool on_disk = !source.script_path.empty();
        options.script_path = source.script_path;
        options.script_source = std::move(source.script_source);
        options.theme_source = std::move(source.theme_source);
        options.script_name = source.script_name;
        // A compiled-in theme path only belongs to the on-disk document; an
        // embedded one carries its own override bytes.
        const bool has_configured_theme = on_disk && configured_ui_theme_path().has_value();
        if (has_configured_theme) {
            options.theme_path = *configured_ui_theme_path();
        }
        options.asset_roots = configured_ui_asset_roots();
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
                [root_ptr = root.get(), &store, label = source.label()](
                    bool loaded, const std::string& load_error) {
                    if (loaded) return;
                    runtime::log_error(
                        "Scripted editor UI failed to load from '{}': {}. "
                        "Falling back to AutoUi.", label, load_error);
                    if (auto fallback = view::AutoUi::build(store))
                        root_ptr->add_child(std::move(fallback));
                });
        }

        std::string load_error;
        if (scripted_ui->load_deferrable(&load_error)) {
            return {std::move(root), std::move(scripted_ui), true};
        }

        runtime::log_error("Scripted editor UI failed to load from '{}': {}. Falling back to AutoUi.",
                           source.label(), load_error);
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

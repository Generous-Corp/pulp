#pragma once

#include <pulp/format/editor_ui_source.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/alive_token.hpp>
#include <pulp/runtime/exceptions.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/auto_ui.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <atomic>
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

/// The files embedded beside `assets`' script, as the bridge consumes them.
inline std::vector<view::WidgetBridge::EmbeddedAsset>
embedded_ui_assets(const EditorAssets& assets) {
    return copy_embedded_editor_files<view::WidgetBridge::EmbeddedAsset>(assets);
}

/// Where the default scripted editor's document comes from.
struct EditorUiSource {
    /// Set when the editor reads this file (and its sibling theme.json) from
    /// disk; empty when it evaluates `script_source`.
    std::filesystem::path script_path;
    std::string script_source;
    std::string theme_source;
    std::string script_name;
    /// Files embedded beside an embedded script; empty for an on-disk document.
    std::vector<view::WidgetBridge::EmbeddedAsset> embedded_assets;
    bool available() const noexcept {
        return !script_path.empty() || !script_source.empty();
    }
    bool embedded() const noexcept {
        return script_path.empty() && !script_source.empty();
    }
    /// What log lines call the document.
    std::string label() const {
        return script_path.empty() ? script_name : script_path.string();
    }
};

inline EditorUiSource select_editor_ui_source(const EditorAssets& assets, bool enable_hot_reload) {
    return select_editor_ui_source_as<EditorUiSource>(assets, enable_hot_reload);
}

/// Builds the scripted editor from `source`, the document
/// select_editor_ui_source() chose for `assets` (the caller has already
/// checked it is available). Returns an instance whose root is null when the
/// script failed to load; the failure is already logged and copied into
/// `error`.
using ScriptedEditorBuilder = EditorUiInstance (*)(const EditorAssets& assets,
                                                   EditorUiSource source, state::StateStore& store,
                                                   bool enable_hot_reload, std::string* error,
                                                   view::ValueChannelAccess value_channel_access,
                                                   std::string_view storage_identity);

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

/// Build the editor `assets` declares: a scripted UI when it names or embeds a
/// script and the binary links the scripted editor builder, AutoUi otherwise
/// (or when the script fails to load). `value_channel_access` leases the
/// hosting processor's current named value channels while scripted UI code
/// resolves and snapshots them. `storage_identity` names the plug-in (its
/// bundle id) so an embedded editor, which has no script directory to be keyed
/// by, keeps stable persisted stores.
inline EditorUiInstance build_editor_ui_for_assets(const EditorAssets& assets,
                                                   state::StateStore& store, bool enable_hot_reload,
                                                   std::string* error,
                                                   view::ValueChannelAccess value_channel_access,
                                                   std::string_view storage_identity = {}) {
    // Selected once here: choosing it stats the on-disk script, and the
    // builder evaluates exactly the document this check found.
    if (auto source = select_editor_ui_source(assets, enable_hot_reload); source.available()) {
        if (auto builder = scripted_editor_builder_slot().load(std::memory_order_acquire)) {
            auto instance = builder(assets, std::move(source), store, enable_hot_reload, error,
                                    std::move(value_channel_access), storage_identity);
            if (instance.root)
                return instance;
        } else {
            const std::string missing =
                "this binary declares a scripted editor but does not link the script runtime; "
                "declare UI_SCRIPT on the plug-in target or call "
                "pulp::format::install_scripted_editor_builder()";
            runtime::log_error("Scripted editor UI not built: {}. Falling back to AutoUi.",
                               missing);
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

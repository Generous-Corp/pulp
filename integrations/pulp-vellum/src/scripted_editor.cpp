#include <pulp/vellum/scripted_editor.hpp>

#include <pulp/format/editor_prewarm.hpp>
#include <pulp/format/editor_ui_source.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/vellum/editor.hpp>

#include <vellum/view/scripted_ui.hpp>
#include <vellum/view/scripted_ui_prewarm.hpp>

#include <cstdint>
#include <string>

namespace pulp::vellum {
namespace {

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

class ScriptedEditorRoot final : public ::vellum::view::View {
  public:
    ScriptedEditorRoot(EditorEnv& env, format::Processor& processor)
        : processor_(processor), preferred_(processor.view_size()),
          supports_reload_(processor.supports_editor_reload()),
          session_(*this, env.params, script_options(processor, env.context.host_kind())) {
        set_theme(::vellum::view::Theme::dark());
        flex().direction = ::vellum::view::FlexDirection::column;
        session_.set_host_kind(env.context.host_kind() == format::EditorHostKind::standalone
                                   ? "standalone"
                                   : "plugin");
        std::string error;
        if (!session_.load(&error))
            runtime::log_error("Vellum scripted editor failed to load '{}': {}",
                               session_.script_label(), error);
        if (supports_reload_)
            reload_generation_ = processor_.editor_reload_generation();
    }

    format::EditorSize preferred_editor_size() const {
        return {preferred_.preferred_width, preferred_.preferred_height, 1.0};
    }

    void editor_opened() {
        if (auto* host = plugin_view_host()) {
            gpu_subscription_ = host->observe_gpu_surface(
                [this](const auto& status) { session_.attach_gpu_surface(status.surface); });
        }
    }

    void editor_closing() {
        gpu_subscription_ = {};
        session_.attach_gpu_surface(nullptr);
    }

    void editor_idle() {
        // ContractEditorSession reconciles store listeners before calling Editor::idle().
        if (supports_reload_) {
            const auto generation = processor_.editor_reload_generation();
            if (generation != reload_generation_ && session_.reload())
                reload_generation_ = generation;
        }
        session_.poll();
    }

  private:
    format::Processor& processor_;
    format::ViewSize preferred_;
    bool supports_reload_;
    std::uint64_t reload_generation_ = 0;
    ::vellum::view::ScriptedUiSession session_;
    ::vellum::view::PluginViewHost::GpuSurfaceSubscription gpu_subscription_;
};

void schedule_scripted_ui_prewarm(const format::Processor::EditorPrewarm& request) {
    ::vellum::view::prewarm_scripted_ui({
        {request.scripts.begin(), request.scripts.end()},
        {request.materialized_documents.begin(), request.materialized_documents.end()},
    });
}

// This object is linked into the image by the scripted editor package's factory.
[[maybe_unused]] const bool prewarm_installed = [] {
    format::set_editor_prewarm_scheduler(&schedule_scripted_ui_prewarm);
    return true;
}();

} // namespace

format::EditorFactory* scripted_editor_factory() {
    return editor_factory<ScriptedEditorRoot>();
}

} // namespace pulp::vellum

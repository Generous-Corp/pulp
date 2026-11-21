#pragma once

// Private root shared by the production session and the recording boundary
// test. Lifecycle forwarding stays in one implementation.
#include <pulp/runtime/log.hpp>
#include <pulp/vellum/editor.hpp>
#include <vellum/view/scripted_ui.hpp>

#if defined(PULP_VELLUM_AUDIO)
#include <pulp/vellum_audio/audio_widget_registration.hpp>
#endif

#include <cstdint>
#include <string>

namespace pulp::vellum::detail {

::vellum::view::ScriptedUiOptions script_options(const format::Processor& processor,
                                                 format::EditorHostKind host_kind);

template <typename Session> class ScriptedEditorRoot final : public ::vellum::view::View {
  public:
    ScriptedEditorRoot(EditorEnv& env, format::Processor& processor)
        : processor_(processor), preferred_(processor.view_size()),
          supports_reload_(processor.supports_editor_reload()),
#if defined(PULP_VELLUM_AUDIO)
          visuals_(env.visuals),
#endif
          session_(*this, env.params, script_options(processor, env.context.host_kind())) {
        set_theme(::vellum::view::Theme::dark());
        flex().direction = ::vellum::view::FlexDirection::column;
#if defined(PULP_VELLUM_AUDIO)
        ::pulp::vellum_audio::register_audio_widgets();
#endif
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

    format::ViewSize editor_view_size() const {
        return preferred_;
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
#if defined(PULP_VELLUM_AUDIO)
        ::pulp::vellum_audio::sample_audio_widgets(*this, visuals_);
#endif
    }

  private:
    format::Processor& processor_;
    format::ViewSize preferred_;
    bool supports_reload_;
    std::uint64_t reload_generation_ = 0;
#if defined(PULP_VELLUM_AUDIO)
    ::vellum::value::ValueFeed* visuals_;
#endif
    Session session_;
    ::vellum::view::PluginViewHost::GpuSurfaceSubscription gpu_subscription_;
};

} // namespace pulp::vellum::detail

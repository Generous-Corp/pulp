#pragma once

/// @file
/// Editor-host glue for a Vellum `view::View` tree.
///
/// A plug-in editor type derives from `vellum::view::View` and accepts an
/// `EditorEnv&` or `(EditorEnv&, format::Processor&)` in its constructor.
/// It can return a `format::ViewSize` from `editor_view_size()` to declare
/// resize constraints and an authored viewport.
/// Each window has its own `ParamPortModel` for subscriptions and gesture
/// lifetime while all edits still use Pulp's existing parameter bridge.

#include <pulp/format/editor_host.hpp>
#include <pulp/format/editor_package.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/vellum/editor_env.hpp>
#include <pulp/vellum/param_port_model.hpp>
#include <pulp/vellum/visual_port_feed.hpp>

#include <vellum/view/plugin_view_host.hpp>
#include <vellum/graphics/dawn_native_bootstrap.hpp>
#include <vellum/view/view.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::vellum {

/// Install Dawn's proc table before Vellum creates the first GPU editor host.
/// The process owns that table; the installed Vellum SDK registers it through
/// its app-host bootstrap, which agrees across plug-ins built on the same Dawn
/// revision. Pulp's own gpu-runtime copy must not be used here: linking it into
/// a plug-in would carry a second Dawn beside the one in Vellum's provider.
PULP_FORMAT_IMAGE_LOCAL inline bool register_editor_dawn_bootstrap(std::string_view bundle_id,
                                                                   std::string* error = nullptr) {
    static std::once_flag once;
    static std::string failure;
    bool log_failure = false;
    std::call_once(once, [&] {
        if (::vellum::app_host::register_native_dawn_bootstrap(&failure)) {
            failure.clear();
        } else {
            if (failure.empty())
                failure = "Vellum did not report a Dawn bootstrap error";
            log_failure = true;
        }
    });
    if (log_failure)
        runtime::log_error("Vellum editor '{}': Dawn bootstrap failed: {}", bundle_id, failure);
    if (error != nullptr)
        *error = failure;
    return failure.empty();
}

template <typename ViewType>
class Editor final : public format::Editor, public format::EditorAttachDiagnostics {
    static_assert(std::is_base_of_v<::vellum::view::View, ViewType>,
                  "a Pulp Vellum editor must derive from vellum::view::View");

  public:
    Editor(format::EditorContext& context, std::string bundle_id)
        : Editor(context, bundle_id,
                 [](EditorEnv& env) { return std::make_unique<ViewType>(env); }) {}

    Editor(format::EditorContext& context, format::Processor& processor)
        : Editor(
              context, processor.descriptor().bundle_id,
              [&processor](EditorEnv& env) {
                  if constexpr (std::is_constructible_v<ViewType, EditorEnv&, format::Processor&>)
                      return std::make_unique<ViewType>(env, processor);
                  else
                      return std::make_unique<ViewType>(env);
              },
              processor.editor_background()) {}

  private:
    template <typename MakeRoot>
    Editor(format::EditorContext& context, std::string_view bundle_id, MakeRoot make_root,
           std::optional<std::uint32_t> background = std::nullopt)
        : model_(context.params()), env_{context, model_, nullptr} {
        if (auto* visuals = context.visuals()) {
            visual_feed_ = std::make_unique<VisualPortFeed>(*visuals);
            env_.visuals = visual_feed_.get();
            visual_publish_counts_.resize(visual_feed_->channel_count());
            for (std::size_t i = 0; i < visual_publish_counts_.size(); ++i)
                visual_publish_counts_[i] = visual_feed_->publish_count(i);
        }
        root_ = make_root(env_);
        if constexpr (requires(ViewType& view) {
                          { view.editor_view_size() } -> std::convertible_to<format::ViewSize>;
                      }) {
            view_size_ = root_->editor_view_size();
            env_.lifetime_->size_hints = view_size_;
            if (view_size_->preferred_width != 0 && view_size_->preferred_height != 0)
                size_ = {view_size_->preferred_width, view_size_->preferred_height, size_.scale};
        } else if constexpr (requires(ViewType& view) {
                                 {
                                     view.preferred_editor_size()
                                 } -> std::convertible_to<format::EditorSize>;
                             }) {
            const format::EditorSize preferred = root_->preferred_editor_size();
            if (preferred.width != 0 && preferred.height != 0)
                size_ = preferred;
        }
        layout_root();
        if (!register_editor_dawn_bootstrap(bundle_id, &bootstrap_error_))
            return;
        root_->set_requires_gpu_host(true);
        ::vellum::view::PluginViewHost::Options options;
        options.size = {size_.width, size_.height};
        options.use_gpu = true;
        // The plug-in's declared editor colour is what the host shows before
        // the first frame and wherever the root does not paint.
        if (background)
            options.background_rgb = *background & 0xFFFFFFu;
        host_ = ::vellum::view::PluginViewHost::create(*root_, options);
        if (host_ != nullptr && has_design_viewport()) {
            host_->set_design_viewport(static_cast<float>(view_size_->design_width),
                                       static_cast<float>(view_size_->design_height));
            host_->set_fixed_aspect_ratio(static_cast<float>(view_size_->aspect_ratio));
        }
    }

  public:
    // The root is destroyed after the host (member order), and its teardown
    // can still repaint: a ScriptedUiSession quarantines its realm and
    // requests a frame. A host is not required to clear the root's pointer to
    // it, so clear it here before the host goes away.
    ~Editor() override {
        detach();
        invalidate();
    }

    // X11 is not offered: an X11 parent carries its window in window_id with a
    // null handle, and PluginViewHost attaches only to a pointer parent, so a
    // claimed X11 parent could never attach. Add it once the host takes an id.
    bool supports_parent(format::NativeParentKind kind) const override {
        return kind == format::NativeParentKind::none ||
               kind == format::NativeParentKind::cocoa_view ||
               kind == format::NativeParentKind::uikit_view ||
               kind == format::NativeParentKind::win32_window;
    }

    bool attach(const format::NativeParent& parent) override {
        if (invalidated_ || !bootstrap_error_.empty())
            return false;
        if (parent.kind == format::NativeParentKind::none)
            return true;
        if (host_ == nullptr || parent.handle == nullptr || !supports_parent(parent.kind) ||
            attached_ || closing_)
            return false;
        if (!host_->try_attach_to_parent(parent.handle))
            return false;
        if (!env_.lifetime_) {
            env_.lifetime_ = std::make_shared<EditorEnv::Attachment>();
            env_.lifetime_->size_hints = view_size_;
        }
        attached_ = true;
        if constexpr (requires(ViewType& view) { view.editor_opened(); })
            root_->editor_opened();
        return attached_;
    }

    void detach() override {
        if (!attached_)
            return;
        close_attachment();
        if (host_ != nullptr)
            host_->detach();
    }

    void invalidate() override {
        if (std::exchange(invalidated_, true))
            return;
        close_attachment();
        if (host_ != nullptr && root_->plugin_view_host() == host_.get())
            root_->set_plugin_view_host(nullptr);
        host_.reset();
    }

    format::EditorSize size() const override {
        return size_;
    }

    bool set_size(const format::EditorSize& size) override {
        if (closing_ || invalidated_ || size.width == 0 || size.height == 0)
            return false;
        size_ = size;
        if (view_size_)
            format::detail::clamp_to_hints(*view_size_, size_.width, size_.height);
        layout_root();
        if (host_ != nullptr)
            host_->set_size(size_.width, size_.height);
        return true;
    }

    format::ViewSize size_hints() const override {
        if (view_size_)
            return *view_size_;
        format::ViewSize hints;
        hints.preferred_width = size_.width;
        hints.preferred_height = size_.height;
        hints.min_width = 1;
        hints.min_height = 1;
        return hints;
    }

    void idle() override {
        if (!env_.lifetime_)
            return;
        // ContractEditorSession pumps ParamPort listeners before this call.
        if constexpr (requires(ViewType& view) { view.editor_idle(); }) {
            if (root_ != nullptr)
                root_->editor_idle();
        }
        if (host_ == nullptr)
            return;
        // View damage already schedules a repaint through PluginViewHost.
        bool published = false;
        for (std::size_t i = 0; i < visual_publish_counts_.size(); ++i) {
            const auto count = visual_feed_->publish_count(i);
            if (std::exchange(visual_publish_counts_[i], count) != count)
                published = true;
        }
        if (published)
            host_->request_frame();
    }

    format::EditorAutomation* automation() override {
        return &automation_;
    }

  private:
    std::string_view attach_failure_reason() const noexcept override {
        return bootstrap_error_;
    }

    class Automation final : public format::EditorAutomation {
      public:
        explicit Automation(Editor& editor) : editor_(editor) {}

        bool capture_png(std::vector<std::uint8_t>& png) override {
            if (editor_.host_ == nullptr || !editor_.host_->present_first_frame())
                return false;
            png = editor_.host_->capture_back_buffer_png();
            return !png.empty();
        }

      private:
        Editor& editor_;
    };

    void close_attachment() {
        if (env_.lifetime_)
            env_.lifetime_->open = false;
        env_.lifetime_.reset();
        if (!std::exchange(attached_, false))
            return;
        closing_ = true;
        if constexpr (requires(ViewType& view) { view.editor_closing(); })
            root_->editor_closing();
        closing_ = false;
    }

    bool has_design_viewport() const {
        return view_size_ && view_size_->design_width != 0 && view_size_->design_height != 0;
    }

    void layout_root() {
        if (root_ == nullptr)
            return;
        const bool design = has_design_viewport();
        const auto width = design ? view_size_->design_width : size_.width;
        const auto height = design ? view_size_->design_height : size_.height;
        root_->set_bounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
        root_->layout_children();
    }

    ParamPortModel model_;
    std::unique_ptr<VisualPortFeed> visual_feed_;
    std::vector<std::uint32_t> visual_publish_counts_;
    EditorEnv env_;
    std::unique_ptr<ViewType> root_;
    std::unique_ptr<::vellum::view::PluginViewHost> host_;
    Automation automation_{*this};
    std::optional<format::ViewSize> view_size_;
    format::EditorSize size_{400, 300, 1.0};
    std::string bootstrap_error_;
    bool attached_ = false;
    bool closing_ = false;
    bool invalidated_ = false;
};

template <typename ViewType> class EditorFactory final : public format::EditorFactory {
  public:
    bool supports(format::EditorRole role) const override {
        return role == format::EditorRole::editor || role == format::EditorRole::inspector;
    }

    std::unique_ptr<format::Editor> create(format::Processor& processor,
                                           format::EditorContext& context,
                                           format::EditorRole role) override {
        return supports(role) ? std::make_unique<Editor<ViewType>>(context, processor) : nullptr;
    }
};

/// Return the image-local factory used by `pulp_add_plugin(... UI <package>)`.
template <typename ViewType> format::EditorFactory* editor_factory() {
    static EditorFactory<ViewType> factory;
    return &factory;
}

} // namespace pulp::vellum

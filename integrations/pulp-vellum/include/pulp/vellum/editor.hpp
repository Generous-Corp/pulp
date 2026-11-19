#pragma once

/// @file
/// Editor-host glue for a Vellum `view::View` tree.
///
/// A plug-in editor type derives from `vellum::view::View` and accepts an
/// `EditorEnv&` in its constructor. The environment owns the per-editor
/// `ParamPortModel`, so each window has independent subscriptions and gesture
/// lifetime while all edits still use Pulp's existing parameter bridge.

#include <pulp/format/editor_host.hpp>
#include <pulp/vellum/param_port_model.hpp>
#include <pulp/vellum/visual_port_feed.hpp>

#include <vellum/view/plugin_view_host.hpp>
#include <vellum/view/view.hpp>

#include <memory>
#include <type_traits>

namespace pulp::vellum {

struct EditorEnv {
    format::EditorContext& context;
    ParamPortModel& params;
    ::vellum::value::ValueFeed* visuals = nullptr;
};

template <typename ViewType> class Editor final : public format::Editor {
    static_assert(std::is_base_of_v<::vellum::view::View, ViewType>,
                  "a Pulp Vellum editor must derive from vellum::view::View");

  public:
    explicit Editor(format::EditorContext& context)
        : model_(context.params()), env_{context, model_, nullptr} {
        if (auto* visuals = context.visuals()) {
            visual_feed_ = std::make_unique<VisualPortFeed>(*visuals);
            env_.visuals = visual_feed_.get();
        }
        root_ = std::make_unique<ViewType>(env_);
        root_->set_requires_gpu_host(true);
        ::vellum::view::PluginViewHost::Options options;
        options.size = {size_.width, size_.height};
        options.use_gpu = true;
        host_ = ::vellum::view::PluginViewHost::create(*root_, options);
    }

    ~Editor() override {
        detach();
    }

    bool supports_parent(format::NativeParentKind kind) const override {
        return kind == format::NativeParentKind::none ||
               kind == format::NativeParentKind::cocoa_view ||
               kind == format::NativeParentKind::uikit_view ||
               kind == format::NativeParentKind::win32_window ||
               kind == format::NativeParentKind::x11_window;
    }

    bool attach(const format::NativeParent& parent) override {
        if (parent.kind == format::NativeParentKind::none)
            return true;
        if (host_ == nullptr || parent.handle == nullptr || !supports_parent(parent.kind))
            return false;
        return host_->try_attach_to_parent(parent.handle);
    }

    void detach() override {
        if (host_ != nullptr)
            host_->detach();
    }

    void invalidate() override {
        if (root_ != nullptr)
            root_->set_plugin_view_host(nullptr);
        host_.reset();
    }

    format::EditorSize size() const override {
        return size_;
    }

    bool set_size(const format::EditorSize& size) override {
        if (size.width == 0 || size.height == 0)
            return false;
        size_ = size;
        if (host_ != nullptr)
            host_->set_size(size.width, size.height);
        return true;
    }

    format::ViewSize size_hints() const override {
        format::ViewSize hints;
        hints.preferred_width = size_.width;
        hints.preferred_height = size_.height;
        hints.min_width = 1;
        hints.min_height = 1;
        return hints;
    }

    void idle() override {
        // ContractEditorSession pumps ParamPort listeners before this call.
        // Repaint once per host tick so meter/scope snapshots are visible even
        // when the Vellum tree has no parameter-driven damage of its own.
        if (host_ != nullptr)
            host_->repaint();
    }

  private:
    ParamPortModel model_;
    std::unique_ptr<VisualPortFeed> visual_feed_;
    EditorEnv env_;
    std::unique_ptr<ViewType> root_;
    std::unique_ptr<::vellum::view::PluginViewHost> host_;
    format::EditorSize size_{400, 300, 1.0};
};

template <typename ViewType> class EditorFactory final : public format::EditorFactory {
  public:
    bool supports(format::EditorRole role) const override {
        return role == format::EditorRole::editor || role == format::EditorRole::inspector;
    }

    std::unique_ptr<format::Editor> create(format::Processor&, format::EditorContext& context,
                                           format::EditorRole role) override {
        return supports(role) ? std::make_unique<Editor<ViewType>>(context) : nullptr;
    }
};

/// Return the image-local factory used by `pulp_add_plugin(... UI <package>)`.
template <typename ViewType> format::EditorFactory* editor_factory() {
    static EditorFactory<ViewType> factory;
    return &factory;
}

} // namespace pulp::vellum

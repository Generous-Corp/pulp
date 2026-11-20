#pragma once

/// @file
/// Editor-host glue for a Vellum `view::View` tree.
///
/// A plug-in editor type derives from `vellum::view::View` and accepts an
/// `EditorEnv&` in its constructor. The environment owns the per-editor
/// `ParamPortModel`, so each window has independent subscriptions and gesture
/// lifetime while all edits still use Pulp's existing parameter bridge.

#include <pulp/format/editor_host.hpp>
#include <pulp/vellum/editor_env.hpp>
#include <pulp/vellum/param_port_model.hpp>
#include <pulp/vellum/visual_port_feed.hpp>

#include <vellum/view/plugin_view_host.hpp>
#include <vellum/graphics/dawn_native_bootstrap.hpp>
#include <vellum/view/view.hpp>

#include <memory>
#include <type_traits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace pulp::vellum {

/// Install Dawn's proc table before Vellum creates the first GPU editor host.
/// The process owns that table; the installed Vellum SDK registers it through
/// its app-host bootstrap, which agrees across plug-ins built on the same Dawn
/// revision. Pulp's own gpu-runtime copy must not be used here: linking it into
/// a plug-in would carry a second Dawn beside the one in Vellum's provider.
inline bool register_editor_dawn_bootstrap(std::string_view bundle_id,
                                           std::string* error = nullptr) {
    static std::mutex mutex;
    static std::unordered_set<std::string> registered;
    std::lock_guard lock(mutex);
    if (registered.contains(std::string(bundle_id)))
        return true;
    if (!::vellum::app_host::register_native_dawn_bootstrap(error))
        return false;
    registered.emplace(bundle_id);
    return true;
}

template <typename ViewType> class Editor final : public format::Editor {
    static_assert(std::is_base_of_v<::vellum::view::View, ViewType>,
                  "a Pulp Vellum editor must derive from vellum::view::View");

  public:
    Editor(format::EditorContext& context, std::string bundle_id)
        : model_(context.params()), env_{context, model_, nullptr} {
        if (auto* visuals = context.visuals()) {
            visual_feed_ = std::make_unique<VisualPortFeed>(*visuals);
            env_.visuals = visual_feed_.get();
        }
        root_ = std::make_unique<ViewType>(env_);
        std::string bootstrap_error;
        if (!register_editor_dawn_bootstrap(bundle_id, &bootstrap_error))
            return;
        root_->set_requires_gpu_host(true);
        ::vellum::view::PluginViewHost::Options options;
        options.size = {size_.width, size_.height};
        options.use_gpu = true;
        host_ = ::vellum::view::PluginViewHost::create(*root_, options);
    }

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

    std::unique_ptr<format::Editor> create(format::Processor& processor,
                                           format::EditorContext& context,
                                           format::EditorRole role) override {
        return supports(role)
                   ? std::make_unique<Editor<ViewType>>(context, processor.descriptor().bundle_id)
                   : nullptr;
    }
};

/// Return the image-local factory used by `pulp_add_plugin(... UI <package>)`.
template <typename ViewType> format::EditorFactory* editor_factory() {
    static EditorFactory<ViewType> factory;
    return &factory;
}

} // namespace pulp::vellum

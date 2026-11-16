#include "rmlui_metal_demo_ui.hpp"

#include "rmlui_metal_demo_processor.hpp"

#include <pulp/format/editor_host.hpp>
#include <pulp/rmlui_native/editor_keys.hpp>
#include <pulp/rmlui_native/metal_renderer.hpp>
#include <pulp/rmlui_native/rmlui_surface.hpp>

#include <vellum/rmlui/assets.hpp>
#include <vellum/rmlui/metal_render_interface.hpp>

#import <Metal/Metal.h>

#include <memory>
#include <string>
#include <string_view>

namespace pulp_rmlui_metal_demo {
namespace {

class Assets final : public vellum::rmlui::AssetProvider {
  public:
    vellum::rmlui::Status load(std::string_view path, vellum::rmlui::Asset& asset) override {
        if (path != "demo.rml")
            return vellum::rmlui::Status::missing_asset;
        asset.path = std::string(path);
        asset.bytes = R"(<rml><head><style>
body { width: 100%; height: 100%; margin: 0; background: #111827; }
#title { position: absolute; left: 20px; top: 20px; font-family: Inter;
         font-size: 24dp; color: #ffffff; }
#card { position: absolute; left: 24px; top: 24px; width: 272px; height: 168px;
        padding: 12px; background: linear-gradient(135deg, #2563eb, #7c3aed);
        border: 2px #93c5fd; border-radius: 16px; }
#bar { position: absolute; left: 20px; top: 96px; width: 232px; height: 18px;
       background: #111827; border-radius: 9px; }
#fill { width: 68%; height: 100%; background: #fbbf24; border-radius: 9px; }
</style></head><body><div id="title">Pulp + RmlUi + Metal</div><div id="card"><div id="bar"><div id="fill"/></div></div></body></rml>)";
        return vellum::rmlui::Status::ok;
    }
};

class Editor final : public ::pulp::format::Editor {
  public:
    explicit Editor(::pulp::format::EditorContext& context) : context_(context) {}
    ~Editor() override {
        teardown(false);
    }

    bool supports_parent(::pulp::format::NativeParentKind kind) const override {
        return kind == ::pulp::format::NativeParentKind::cocoa_view;
    }

    bool attach(const ::pulp::format::NativeParent& parent) override {
        if (parent.kind != ::pulp::format::NativeParentKind::cocoa_view || !parent.handle || ui_)
            return false;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
            return false;
        renderer_ = std::make_unique<vellum::rmlui::MetalRenderInterface>(device);
        if (!renderer_->valid()) {
            renderer_.reset();
            return false;
        }
        ::pulp::rmlui_native::RmlUiSurfaceOptions options;
        options.document = "demo.rml";
        options.device = (__bridge void*)device;
        ui_ = std::make_unique<::pulp::rmlui_native::RmlUiSurface>(
            assets_, *renderer_, ::pulp::rmlui_native::metal_renderer_hooks(*renderer_), options);
        if (!ui_->attach(parent.handle)) {
            ui_.reset();
            renderer_.reset();
            return false;
        }
        return true;
    }

    void detach() override {
        teardown(true);
    }
    void invalidate() override {
        if (ui_)
            ui_->invalidate();
        teardown(false);
    }
    ::pulp::format::EditorSize size() const override {
        return size_;
    }
    bool set_size(const ::pulp::format::EditorSize& size) override {
        if (size.width == 0 || size.height == 0)
            return false;
        size_ = size;
        return true;
    }
    ::pulp::format::ViewSize size_hints() const override {
        return {.preferred_width = 320,
                .preferred_height = 240,
                .min_width = 240,
                .min_height = 180,
                .max_width = 1280,
                .max_height = 960};
    }
    void idle() override {
        if (ui_)
            ui_->idle();
    }
    void focus_changed(bool focused) override {
        if (ui_)
            ::pulp::rmlui_native::forward_editor_focus(*ui_, focused);
    }
    ::pulp::format::KeyDisposition offer_key(const ::pulp::format::EditorKeyEvent& event) override {
        return ui_ ? ::pulp::rmlui_native::offer_editor_key(*ui_, event)
                   : ::pulp::format::KeyDisposition::forward;
    }

  private:
    void teardown(bool orderly) {
        if (ui_ && orderly)
            ui_->detach();
        ui_.reset();
        renderer_.reset();
    }

    ::pulp::format::EditorContext& context_;
    ::pulp::format::EditorSize size_{320, 240, 1.0};
    Assets assets_;
    std::unique_ptr<vellum::rmlui::MetalRenderInterface> renderer_;
    std::unique_ptr<::pulp::rmlui_native::RmlUiSurface> ui_;
};

class Factory final : public ::pulp::format::EditorFactory {
  public:
    bool supports(::pulp::format::EditorRole role) const override {
        return role == ::pulp::format::EditorRole::editor;
    }
    std::unique_ptr<::pulp::format::Editor> create(::pulp::format::Processor&,
                                                   ::pulp::format::EditorContext& context,
                                                   ::pulp::format::EditorRole role) override {
        if (role != ::pulp::format::EditorRole::editor)
            return nullptr;
        return std::make_unique<Editor>(context);
    }
};

} // namespace

::pulp::format::EditorFactory* editor_factory() {
    static Factory factory;
    return &factory;
}

::pulp::format::EditorFactory* Processor::editor_factory() {
    return ::pulp_rmlui_metal_demo::editor_factory();
}

} // namespace pulp_rmlui_metal_demo

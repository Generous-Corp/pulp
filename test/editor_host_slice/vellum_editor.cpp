// The slice's Vellum editor: the Pulp side of a scratch `pulp-vellum`.
//
// It implements the editor-host contract over the std-only KnobPanel boundary
// and defines this binary's `slice_editor_factory()`. No Vellum header is
// visible here.

#include "gain_meter_processor.hpp"
#include "layer_view_mac.hpp"
#include "vellum_ui/knob_panel.hpp"

#include <array>
#include <cstdlib>
#include <string>

namespace pulp::slice {

namespace {

constexpr std::uint32_t kWidth = 240;
constexpr std::uint32_t kHeight = 160;

class VellumEditor final : public format::Editor, public format::EditorAutomation {
  public:
    explicit VellumEditor(format::EditorContext& context) : context_(context) {
        auto& params = context_.params();
        panel_ = slice_vellum_ui::KnobPanel::create(
            kWidth, kHeight, params.get_normalized(kGainParam),
            {[&params] { params.begin(kGainParam); },
             [&params](float v) { params.set_normalized(kGainParam, v); },
             [&params] { params.end(kGainParam); }});
        subscription_ = params.add_listener([this](state::ParamID id, float) {
            if (id == kGainParam)
                panel_->value_changed(context_.params().get_normalized(kGainParam));
        });
    }

    bool supports_parent(format::NativeParentKind kind) const override {
        return kind == format::NativeParentKind::cocoa_view ||
               kind == format::NativeParentKind::none;
    }
    ~VellumEditor() override {
        detach();
    }
    bool attach(const format::NativeParent& parent) override {
        if (parent.kind == format::NativeParentKind::none)
            return true;
        if (parent.kind != format::NativeParentKind::cocoa_view || !parent.handle)
            return false;
        layer_view_ = layer_view_create(parent.handle, panel_->width(), panel_->height());
        present();
        return layer_view_ != nullptr;
    }
    void detach() override {
        if (layer_view_) {
            layer_view_destroy(layer_view_);
            layer_view_ = nullptr;
        }
    }
    void invalidate() override {
        if (layer_view_) {
            layer_view_abandon(layer_view_);
            layer_view_ = nullptr;
        }
    }
    format::EditorSize size() const override {
        return {panel_->width(), panel_->height(), 1.0};
    }
    bool set_size(const format::EditorSize& size) override {
        if (size.width < 120 || size.height < 80)
            return false;
        panel_->set_size(size.width, size.height, size.scale);
        if (layer_view_)
            layer_view_resize(layer_view_, size.width, size.height);
        present();
        return true;
    }
    void idle() override {
        if (auto* visuals = context_.visuals()) {
            std::array<float, 1> rms{}, peak{};
            if (visuals->read_meter(0, rms, peak) > 0)
                panel_->set_meter(peak[0]);
        }
        panel_->idle();
        present();
    }
    format::EditorAutomation* automation() override {
        return this;
    }

    // ── automation ──
    bool capture_png(std::vector<std::uint8_t>& png) override {
        png = panel_->capture_png();
        return !png.empty();
    }
    // Request grammar for the slice: "drag <dy>".
    bool inject_input(std::string_view request, std::string& error) override {
        if (request.rfind("drag ", 0) == 0) {
            panel_->drag_knob(std::strtof(std::string(request.substr(5)).c_str(), nullptr));
            return true;
        }
        error = "unsupported request";
        return false;
    }
    bool observe(std::string_view, std::string& reply) override {
        reply = "{\"knob\":" + std::to_string(panel_->knob_value()) + "}";
        return true;
    }

  private:
    // Show the panel's latest frame in the host's view.
    void present() {
        if (!layer_view_)
            return;
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        if (panel_->render_rgba(rgba, w, h))
            layer_view_update(layer_view_, rgba.data(), w, h);
    }

    format::EditorContext& context_;
    std::unique_ptr<slice_vellum_ui::KnobPanel> panel_;
    format::ParamSubscription subscription_;
    void* layer_view_ = nullptr;
};

class VellumEditorFactory final : public format::EditorFactory {
  public:
    bool supports(format::EditorRole role) const override {
        return role == format::EditorRole::editor || role == format::EditorRole::inspector;
    }
    std::unique_ptr<format::Editor> create(format::Processor&, format::EditorContext& context,
                                           format::EditorRole role) override {
        if (!supports(role))
            return nullptr;
        return std::make_unique<VellumEditor>(context);
    }
};

} // namespace

format::EditorFactory* slice_editor_factory() {
    static VellumEditorFactory factory;
    return &factory;
}

/// Live Vellum-side panels, for the harness's leak check.
int slice_editor_live_instances() {
    return slice_vellum_ui::live_panels();
}

} // namespace pulp::slice

// The slice's RmlUi editor: a scratch `pulp-rmlui`.
//
// It renders an RmlUi document through Vellum's soft renderer, binds the
// document's `controls.gain` model to the ParamPort, and turns pointer drags on
// the knob into a begin -> set -> end gesture. It defines this binary's
// `slice_editor_factory()`.

#include "gain_meter_processor.hpp"
#include "layer_view_mac.hpp"
#include "slice_rmlui_document.hpp" // generated: kSliceRmlDocument

#include <vellum/rmlui/backend.hpp>
#include <vellum/rmlui/soft_renderer.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <utility>

namespace pulp::slice {

namespace {

namespace rml = vellum::rmlui;

// The knob's box in the document (knob_led.rml): left 24, top 13, 64 px plus a
// 3 px border on each side.
constexpr float kKnobLeft = 24.0f, kKnobTop = 13.0f, kKnobExtent = 70.0f;
// Normalized change per pixel of vertical drag (up increases).
constexpr float kDragPerPixel = 1.0f / 200.0f;

std::atomic<int> g_live{0};

class DocumentAssets final : public rml::AssetProvider {
  public:
    rml::Status load(std::string_view path, rml::Asset& out) override {
        if (path != "knob_led.rml")
            return rml::Status::missing_asset;
        out.path = std::string(path);
        out.bytes = kSliceRmlDocument;
        return rml::Status::ok;
    }
};

class GainModel final : public rml::ModelHost {
  public:
    explicit GainModel(format::ParamPort& params) : params_(params) {}
    rml::Status read(std::string_view field, rml::ModelValue& out) override {
        if (field != "gain")
            return rml::Status::rejected;
        out = rml::ModelValue::number_value(params_.get_normalized(kGainParam));
        return rml::Status::ok;
    }
    rml::Status write(std::string_view, const rml::ModelValue&) override {
        // The document has no setters; edits arrive as pointer gestures.
        return rml::Status::rejected;
    }

  private:
    format::ParamPort& params_;
};

class RmlUiEditor final : public format::Editor, public format::EditorAutomation {
  public:
    explicit RmlUiEditor(format::EditorContext& context)
        : context_(context), renderer_(0x000000ffu), backend_(assets_, renderer_, no_lua_) {
        ++g_live;
        open_backend();
        subscription_ = context_.params().add_listener([this](state::ParamID id, float) {
            if (id == kGainParam && backend_.is_open())
                backend_.dirty_model("controls", "gain");
        });
    }
    ~RmlUiEditor() override {
        detach();
        backend_.close();
        --g_live;
    }

    bool supports_parent(format::NativeParentKind kind) const override {
        return kind == format::NativeParentKind::cocoa_view ||
               kind == format::NativeParentKind::none;
    }
    bool attach(const format::NativeParent& parent) override {
        if (parent.kind == format::NativeParentKind::none)
            return true;
        if (parent.kind != format::NativeParentKind::cocoa_view || !parent.handle)
            return false;
        layer_view_ = layer_view_create(parent.handle, width_, height_);
        render_frame();
        return layer_view_ != nullptr;
    }
    void detach() override {
        if (layer_view_) {
            layer_view_destroy(layer_view_);
            layer_view_ = nullptr;
        }
    }
    void invalidate() override {
        // The parent is gone: drop the subview without touching the parent.
        if (layer_view_) {
            layer_view_abandon(layer_view_);
            layer_view_ = nullptr;
        }
    }
    format::EditorSize size() const override {
        return {width_, height_, 1.0};
    }
    bool set_size(const format::EditorSize& size) override {
        if (size.width < 120 || size.height < 80)
            return false;
        width_ = size.width;
        height_ = size.height;
        backend_.close();
        open_backend();
        if (layer_view_)
            layer_view_resize(layer_view_, width_, height_);
        render_frame();
        return true;
    }
    void idle() override {
        render_frame();
    }
    format::EditorAutomation* automation() override {
        return this;
    }

    // ── automation ──
    bool capture_png(std::vector<std::uint8_t>& png) override {
        render_frame();
        const auto encoded = renderer_.encode_png();
        png.assign(encoded.begin(), encoded.end());
        return !png.empty();
    }
    // Request grammar for the slice: "drag <dy>".
    bool inject_input(std::string_view request, std::string& error) override {
        if (request.rfind("drag ", 0) != 0) {
            error = "unsupported request";
            return false;
        }
        const float dy = std::strtof(std::string(request.substr(5)).c_str(), nullptr);
        drag(dy);
        return true;
    }
    bool observe(std::string_view query, std::string& reply) override {
        if (query == "led") {
            render_frame();
            // LED centre in the document: (122, 48).
            const auto& px = renderer_.pixels();
            const std::size_t i = (48u * renderer_.width() + 122u) * 4u;
            reply = (i + 3 < px.size() && px[i] > 0xc0) ? "on" : "off";
            return true;
        }
        reply = "{\"gain\":" + std::to_string(context_.params().get_normalized(kGainParam)) + "}";
        return true;
    }

  private:
    void open_backend() {
        backend_.open(width_, height_);
        backend_.bind_model("controls", std::make_shared<GainModel>(context_.params()), {"gain"});
        backend_.load_document("knob_led.rml");
    }

    void render_frame() {
        // The first update evaluates bindings; the second lays out what they
        // produced.
        backend_.render();
        backend_.render();
        if (layer_view_)
            layer_view_update(layer_view_, renderer_.pixels().data(), renderer_.width(),
                              renderer_.height());
    }

    void drag(float dy) {
        const int x = static_cast<int>(kKnobLeft + kKnobExtent * 0.5f);
        const int y0 = static_cast<int>(kKnobTop + kKnobExtent * 0.5f);
        auto& params = context_.params();
        const float start = params.get_normalized(kGainParam);
        backend_.process_mouse_move(x, y0);
        backend_.process_mouse_button_down(0);
        params.begin(kGainParam);
        constexpr int kSteps = 5;
        for (int step = 1; step <= kSteps; ++step) {
            const float d = dy * static_cast<float>(step) / kSteps;
            backend_.process_mouse_move(x, y0 + static_cast<int>(d));
            params.set_normalized(kGainParam, std::clamp(start - d * kDragPerPixel, 0.0f, 1.0f));
        }
        backend_.process_mouse_button_up(0);
        params.end(kGainParam);
    }

    format::EditorContext& context_;
    std::uint32_t width_ = 160;
    std::uint32_t height_ = 96;
    DocumentAssets assets_;
    rml::SoftRenderer renderer_;
    rml::HeadlessRecorder no_lua_;
    rml::Backend backend_;
    format::ParamSubscription subscription_;
    void* layer_view_ = nullptr;
};

class RmlUiEditorFactory final : public format::EditorFactory {
  public:
    bool supports(format::EditorRole role) const override {
        return role == format::EditorRole::editor || role == format::EditorRole::inspector;
    }
    std::unique_ptr<format::Editor> create(format::Processor&, format::EditorContext& context,
                                           format::EditorRole role) override {
        if (!supports(role))
            return nullptr;
        return std::make_unique<RmlUiEditor>(context);
    }
};

} // namespace

format::EditorFactory* slice_editor_factory() {
    static RmlUiEditorFactory factory;
    return &factory;
}

int slice_editor_live_instances() {
    return g_live.load();
}

} // namespace pulp::slice

#include "../src/scripted_editor_root.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/editor_prewarm.hpp>
#include <pulp/vellum/scripted_editor.hpp>
#include <vellum/view/scripted_ui_prewarm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
std::vector<::vellum::view::ScriptedUiPrewarmRequest> prewarm_requests;
}

// Replace only the external scheduler, in this executable. The Pulp library's
// static registration, format-core request gate and payload conversion are real.
namespace vellum::view {
void prewarm_scripted_ui(ScriptedUiPrewarmRequest request) {
    prewarm_requests.push_back(std::move(request));
}
} // namespace vellum::view

namespace {
class Processor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor descriptor;
        descriptor.bundle_id = "test.vellum.scripted-wiring";
        descriptor.name = "Scripted wiring";
        return descriptor;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    pulp::format::ViewSize view_size() const override {
        return sizing;
    }
    pulp::format::ViewSize sizing;
    EditorPrewarm editor_prewarm() const override {
        return {{"first script", "second script"}, {"first document", "second document"}};
    }
};

class Context final : public pulp::format::EditorContext {
  public:
    explicit Context(pulp::format::ParamPort& port) : port_(port) {}
    pulp::format::ParamPort& params() override {
        return port_;
    }
    pulp::format::VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const pulp::format::EditorSize&) override {
        return false;
    }
    bool host_action(const pulp::format::HostActionRequest&) override {
        return false;
    }
    pulp::format::EditorHostKind host_kind() const override {
        return pulp::format::EditorHostKind::plugin;
    }

  private:
    pulp::format::ParamPort& port_;
};

struct RecordingSession {
    static inline std::vector<::vellum::render::GpuSurface*> attachments;
    RecordingSession(::vellum::view::View&, ::vellum::value::ParamModel&,
                     ::vellum::view::ScriptedUiOptions) {}
    void set_host_kind(const char*) {}
    bool load(std::string*) {
        return true;
    }
    const char* script_label() const {
        return "recording session";
    }
    bool reload() {
        return true;
    }
    void poll() {}
    void attach_gpu_surface(::vellum::render::GpuSurface* surface) {
        attachments.push_back(surface);
    }
};

class RecordingHost final : public ::vellum::view::PluginViewHost {
  public:
    void publish(::vellum::render::GpuSurface* surface) {
        publish_gpu_surface(surface, surface ? GpuSurfaceState::ready : GpuSurfaceState::pending);
    }
    ::vellum::view::NativeViewHandle native_handle() override {
        return this;
    }
    void attach_to_parent(::vellum::view::NativeViewHandle parent) override {
        attached = parent == this;
    }
    bool is_attached() const noexcept override {
        return attached;
    }
    void detach() override {
        attached = false;
    }
    void repaint() override {}
    void set_size(uint32_t, uint32_t) override {}
    Size get_size() const override {
        return {};
    }
    void set_design_viewport(float width, float height) override {
        design_width = width;
        design_height = height;
    }
    void set_fixed_aspect_ratio(float ratio) override {
        aspect_ratio = ratio;
    }
    float design_width = 0;
    float design_height = 0;
    float aspect_ratio = 0;
    bool attached = false;
};

struct FactoryReset {
    ~FactoryReset() {
        ::vellum::view::PluginViewHost::clear_factory();
    }
};
} // namespace

TEST_CASE("Scripted root forwards surface replacement and unsubscribes on close",
          "[pulp-vellum][scripted-wiring][surface]") {
    FactoryReset reset;
    RecordingSession::attachments.clear();
    pulp::state::StateStore store;
    pulp::format::StateStoreParamPort port(store);
    Context context(port);
    Processor processor;
    RecordingHost* host = nullptr;
    ::vellum::view::PluginViewHost::set_factory(
        [&](::vellum::view::View&, const ::vellum::view::PluginViewHost::Options&) {
            auto result = std::make_unique<RecordingHost>();
            host = result.get();
            return result;
        });
    using Root = pulp::vellum::detail::ScriptedEditorRoot<RecordingSession>;
    pulp::vellum::Editor<Root> editor(context, processor);
    REQUIRE(host);
    // Opaque identities: the recording session never dereferences GPU objects.
    int first_identity = 0, second_identity = 0;
    auto* first = reinterpret_cast<::vellum::render::GpuSurface*>(&first_identity);
    auto* second = reinterpret_cast<::vellum::render::GpuSurface*>(&second_identity);
    host->publish(first);
    REQUIRE(RecordingSession::attachments.empty());
    const pulp::format::NativeParent parent{.kind = pulp::format::NativeParentKind::cocoa_view,
                                            .handle = host};
    REQUIRE(editor.attach(parent));
    REQUIRE(RecordingSession::attachments == std::vector{first});
    host->publish(second);
    REQUIRE(RecordingSession::attachments == std::vector{first, second});
    editor.detach();
    REQUIRE(RecordingSession::attachments ==
            std::vector<::vellum::render::GpuSurface*>{first, second, nullptr});
    host->publish(first);
    REQUIRE(RecordingSession::attachments.size() == 3);
    REQUIRE(editor.attach(parent));
    REQUIRE(RecordingSession::attachments.back() == first);
    REQUIRE(RecordingSession::attachments.size() == 4);
    host->publish(second);
    REQUIRE(RecordingSession::attachments.back() == second);
    REQUIRE(RecordingSession::attachments.size() == 5);
    editor.detach();
    REQUIRE(RecordingSession::attachments.back() == nullptr);
    REQUIRE(RecordingSession::attachments.size() == 6);
    host->publish(nullptr);
    REQUIRE(RecordingSession::attachments.size() == 6);
}

TEST_CASE("Format-core prewarm reaches the Vellum scheduler with both payload lists",
          "[pulp-vellum][scripted-wiring][prewarm]") {
    // Referencing the public factory pulls the production registration object
    // out of its archive; the test must not install the scheduler itself.
    REQUIRE(pulp::vellum::scripted_editor_factory());
    pulp::format::detail::reset_editor_prewarm_requests_for_tests();
    prewarm_requests.clear();
    Processor processor;
    REQUIRE(pulp::format::request_editor_prewarm(processor));
    REQUIRE(prewarm_requests.size() == 1);
    CHECK(prewarm_requests[0].scripts == std::vector<std::string>{"first script", "second script"});
    CHECK(prewarm_requests[0].materialized_documents ==
          std::vector<std::string>{"first document", "second document"});
    CHECK_FALSE(pulp::format::request_editor_prewarm(processor));
    CHECK(prewarm_requests.size() == 1);
}

TEST_CASE("Scripted root preserves processor resize bounds and design viewport",
          "[pulp-vellum][scripted-wiring][sizing]") {
    FactoryReset reset;
    pulp::state::StateStore store;
    pulp::format::StateStoreParamPort port(store);
    Context context(port);
    Processor processor;
    processor.sizing = {.preferred_width = 600,
                        .preferred_height = 400,
                        .min_width = 300,
                        .min_height = 200,
                        .max_width = 1200,
                        .max_height = 800,
                        .aspect_ratio = 1.5,
                        .design_width = 900,
                        .design_height = 600};
    RecordingHost* host = nullptr;
    ::vellum::view::PluginViewHost::set_factory(
        [&](::vellum::view::View&, const ::vellum::view::PluginViewHost::Options&) {
            auto result = std::make_unique<RecordingHost>();
            host = result.get();
            return result;
        });
    using Root = pulp::vellum::detail::ScriptedEditorRoot<RecordingSession>;
    pulp::vellum::Editor<Root> editor(context, processor);
    REQUIRE(host);
    CHECK(editor.size().width == 600);
    CHECK(editor.size().height == 400);
    const auto hints = editor.size_hints();
    CHECK(hints.min_width == 300);
    CHECK(hints.min_height == 200);
    CHECK(hints.max_width == 1200);
    CHECK(hints.max_height == 800);
    CHECK(hints.aspect_ratio == 1.5);
    CHECK(hints.design_width == 900);
    CHECK(hints.design_height == 600);
    CHECK(host->design_width == 900);
    CHECK(host->design_height == 600);
    CHECK(host->aspect_ratio == 1.5);
    REQUIRE(editor.set_size({150, 100, 1.0}));
    CHECK(editor.size().width == 300);
    CHECK(editor.size().height == 200);
    REQUIRE(editor.set_size({1800, 1200, 1.0}));
    CHECK(editor.size().width == 1200);
    CHECK(editor.size().height == 800);
}

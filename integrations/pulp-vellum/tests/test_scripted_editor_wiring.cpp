#include "../src/scripted_editor_root.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/editor_assets.hpp>
#include <pulp/format/editor_prewarm.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/vellum/scripted_editor.hpp>
#include <vellum/view/scripted_ui_prewarm.hpp>

#include "support/portable_env.hpp"
#include "support/unique_temp_dir.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
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
        descriptor.bundle_id = bundle_id;
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
    std::string bundle_id = "test.vellum.scripted-wiring";
    EditorPrewarm editor_prewarm() const override {
        return {{"first script", "second script"}, {"first document", "second document"}};
    }
};

class Context final : public pulp::format::EditorContext {
  public:
    explicit Context(pulp::format::ParamPort& port,
                     pulp::format::EditorHostKind kind = pulp::format::EditorHostKind::plugin)
        : port_(port), kind_(kind) {}
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
        return kind_;
    }

  private:
    pulp::format::ParamPort& port_;
    pulp::format::EditorHostKind kind_;
};

struct RecordingSession {
    static inline std::vector<::vellum::render::GpuSurface*> attachments;
    static inline std::optional<::vellum::view::ScriptedUiOptions> options;
    static inline std::string host_kind;
    static inline bool load_succeeds = true;
    RecordingSession(::vellum::view::View&, ::vellum::value::ParamModel&,
                     ::vellum::view::ScriptedUiOptions opened_with) {
        options = std::move(opened_with);
    }
    void set_host_kind(const char* kind) {
        host_kind = kind;
    }
    bool load(std::string* error) {
        if (!load_succeeds && error)
            *error = "no such script";
        return load_succeeds;
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

namespace {

constexpr unsigned char kEmbeddedScript[] = "createLabel('status', 'embedded', '');\n";

// The binary-wide editor record a UI_SCRIPT target registers, plus the on-disk
// copy a dev build also records, cleared again when the case ends.
struct RegisteredEditorScript {
    pulp::test::ScopedTempDir dir{"pulp-vellum-scripted-assets"};
    std::string dev_path = (dir.path / "main.js").string();
    std::optional<std::string> saved_hot_reload;

    RegisteredEditorScript() {
        std::ofstream(dev_path) << "createLabel('status', 'disk', '');\n";
        if (const char* value = std::getenv("PULP_DEV_HOT_RELOAD"))
            saved_hot_reload = value;
        pulp::test::unset_env_var("PULP_DEV_HOT_RELOAD");
        pulp::format::EditorAssets assets;
        assets.script_path = dev_path.c_str();
        assets.script_name = "main.js";
        assets.script = kEmbeddedScript;
        assets.script_size = sizeof(kEmbeddedScript) - 1;
        pulp::format::register_editor_assets(assets);
        RecordingSession::options.reset();
        RecordingSession::host_kind.clear();
    }
    ~RegisteredEditorScript() {
        pulp::format::reset_editor_assets_for_testing();
        pulp::format::reset_registry_for_testing();
        if (saved_hot_reload)
            pulp::test::set_env_var("PULP_DEV_HOT_RELOAD", saved_hot_reload->c_str());
        else
            pulp::test::unset_env_var("PULP_DEV_HOT_RELOAD");
    }
};

// Opens the scripted root the way a format adapter does and returns the
// options its script session was built with.
::vellum::view::ScriptedUiOptions open_scripted_root(Processor& processor,
                                                     pulp::format::EditorHostKind kind) {
    FactoryReset reset;
    pulp::state::StateStore store;
    pulp::format::StateStoreParamPort port(store);
    Context context(port, kind);
    ::vellum::view::PluginViewHost::set_factory(
        [](::vellum::view::View&, const ::vellum::view::PluginViewHost::Options&) {
            return std::make_unique<RecordingHost>();
        });
    using Root = pulp::vellum::detail::ScriptedEditorRoot<RecordingSession>;
    pulp::vellum::Editor<Root> editor(context, processor);
    REQUIRE(RecordingSession::options);
    return *RecordingSession::options;
}

} // namespace

TEST_CASE("Scripted root evaluates the embedded script in a plug-in host",
          "[pulp-vellum][scripted-wiring][editor-assets]") {
    RegisteredEditorScript registered;
    Processor processor;
    const auto options = open_scripted_root(processor, pulp::format::EditorHostKind::plugin);
    CHECK(RecordingSession::host_kind == "plugin");
    CHECK(options.script_source == reinterpret_cast<const char*>(kEmbeddedScript));
    CHECK(options.script_name == "main.js");
    CHECK(options.script_path.empty());
    CHECK_FALSE(options.enable_hot_reload);
    CHECK(options.storage_identity == "test.vellum.scripted-wiring/main.js");
}

TEST_CASE("Scripted root reads the dev build's script file only when it hot reloads",
          "[pulp-vellum][scripted-wiring][editor-assets]") {
    RegisteredEditorScript registered;
    Processor processor;

    SECTION("the standalone app is the dev tool and always reloads") {
        const auto options =
            open_scripted_root(processor, pulp::format::EditorHostKind::standalone);
        CHECK(RecordingSession::host_kind == "standalone");
        CHECK(options.enable_hot_reload);
        CHECK(options.script_source.empty());
        CHECK(options.script_path == std::filesystem::path(registered.dev_path));
    }
    SECTION("a plug-in host reloads when the developer exports PULP_DEV_HOT_RELOAD") {
        REQUIRE(pulp::test::set_env_var("PULP_DEV_HOT_RELOAD", "1") == 0);
        const auto options = open_scripted_root(processor, pulp::format::EditorHostKind::plugin);
        CHECK(RecordingSession::host_kind == "plugin");
        CHECK(options.enable_hot_reload);
        CHECK(options.script_path == std::filesystem::path(registered.dev_path));
    }
    SECTION("a reloading editor keeps the embedded script once the file is gone") {
        std::filesystem::remove(registered.dev_path);
        const auto options =
            open_scripted_root(processor, pulp::format::EditorHostKind::standalone);
        CHECK(options.enable_hot_reload);
        CHECK(options.script_source == reinterpret_cast<const char*>(kEmbeddedScript));
        CHECK(options.script_path.empty());
    }
}

TEST_CASE("Scripted root opens the editor its own bundle entry registered",
          "[pulp-vellum][scripted-wiring][editor-assets]") {
    RegisteredEditorScript registered;
    const auto keyed_path = (registered.dir.path / "keyed.js").string();
    std::ofstream(keyed_path) << "createLabel('status', 'keyed', '');\n";
    pulp::format::register_plugin(pulp::format::PluginRegistration{
        .id = "test.vellum.keyed",
        .factory = nullptr,
        .ui_script = keyed_path.c_str(),
    });

    Processor keyed;
    keyed.bundle_id = "test.vellum.keyed";
    const auto keyed_options = open_scripted_root(keyed, pulp::format::EditorHostKind::plugin);
    CHECK(keyed_options.script_path == std::filesystem::path(keyed_path));
    CHECK(keyed_options.script_source.empty());

    // Another plug-in in the same image falls back to the binary-wide record.
    Processor other;
    other.bundle_id = "test.vellum.other";
    const auto other_options = open_scripted_root(other, pulp::format::EditorHostKind::plugin);
    CHECK(other_options.script_source == reinterpret_cast<const char*>(kEmbeddedScript));
    CHECK(other_options.storage_identity == "test.vellum.other/main.js");
}

TEST_CASE("A scripted editor whose script does not load opens no editor",
          "[pulp-vellum][scripted-wiring][editor-open]") {
    FactoryReset reset;
    pulp::state::StateStore store;
    pulp::format::StateStoreParamPort port(store);
    Context context(port);
    Processor processor;
    int hosts_created = 0;
    ::vellum::view::PluginViewHost::set_factory(
        [&](::vellum::view::View&, const ::vellum::view::PluginViewHost::Options&) {
            ++hosts_created;
            return std::make_unique<RecordingHost>();
        });
    using Root = pulp::vellum::detail::ScriptedEditorRoot<RecordingSession>;
    pulp::vellum::EditorFactory<Root> factory;

    RecordingSession::load_succeeds = false;
    auto failed = factory.create(processor, context, pulp::format::EditorRole::editor);
    RecordingSession::load_succeeds = true;
    CHECK(failed == nullptr);
    CHECK(hosts_created == 0);

    auto opened = factory.create(processor, context, pulp::format::EditorRole::editor);
    CHECK(opened != nullptr);
    CHECK(hosts_created == 1);
}

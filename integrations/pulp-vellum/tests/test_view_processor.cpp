// Built with PULP_FORMAT_LEGACY_VIEW=0, as in an SDK whose UI comes from
// Vellum. First, so the header proves it compiles on its own in that mode.
#include <pulp/vellum/view_processor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <vellum/view/plugin_view_host.hpp>
#include <vellum/view/ui_components.hpp>
#include <vellum/view/view.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using pulp::format::EditorRole;
using pulp::format::NativeParentKind;
using PluginViewHost = ::vellum::view::PluginViewHost;

static_assert(!PULP_FORMAT_LEGACY_VIEW, "this test covers the UI-free SDK");

class Context final : public pulp::format::EditorContext {
  public:
    explicit Context(pulp::format::ParamPort& params) : params_(params) {}
    pulp::format::ParamPort& params() override {
        return params_;
    }
    pulp::format::VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const pulp::format::EditorSize&) override {
        return true;
    }
    bool host_action(const pulp::format::HostActionRequest&) override {
        return false;
    }
    pulp::format::EditorHostKind host_kind() const override {
        return pulp::format::EditorHostKind::plugin;
    }

  private:
    pulp::format::ParamPort& params_;
};

class StubHost final : public PluginViewHost {
  public:
    explicit StubHost(Size size) : size_(size) {}
    ::vellum::view::NativeViewHandle native_handle() override {
        return this;
    }
    void attach_to_parent(::vellum::view::NativeViewHandle parent) override {
        attached_ = parent == this;
    }
    bool is_attached() const noexcept override {
        return attached_;
    }
    void detach() override {
        attached_ = false;
    }
    void repaint() override {}
    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
    }
    Size get_size() const override {
        return size_;
    }

  private:
    Size size_;
    bool attached_ = false;
};

class Panel final : public ::vellum::view::View {
  public:
    explicit Panel(int id) : id(id) {}
    int id;
};

// A plug-in as it reads after the migration: the base class is the only
// change from a create_view() plug-in on Pulp's own view layer.
class Plugin final : public pulp::vellum::ViewProcessor<> {
  public:
    explicit Plugin(int id) : id(id) {}

    pulp::format::PluginDescriptor descriptor() const override {
        return {};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    pulp::format::ViewSize view_size() const override {
        return {.preferred_width = 500,
                .preferred_height = 300,
                .min_width = 250,
                .min_height = 150,
                .max_width = 1000,
                .max_height = 600};
    }

    std::unique_ptr<::vellum::view::View> create_view() override {
        ++created;
        return has_editor_view ? std::make_unique<Panel>(id) : nullptr;
    }
    void on_view_opened(::vellum::view::View& view) override {
        ++opened;
        last_opened = static_cast<Panel&>(view).id;
    }
    void on_view_closed(::vellum::view::View&) override {
        ++closed;
    }
    void on_view_resized(::vellum::view::View&, std::uint32_t w, std::uint32_t h) override {
        resized.emplace_back(w, h);
    }

    int id;
    bool has_editor_view = true;
    int created = 0;
    int opened = 0;
    int closed = 0;
    int last_opened = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> resized;
};

// An inspector window is opt-in through create_view(role).
class InspectedPlugin final : public pulp::vellum::ViewProcessor<> {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    std::unique_ptr<::vellum::view::View> create_view() override {
        return std::make_unique<Panel>(1);
    }
    std::unique_ptr<::vellum::view::View> create_view(EditorRole role) override {
        return std::make_unique<Panel>(role == EditorRole::inspector ? 2 : 1);
    }
    bool has_view(EditorRole role) const override {
        return role == EditorRole::editor || role == EditorRole::inspector;
    }
};

// A plug-in whose editor view exists only some of the time says so.
class SometimesPlugin final : public pulp::vellum::ViewProcessor<> {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    std::unique_ptr<::vellum::view::View> create_view() override {
        ++built;
        return available ? std::make_unique<Panel>(5) : nullptr;
    }
    bool has_view(EditorRole role) const override {
        return role == EditorRole::editor && available;
    }
    bool available = false;
    int built = 0;
};

// Settings sections as a create_view() plug-in contributes them.
class SettingsPlugin final : public pulp::vellum::ViewProcessor<> {
  public:
    explicit SettingsPlugin(int sections) : sections(sections) {}
    pulp::format::PluginDescriptor descriptor() const override {
        return {};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    std::unique_ptr<::vellum::view::View> create_view() override {
        return std::make_unique<Panel>(0);
    }
    std::vector<pulp::vellum::ViewSettingsSection> settings_sections() override {
        ++section_builds;
        std::vector<pulp::vellum::ViewSettingsSection> out;
        for (int i = 0; i < sections; ++i)
            out.push_back({"Section " + std::to_string(i), std::make_unique<Panel>(100 + i)});
        return out;
    }
    bool has_settings() const override {
        return sections > 0;
    }
    void on_view_opened(::vellum::view::View& view) override {
        opened_view = &view;
    }

    int sections;
    int section_builds = 0;
    ::vellum::view::View* opened_view = nullptr;
};

struct Host {
    pulp::state::StateStore store;
    pulp::format::StateStoreParamPort port{store};
    Context context{port};
    StubHost* host = nullptr;

    Host() {
        PluginViewHost::set_factory(
            [this](::vellum::view::View&, const PluginViewHost::Options& options) {
                auto stub = std::make_unique<StubHost>(options.size);
                host = stub.get();
                return stub;
            });
    }
    ~Host() {
        PluginViewHost::clear_factory();
    }

    std::unique_ptr<pulp::format::Editor> open(pulp::format::Processor& processor,
                                               EditorRole role = EditorRole::editor) {
        processor.set_state_store(&store);
        auto* factory = processor.editor_factory();
        REQUIRE(factory != nullptr);
        return factory->create(processor, context, role);
    }

    pulp::format::NativeParent parent() const {
        return {.kind = NativeParentKind::cocoa_view, .handle = host};
    }
};

} // namespace

TEST_CASE("A ViewProcessor opens its create_view() tree through its editor factory",
          "[pulp-vellum][view-processor]") {
    Host h;
    Plugin plugin(7);
    auto editor = h.open(plugin);
    REQUIRE(editor != nullptr);
    CHECK(plugin.created == 1);
    CHECK(editor->size_hints().max_width == 1000);

    REQUIRE(editor->attach(h.parent()));
    CHECK(plugin.opened == 1);
    CHECK(plugin.last_opened == 7);
    CHECK(editor->set_size({800, 480, 2.0}));
    REQUIRE(plugin.resized.size() == 1);
    CHECK(plugin.resized[0] == std::pair<std::uint32_t, std::uint32_t>{800, 480});
    editor->detach();
    CHECK(plugin.closed == 1);
}

TEST_CASE("A ViewProcessor whose create_view() returns null has no editor",
          "[pulp-vellum][view-processor]") {
    Host h;
    Plugin plugin(1);
    plugin.has_editor_view = false;
    CHECK(h.open(plugin) == nullptr);
    CHECK(plugin.created == 1);
}

TEST_CASE("A ViewProcessor offers only the roles has_view names, and builds nothing to answer",
          "[pulp-vellum][view-processor][roles]") {
    Host h;
    SometimesPlugin plugin;
    plugin.set_state_store(&h.store);
    auto* factory = plugin.editor_factory();
    REQUIRE(factory != nullptr);
    CHECK_FALSE(factory->supports(EditorRole::editor));
    CHECK_FALSE(factory->supports(EditorRole::inspector));
    CHECK_FALSE(factory->supports(EditorRole::settings));
    CHECK(plugin.built == 0);
    CHECK(factory->create(plugin, h.context, EditorRole::editor) == nullptr);
    CHECK(plugin.built == 0);

    plugin.available = true;
    CHECK(factory->supports(EditorRole::editor));
    CHECK(plugin.built == 0);
    CHECK(factory->create(plugin, h.context, EditorRole::editor) != nullptr);
    CHECK(plugin.built == 1);
}

TEST_CASE("Two ViewProcessors share one factory and each opens its own tree",
          "[pulp-vellum][view-processor]") {
    Host h;
    Plugin first(1);
    Plugin second(2);
    // Each processor owns its factory, as the editor-host contract asks.
    CHECK(first.editor_factory() != second.editor_factory());
    auto a = h.open(first);
    auto* host_a = h.host;
    auto b = h.open(second);
    auto* host_b = h.host;
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->attach({.kind = NativeParentKind::cocoa_view, .handle = host_a}));
    REQUIRE(b->attach({.kind = NativeParentKind::cocoa_view, .handle = host_b}));
    CHECK(first.opened == 1);
    CHECK(first.last_opened == 1);
    CHECK(second.opened == 1);
    CHECK(second.last_opened == 2);
}

TEST_CASE("A ViewProcessor offers an inspector window only when it builds one",
          "[pulp-vellum][view-processor][roles]") {
    Host h;
    Plugin plain(1);
    CHECK(h.open(plain, EditorRole::inspector) == nullptr);
    CHECK(plain.created == 0);

    InspectedPlugin inspected;
    auto main = h.open(inspected, EditorRole::editor);
    auto inspector = h.open(inspected, EditorRole::inspector);
    CHECK(main != nullptr);
    CHECK(inspector != nullptr);
}

TEST_CASE("A ViewProcessor's settings sections are its settings editor",
          "[pulp-vellum][view-processor][settings]") {
    Host h;
    // No sections: the settings role is not offered, and asking builds no view.
    SettingsPlugin none(0);
    CHECK_FALSE(none.editor_factory()->supports(EditorRole::settings));
    CHECK(none.editor_factory()->supports(EditorRole::editor));
    CHECK(none.section_builds == 0);

    SettingsPlugin offered(2);
    CHECK(offered.editor_factory()->supports(EditorRole::settings));
    CHECK(offered.section_builds == 0);

    SettingsPlugin one(1);
    auto single = h.open(one, EditorRole::settings);
    REQUIRE(single != nullptr);
    REQUIRE(single->attach(h.parent()));
    auto* panel = dynamic_cast<Panel*>(one.opened_view);
    REQUIRE(panel != nullptr);
    CHECK(panel->id == 100);
    single->detach();

    SettingsPlugin two(2);
    auto tabs = h.open(two, EditorRole::settings);
    REQUIRE(tabs != nullptr);
    REQUIRE(tabs->attach(h.parent()));
    auto* tab_panel = dynamic_cast<::vellum::view::TabPanel*>(two.opened_view);
    REQUIRE(tab_panel != nullptr);
    CHECK(tab_panel->child_count() == 2);
    tabs->detach();

    // The main editor is unaffected.
    auto editor = h.open(two, EditorRole::editor);
    REQUIRE(editor != nullptr);
    REQUIRE(editor->attach(h.parent()));
    CHECK(dynamic_cast<Panel*>(two.opened_view) != nullptr);
}

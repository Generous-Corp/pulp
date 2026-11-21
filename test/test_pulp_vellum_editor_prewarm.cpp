#include <pulp/format/editor_prewarm.hpp>
#include <pulp/vellum/editor.hpp>

#include <catch2/catch_test_macros.hpp>
#include <vellum/view/scripted_ui_prewarm.hpp>

#include <chrono>
#include <string>

namespace {

class RootView final : public ::vellum::view::View {
  public:
    RootView(pulp::vellum::EditorEnv&, pulp::format::Processor&) {}
};

class Processor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {"Prewarm", "Pulp", "org.pulp.test.vellum-prewarm", "1.0.0",
                pulp::format::PluginCategory::Effect};
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    EditorPrewarm editor_prewarm() const override {
        static const auto script = [] {
            std::string source = "var vellumPrewarmFixture = 0;\n";
            while (source.size() < 8192)
                source += "vellumPrewarmFixture += 1;\n";
            return source;
        }();
        return {{script}, {}};
    }

    pulp::format::EditorFactory* editor_factory() override {
        return pulp::vellum::editor_factory<RootView>();
    }
};

} // namespace

TEST_CASE("Vellum prewarms a custom editor before its factory runs", "[editor-prewarm]") {
    Processor processor;
    const auto before = ::vellum::view::scripted_ui_prewarm_stats();
    REQUIRE(pulp::format::request_editor_prewarm(processor));
    REQUIRE(::vellum::view::wait_for_scripted_ui_prewarm(std::chrono::seconds(20)));
    const auto after = ::vellum::view::scripted_ui_prewarm_stats();
    CHECK(after.requests == before.requests + 1);
    CHECK(after.scripts_compiled == before.scripts_compiled + 1);
    CHECK(processor.editor_factory()->supports(pulp::format::EditorRole::editor));
}

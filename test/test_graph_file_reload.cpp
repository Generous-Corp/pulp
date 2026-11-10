#include "../examples/graph-patch-consumer/hello.hpp"
#include "support/unique_temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/graph_file_reload.hpp>
#include <fstream>
#include <utility>
#include <thread>

using namespace pulp::host;
namespace {
std::string patch(float value) {
    return "# flow 1\ninput : pulp/audio_in\nvoice : \"com.example.PatchGain\"\n"
           "out : pulp/audio_out\ninput.audio >> voice.audio\nvoice.audio >> out.audio\n"
           "voice.level = " + std::to_string(value) + "\n";
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    REQUIRE(file.good());
}
template <typename Graph> void audio_is(Graph& graph, float expected) {
    pulp::audio::Buffer<float> input(2, 64), output(2, 64);
    for (std::size_t ch = 0; ch != 2; ++ch)
        for (auto& value : input.channel(ch)) value = 0.5f;
    auto destination = output.view();
    graph.process(destination, std::as_const(input).view(), 64);
    for (const auto sample : output.channel(0)) REQUIRE(sample == expected);
    for (const auto sample : output.channel(1)) REQUIRE(sample == 0.0f);
}
std::unique_ptr<SignalGraph> live(GraphPatchCatalog& catalog) {
    register_patch_gain(catalog);
    auto initial = parse_graph_patch(patch(0.25f), catalog);
    REQUIRE(initial);
    REQUIRE(initial.graph->prepare(48000, 64));
    audio_is(*initial.graph, 0.125f);
    return std::move(initial.graph);
}
}

TEST_CASE("Graph file change prepares off-side and publishes the next audio boundary",
          "[host][graph-file-reload]") {
    GraphPatchCatalog catalog;
    auto graph = live(catalog);
    pulp::test::ScopedTempDir directory("pulp-graph-file-reload");
    const auto file = directory.path / "live.pat";
    GraphFileWatch watch(file);
    write(file, patch(0.25f));
    REQUIRE(watch.poll().code == GraphFileChange::Code::Pending);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Changed);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
    // A real write with the same size and restored mtime must still be seen.
    const auto timestamp = std::filesystem::last_write_time(file);
    write(file, patch(0.75f));
    std::filesystem::last_write_time(file, timestamp);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Pending);
    auto changed = watch.poll();
    REQUIRE(changed.code == GraphFileChange::Code::Changed);
    const auto before = GraphSerializer::to_json(*graph);
    const auto generation = graph->prepared_stats().snapshot_generation;
    GraphFileReloadTransaction reload(*graph);
    const auto prepared = reload.prepare(changed.text, changed.format, catalog, 48000, 64);
    INFO(graph_file_reload_code_name(prepared.code));
    INFO(static_cast<int>(prepared.topology_result));
    REQUIRE(prepared);
    REQUIRE(GraphSerializer::to_json(*graph) == before);
    REQUIRE(graph->prepared_stats().snapshot_generation == generation);
    audio_is(*graph, 0.125f);
    REQUIRE(reload.commit().code == GraphFileReloadCode::Committed);
    audio_is(*graph, 0.375f);
    REQUIRE(GraphSerializer::to_json(*graph) != before);
    const auto formatted = format_graph_patch(*graph, catalog);
    REQUIRE(formatted);
    auto expected = parse_graph_patch(patch(0.75f), catalog);
    REQUIRE(expected);
    REQUIRE(formatted.text == format_graph_patch(*expected.graph, catalog).text);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
}

TEST_CASE("Graph reload malformed unsupported and failed prepare preserve live state and audio",
          "[host][graph-file-reload]") {
    GraphPatchCatalog catalog;
    auto graph = live(catalog);
    auto limits = graph->limits();
    limits.max_nodes = 3;
    graph->set_limits(limits);
    REQUIRE(graph->prepare(48000, 64));
    const auto before = GraphSerializer::to_json(*graph);
    const auto generation = graph->prepared_stats().snapshot_generation;
    std::string text;
    GraphFileFormat format = GraphFileFormat::Notation;
    SECTION("malformed edge") { text = patch(0.75f) + "input.nope >> out.audio\n"; }
    SECTION("unsupported node") { text = patch(0.75f) + "other : unknown/module\n"; }
    SECTION("compile preflight rejects candidate node count") {
        text = patch(0.75f) + "extra : pulp/gain\n";
    }
    SECTION("strict saved mono boundary is unrepresentable") {
        SignalGraph unsupported;
        auto in = unsupported.add_input_node(1), out = unsupported.add_output_node(1);
        REQUIRE(unsupported.connect(in, 0, out, 0));
        text = GraphSerializer::to_json(unsupported);
        format = GraphFileFormat::Pulpgraph;
    }
    GraphFileReloadTransaction reload(*graph);
    REQUIRE_FALSE(reload.prepare(text, format, catalog, 48000, 64));
    REQUIRE_FALSE(reload.commit());
    REQUIRE(GraphSerializer::to_json(*graph) == before);
    REQUIRE(graph->prepared_stats().snapshot_generation == generation);
    audio_is(*graph, 0.125f);
}

TEST_CASE("Graph reload stale prepared edit cannot overwrite a competing publication",
          "[host][graph-file-reload]") {
    GraphPatchCatalog catalog;
    auto graph = live(catalog);
    GraphFileReloadTransaction reload(*graph);
    const auto prepared = reload.prepare(patch(0.75f), GraphFileFormat::Notation, catalog, 48000, 64);
    INFO(graph_file_reload_code_name(prepared.code));
    INFO(static_cast<int>(prepared.topology_result));
    REQUIRE(prepared);
    auto competing = graph->begin_prepared_topology_edit();
    REQUIRE(competing->add_gain_node("unrelated"));
    REQUIRE(competing->prepare(48000, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(competing->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto before = GraphSerializer::to_json(*graph);
    const auto generation = graph->prepared_stats().snapshot_generation;
    REQUIRE(reload.commit().code == GraphFileReloadCode::StaleBase);
    REQUIRE(GraphSerializer::to_json(*graph) == before);
    REQUIRE(graph->prepared_stats().snapshot_generation == generation);
    audio_is(*graph, 0.125f);
}

TEST_CASE("Graph file watcher bounds errors and coalesces repeated or partial saves",
          "[host][graph-file-reload]") {
    pulp::test::ScopedTempDir directory("pulp-graph-watch");
    const auto file = directory.path / "live.pat";
    GraphFileWatch watch(file);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unreadable);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
    write(file, "partial");
    REQUIRE(watch.poll().code == GraphFileChange::Code::Pending);
    write(file, patch(0.75f));
    REQUIRE(watch.poll().code == GraphFileChange::Code::Pending);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Changed);
    write(file, std::string(1024 * 1024 + 1, 'x'));
    REQUIRE(watch.poll().code == GraphFileChange::Code::TooLarge);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
    GraphFileWatch bad_extension(directory.path / "live.cpp");
    REQUIRE(bad_extension.poll().code == GraphFileChange::Code::UnsupportedFormat);
}

namespace {
struct LifecycleCounts {
    int prepares = 0;
    int releases = 0;
    std::thread::id release_thread;
};
class ReloadLifecycleGain final : public pulp::format::Processor {
  public:
    explicit ReloadLifecycleGain(LifecycleCounts& counts) : counts_(counts) {}
    pulp::format::PluginDescriptor descriptor() const override {
        auto descriptor = PatchGain{}.descriptor();
        descriptor.accepts_midi = false;
        descriptor.produces_midi = false;
        return descriptor;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = 7, .name = "level", .range = {0, 2, 0.25f}});
    }
    void prepare(const pulp::format::PrepareContext&) override { ++counts_.prepares; }
    void release() override {
        ++counts_.releases;
        counts_.release_thread = std::this_thread::get_id();
    }
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
            output.channel(0)[frame] = input.channel(0)[frame] * state().get_value(7);
    }
  private:
    LifecycleCounts& counts_;
};
}

TEST_CASE("Prepared replacement refuses removed processor aliases before any preparation",
          "[host][graph-file-reload][lifecycle]") {
    LifecycleCounts baseline, fresh;
    SignalGraph graph;
    const auto input = graph.add_input_node(2), output = graph.add_output_node(2);
    auto instance = pulp::format::ProcessorNodeInstance::create(
        std::make_unique<ReloadLifecycleGain>(baseline));
    const auto processor = graph.add_processor_node(instance);
    REQUIRE(graph.connect(input, 0, processor, 0));
    REQUIRE(graph.connect(processor, 0, output, 0));
    REQUIRE(graph.prepare(48000, 64));
    REQUIRE(baseline.prepares == 1);
    audio_is(graph, 0.125f);
    const auto before = GraphSerializer::to_json(graph);
    {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->remove_node(processor));
        REQUIRE(edit->add_processor_node(std::make_unique<ReloadLifecycleGain>(fresh)));
        REQUIRE(edit->add_processor_node(instance));
        SECTION("prepare refuses alias before preparing fresh or live instance") {
            REQUIRE(edit->prepare(48000, 64) ==
                    SignalGraph::PreparedTopologyEdit::Result::ExternalPluginReprepareRequired);
            REQUIRE(fresh.prepares == 0);
            REQUIRE(baseline.prepares == 1);
            REQUIRE(baseline.releases == 0);
            REQUIRE(GraphSerializer::to_json(graph) == before);
            audio_is(graph, 0.125f);
        }
        SECTION("abandoned candidate cannot release live instance") {}
        SECTION("stale candidate cannot release live instance") {
            auto competing = graph.begin_prepared_topology_edit();
            REQUIRE(competing->add_gain_node("competing change"));
            REQUIRE(competing->prepare(48000, 64) ==
                    SignalGraph::PreparedTopologyEdit::Result::Prepared);
            REQUIRE(competing->commit() ==
                    SignalGraph::PreparedTopologyEdit::Result::Committed);
            REQUIRE(edit->prepare(48000, 64) ==
                    SignalGraph::PreparedTopologyEdit::Result::StaleBase);
        }
    }
    REQUIRE(baseline.releases == 0);
    audio_is(graph, 0.125f);
}

TEST_CASE("Replacement retains old processor audio until last snapshot retires off audio",
          "[host][graph-file-reload][lifecycle]") {
    LifecycleCounts old_counts, new_counts;
    SignalGraph graph;
    const auto input = graph.add_input_node(2), output = graph.add_output_node(2);
    auto old_instance = pulp::format::ProcessorNodeInstance::create(
        std::make_unique<ReloadLifecycleGain>(old_counts));
    const auto old_id = graph.add_processor_node(old_instance);
    REQUIRE(graph.connect(input, 0, old_id, 0));
    REQUIRE(graph.connect(old_id, 0, output, 0));
    REQUIRE(graph.prepare(48000, 64));
    SignalGraph::ExecutionSnapshot retained;
    {
        auto pin = graph.begin_prepared_topology_edit();
        REQUIRE(pin->prepare(48000, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
        REQUIRE(pin->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
        retained = pin->committed_execution_snapshot();
        REQUIRE(retained);
    }
    audio_is(retained, 0.125f);
    {
        auto replacement = graph.begin_prepared_topology_edit();
        REQUIRE(replacement->remove_node(old_id));
        auto new_instance = pulp::format::ProcessorNodeInstance::create(
            std::make_unique<ReloadLifecycleGain>(new_counts));
        new_instance->state_store().set_value(7, 0.75f);
        const auto new_id = replacement->add_processor_node(new_instance);
        REQUIRE(new_id);
        REQUIRE(replacement->connect(input, 0, new_id, 0));
        REQUIRE(replacement->connect(new_id, 0, output, 0));
        REQUIRE(replacement->prepare(48000, 64) ==
                SignalGraph::PreparedTopologyEdit::Result::Prepared);
        REQUIRE(old_counts.prepares == 1);
        REQUIRE(new_counts.prepares == 1);
        audio_is(graph, 0.125f);
        REQUIRE(replacement->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    }
    audio_is(graph, 0.375f);
    audio_is(retained, 0.125f);
    REQUIRE(old_counts.releases == 0);
    retained = {};
    // A control-thread publication also reclaims retired snapshots.
    {
        auto reclaim = graph.begin_prepared_topology_edit();
        REQUIRE(reclaim->prepare(48000, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
        REQUIRE(reclaim->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    }
    REQUIRE(old_counts.releases == 1);
    REQUIRE(old_counts.release_thread == std::this_thread::get_id());
    REQUIRE(new_counts.releases == 0);
    graph.clear();
    graph.release();
    REQUIRE(new_counts.releases == 1);
    REQUIRE(new_counts.release_thread == std::this_thread::get_id());
}

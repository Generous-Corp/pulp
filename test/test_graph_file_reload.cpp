#include "../examples/graph-patch-consumer/hello.hpp"
#include "support/unique_temp_dir.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/graph_file_reload.hpp>
#include <pulp/host/signal_graph_control.hpp>
#include <pulp/inspect/control_signal_graph_executor.hpp>
#include <semaphore>
#include <thread>
#include <utility>

using namespace pulp::host;
namespace {
std::string patch(float value) {
    return "# flow 1\ninput : pulp/audio_in\nvoice : \"com.example.PatchGain\"\n"
           "out : pulp/audio_out\ninput.audio >> voice.audio\nvoice.audio >> out.audio\n"
           "voice.level = " +
           std::to_string(value) + "\n";
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    REQUIRE(file.good());
}
template <typename Graph> void audio_is(Graph& graph, float expected) {
    pulp::audio::Buffer<float> input(2, 64), output(2, 64);
    for (std::size_t ch = 0; ch != 2; ++ch)
        for (auto& value : input.channel(ch))
            value = 0.5f;
    auto destination = output.view();
    graph.process(destination, std::as_const(input).view(), 64);
    for (const auto sample : output.channel(0))
        REQUIRE(sample == expected);
    for (const auto sample : output.channel(1))
        REQUIRE(sample == 0.0f);
}
std::unique_ptr<SignalGraph> live(GraphPatchCatalog& catalog) {
    register_patch_gain(catalog);
    auto initial = parse_graph_patch(patch(0.25f), catalog);
    REQUIRE(initial);
    REQUIRE(initial.graph->prepare(48000, 64));
    audio_is(*initial.graph, 0.125f);
    return std::move(initial.graph);
}
} // namespace

TEST_CASE("Graph file change prepares off-side and publishes the next audio boundary",
          "[host][graph-file-reload]") {
    GraphPatchCatalog catalog;
    auto graph = live(catalog);
    pulp::test::ScopedTempDir directory("pulp-graph-file-reload");
    const auto file = directory.path / "live.pat";
    GraphFileWatch watch(file);
    write(file, patch(0.25f));
    // The first readable contents are the baseline, not a change to apply.
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
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
    watch.acknowledge();
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
    SECTION("malformed edge") {
        text = patch(0.75f) + "input.nope >> out.audio\n";
    }
    SECTION("unsupported node") {
        text = patch(0.75f) + "other : unknown/module\n";
    }
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
    const auto prepared =
        reload.prepare(patch(0.75f), GraphFileFormat::Notation, catalog, 48000, 64);
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
    write(file, patch(0.25f));
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
    write(file, "partial");
    REQUIRE(watch.poll().code == GraphFileChange::Code::Pending);
    write(file, patch(0.75f));
    REQUIRE(watch.poll().code == GraphFileChange::Code::Pending);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Changed);
    watch.acknowledge();
    write(file, std::string(1024 * 1024 + 1, 'x'));
    REQUIRE(watch.poll().code == GraphFileChange::Code::TooLarge);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
    GraphFileWatch bad_extension(directory.path / "live.cpp");
    REQUIRE(bad_extension.poll().code == GraphFileChange::Code::UnsupportedFormat);
}

TEST_CASE("Graph file watch does not reapply its starting contents",
          "[host][graph-file-reload][watch]") {
    pulp::test::ScopedTempDir directory("pulp-graph-watch-seed");
    const auto file = directory.path / "live.pat";
    write(file, patch(0.25f));
    GraphFileWatch watch(file);
    for (int poll = 0; poll != 4; ++poll)
        REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
    // Restoring the starting contents after an edit is a change again only if
    // the edit was acknowledged; returning to the baseline before that is not.
    write(file, patch(0.75f));
    REQUIRE(watch.poll().code == GraphFileChange::Code::Pending);
    write(file, patch(0.25f));
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
}

TEST_CASE("Graph file watch re-offers a change until it is acknowledged",
          "[host][graph-file-reload][watch]") {
    pulp::test::ScopedTempDir directory("pulp-graph-watch-ack");
    const auto file = directory.path / "live.pat";
    write(file, patch(0.25f));
    GraphFileWatch watch(file);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
    write(file, patch(0.75f));
    REQUIRE(watch.poll().code == GraphFileChange::Code::Pending);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Changed);
    // No terminal outcome yet: the same contents are offered again.
    REQUIRE(watch.poll().code == GraphFileChange::Code::Pending);
    const auto again = watch.poll();
    REQUIRE(again.code == GraphFileChange::Code::Changed);
    REQUIRE(again.text == patch(0.75f));
    watch.acknowledge();
    for (int poll = 0; poll != 3; ++poll)
        REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
}

TEST_CASE("Graph file watch refuses an oversize file without reading it",
          "[host][graph-file-reload][watch]") {
    pulp::test::ScopedTempDir directory("pulp-graph-watch-size");
    const auto file = directory.path / "live.pulpgraph";
    write(file, std::string(1024 * 1024, ' '));
    GraphFileWatch watch(file);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
    write(file, std::string(1024 * 1024 + 1, ' '));
    REQUIRE(watch.poll().code == GraphFileChange::Code::TooLarge);
    REQUIRE(watch.poll().code == GraphFileChange::Code::Unchanged);
}

TEST_CASE("Public graph file reload bounds both formats before staging",
          "[host][graph-file-reload][bounds]") {
    for (const auto format : {GraphFileFormat::Notation, GraphFileFormat::Pulpgraph}) {
        GraphPatchCatalog catalog;
        auto graph = live(catalog);
        const auto before = GraphSerializer::to_json(*graph);
        const auto generation = graph->prepared_stats().snapshot_generation;
        auto replacement = parse_graph_patch(patch(0.75f), catalog);
        REQUIRE(replacement);
        auto document = format == GraphFileFormat::Notation
                            ? patch(0.75f)
                            : GraphSerializer::to_json(*replacement.graph);
        constexpr std::size_t limit = 1024 * 1024;
        REQUIRE(document.size() < limit);
        document.resize(limit, '\n');
        GraphFileReloadTransaction oversized(*graph);
        const auto refused = oversized.prepare(document + " ", format, catalog, 48000, 64);
        REQUIRE_FALSE(refused);
        REQUIRE(refused.diagnostic.code == GraphPatchDiagnostic::Code::ResourceLimit);
        REQUIRE_FALSE(oversized.commit());
        REQUIRE(GraphSerializer::to_json(*graph) == before);
        REQUIRE(graph->prepared_stats().snapshot_generation == generation);
        audio_is(*graph, 0.125f);
        GraphFileReloadTransaction boundary(*graph);
        REQUIRE(boundary.prepare(document, format, catalog, 48000, 64));
        audio_is(*graph, 0.125f);
        REQUIRE(boundary.commit());
        audio_is(*graph, 0.375f);
    }
}

namespace {
struct LifecycleCounts {
    int prepares = 0;
    int releases = 0;
    std::thread::id release_thread;
    std::function<void()> on_release;
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
    void prepare(const pulp::format::PrepareContext&) override {
        ++counts_.prepares;
    }
    void release() override {
        ++counts_.releases;
        counts_.release_thread = std::this_thread::get_id();
        if (counts_.on_release)
            counts_.on_release();
    }
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
            output.channel(0)[frame] = input.channel(0)[frame] * state().get_value(7);
    }

  private:
    LifecycleCounts& counts_;
};
} // namespace

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
        const auto alias = edit->add_processor_node(instance);
        REQUIRE(alias == 0);
        SECTION("prepare refuses alias before preparing fresh or live instance") {
            REQUIRE(edit->prepare(48000, 64) ==
                    SignalGraph::PreparedTopologyEdit::Result::InvalidMutation);
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
            REQUIRE(competing->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
            REQUIRE(edit->prepare(48000, 64) ==
                    SignalGraph::PreparedTopologyEdit::Result::InvalidMutation);
        }
        SECTION("quiesced preparation cannot prepare a refused alias") {
            REQUIRE(edit->prepare_quiesced(48000, 64) ==
                    SignalGraph::PreparedTopologyEdit::Result::InvalidMutation);
            REQUIRE(fresh.prepares == 0);
            REQUIRE(baseline.prepares == 1);
            REQUIRE(baseline.releases == 0);
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
    // A remains executable after B is current. It must never receive a second
    // release-owning token, even under a different node ID or graph owner.
    const auto current_state = GraphSerializer::to_json(graph);
    {
        LifecycleCounts fresh;
        auto attempt = graph.begin_prepared_topology_edit();
        REQUIRE(attempt->add_processor_node(std::make_unique<ReloadLifecycleGain>(fresh)));
        REQUIRE(attempt->add_processor_node(old_instance) == 0);
        SECTION("retained alias live refusal") {
            REQUIRE(attempt->prepare(48000, 64) ==
                    SignalGraph::PreparedTopologyEdit::Result::InvalidMutation);
        }
        SECTION("retained alias quiesced refusal") {
            REQUIRE(attempt->prepare_quiesced(48000, 64) ==
                    SignalGraph::PreparedTopologyEdit::Result::InvalidMutation);
        }
        SECTION("retained alias abandonment") {}
        REQUIRE(fresh.prepares == 0);
        REQUIRE(old_counts.prepares == 1);
        REQUIRE(new_counts.prepares == 1);
        REQUIRE(old_counts.releases == 0);
        REQUIRE(GraphSerializer::to_json(graph) == current_state);
        audio_is(graph, 0.375f);
        audio_is(retained, 0.125f);
    }
    {
        SignalGraph another_owner;
        REQUIRE(another_owner.add_processor_node(old_instance) == 0);
        REQUIRE(old_counts.prepares == 1);
        REQUIRE(old_counts.releases == 0);
    }
    bool pinned_audio = false;
    std::thread reader([&] {
        pulp::audio::Buffer<float> input(2, 64), output(2, 64);
        for (auto& sample : input.channel(0))
            sample = 0.5f;
        auto destination = output.view();
        retained.process(destination, std::as_const(input).view(), 64);
        pinned_audio = std::all_of(output.channel(0).begin(), output.channel(0).end(),
                                   [](float value) { return value == 0.125f; });
    });
    // One graph has one audio thread: the pinned snapshot renders off the
    // test thread, but not concurrently with the live graph's own process().
    reader.join();
    audio_is(graph, 0.375f);
    REQUIRE(pinned_audio);
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
    {
        SignalGraph reused;
        const auto in = reused.add_input_node(2), out = reused.add_output_node(2);
        const auto id = reused.add_processor_node(old_instance);
        REQUIRE(id);
        REQUIRE(reused.connect(in, 0, id, 0));
        REQUIRE(reused.connect(id, 0, out, 0));
        REQUIRE(reused.prepare(48000, 64));
        REQUIRE(old_counts.prepares == 2);
        audio_is(reused, 0.125f);
    }
    REQUIRE(old_counts.releases == 2);
}

TEST_CASE("Processor lifecycle reservation spans graphs and release completion",
          "[host][graph-file-reload][ownership]") {
    LifecycleCounts counts;
    auto instance =
        pulp::format::ProcessorNodeInstance::create(std::make_unique<ReloadLifecycleGain>(counts));
    SECTION("simultaneous graph claims have exactly one winner") {
        SignalGraph graphs[2];
        NodeId ids[2]{};
        std::barrier start(3);
        std::thread first([&] {
            start.arrive_and_wait();
            ids[0] = graphs[0].add_processor_node(instance);
        });
        std::thread second([&] {
            start.arrive_and_wait();
            ids[1] = graphs[1].add_processor_node(instance);
        });
        start.arrive_and_wait();
        first.join();
        second.join();
        REQUIRE((ids[0] != 0) != (ids[1] != 0));
        REQUIRE(counts.prepares == 0);
        REQUIRE(counts.releases == 0);
    }
    SECTION("reservation remains visible while user release is blocked") {
        SignalGraph owner, other;
        REQUIRE(owner.add_processor_node(instance));
        REQUIRE(owner.prepare(48000, 64));
        std::binary_semaphore entered(0), finish(0);
        counts.on_release = [&] {
            entered.release();
            finish.acquire();
        };
        std::thread retiring([&] {
            owner.clear();
            owner.release();
        });
        const bool release_entered = entered.try_acquire_for(std::chrono::seconds(5));
        const auto during_release = other.add_processor_node(instance);
        finish.release();
        retiring.join();
        counts.on_release = {};
        REQUIRE(release_entered);
        REQUIRE(during_release == 0);
        REQUIRE(counts.prepares == 1);
        REQUIRE(counts.releases == 1);
        REQUIRE(other.add_processor_node(instance));
        REQUIRE(other.prepare(48000, 64));
        REQUIRE(counts.prepares == 2);
        other.clear();
        other.release();
        REQUIRE(counts.releases == 2);
    }
}

namespace {
void reload_custom_gain(pulp::audio::BufferView<float>& output,
                        const pulp::audio::BufferView<const float>& input, int frames) {
    for (int i = 0; i < frames; ++i)
        output.channel(0)[i] = input.channel(0)[i] * 0.25f;
}
void reload_custom_other(pulp::audio::BufferView<float>& output,
                         const pulp::audio::BufferView<const float>& input, int frames) {
    for (int i = 0; i < frames; ++i)
        output.channel(0)[i] = input.channel(0)[i] * 0.75f;
}
} // namespace
TEST_CASE("Graph reload reuses proven Custom callbacks and refuses ambiguous replacements",
          "[host][graph-file-reload][custom]") {
    CustomNodeType type;
    type.type_id = "test.reload.gain";
    type.num_input_ports = type.num_output_ports = 1;
    type.process = &reload_custom_gain;
    GraphPatchCatalog initial;
    REQUIRE(initial.register_custom(type));
    const std::string document =
        "# flow 1\nin : pulp/audio_in\nx : \"test.reload.gain\"\n"
        "out : pulp/audio_out\nin.audio >> x.audio\nx.audio >> out.audio\n";
    auto parsed = parse_graph_patch(document, initial);
    REQUIRE(parsed);
    REQUIRE(parsed.graph->prepare(48000, 64));
    const auto before = GraphSerializer::to_json(*parsed.graph);
    audio_is(*parsed.graph, 0.125f);
    SECTION("same raw registration reloads without registry replacement") {
        GraphFileReloadTransaction reload(*parsed.graph);
        REQUIRE(reload.prepare(document, GraphFileFormat::Notation, initial, 48000, 64));
        audio_is(*parsed.graph, 0.125f);
        REQUIRE(reload.commit());
        audio_is(*parsed.graph, 0.125f);
    }
    SECTION("incompatible or opaque registrations explicitly refuse") {
        SECTION("changed process") {
            type.process = &reload_custom_other;
        }
        SECTION("changed create") {
            type.create = +[]() -> void* { return nullptr; };
        }
        SECTION("changed release") {
            type.release = +[](void*) {};
        }
        SECTION("changed shape") {
            type.num_output_ports = 2;
        }
        SECTION("changed intentional delay") {
            type.intentional_delay_samples = +[](double) { return 1; };
        }
        SECTION("changed output latency") {
            type.output_port_latency_samples = +[](double, int) { return 1; };
        }
        SECTION("changed realtime declaration") {
            type.rt_contract.declared = true;
        }
        SECTION("changed realtime work callback") {
            type.rt_contract.worst_case_work =
                +[](int) { return pulp::signal::rt::RtWorkCounts{}; };
        }
        SECTION("same callable type different captures") {
            const auto callback = [](float gain) {
                return [gain](pulp::audio::BufferView<float>& out,
                              const pulp::audio::BufferView<const float>& in, int frames) {
                    for (int i = 0; i < frames; ++i)
                        out.channel(0)[i] = in.channel(0)[i] * gain;
                };
            };
            type.process = callback(0.25f);
            GraphPatchCatalog captured;
            REQUIRE(captured.register_custom(type));
            parsed = parse_graph_patch(document, captured);
            REQUIRE(parsed);
            REQUIRE(parsed.graph->prepare(48000, 64));
            type.process = callback(0.75f);
        }
        GraphPatchCatalog incompatible;
        REQUIRE(incompatible.register_custom(type));
        GraphFileReloadTransaction reload(*parsed.graph);
        const auto result =
            reload.prepare(document, GraphFileFormat::Notation, incompatible, 48000, 64);
        REQUIRE_FALSE(result);
        REQUIRE(result.diagnostic.code == GraphPatchDiagnostic::Code::UnsupportedContract);
        REQUIRE_FALSE(reload.commit());
        REQUIRE(GraphSerializer::to_json(*parsed.graph) == before);
        audio_is(*parsed.graph, 0.125f);
    }
}

TEST_CASE("Graph file executor shares typed refusal and numeric publication with public API",
          "[host][graph-file-reload][executor]") {
    GraphPatchCatalog catalog;
    auto graph = live(catalog);
    SignalGraphControlAuthority authority(*graph, 48000, 64);
    authority.set_graph_file_catalog(catalog);
    auto executor = pulp::inspect::make_control_signal_graph_executor(
        [&](const pulp::inspect::ControlAdmissionPlan&) { return &authority; });
    pulp::inspect::ControlExecutionContext context;
    context.checkpoint = [] { return pulp::inspect::ControlExecutionCheckpoint::Continue; };
    pulp::inspect::ControlAdmissionPlan plan;
    plan.receipt_id.value = "graph-file-test";
    const auto request = [&](std::string text, std::uint64_t generation) {
        auto params = choc::value::createObject("");
        params.addMember("text", text);
        params.addMember("format", "notation");
        params.addMember("expected_generation", static_cast<std::int64_t>(generation));
        pulp::inspect::ControlRequestEnvelope result;
        result.operation_id = "dev.pulp.graph/file.reload@1";
        result.operation_version = 1;
        result.params_json = choc::json::toString(params, false);
        return result;
    };
    const auto before = GraphSerializer::to_json(*graph);
    const auto invalid = patch(0.75f) + "input.nope >> out.audio\n";
    const auto api = parse_graph_patch(invalid, catalog);
    REQUIRE_FALSE(api);
    const auto refused = executor(plan, request(invalid, 0), context);
    REQUIRE(refused.terminal_state == pulp::inspect::ControlReceiptState::Failed);
    REQUIRE(refused.result.result_code == pulp::inspect::ControlResultCode::InvalidRequest);
    const auto refused_detail = choc::json::parse(refused.result.detail_json);
    const auto diagnostic = refused_detail["diagnostic"];
    REQUIRE(choc::json::toString(diagnostic, false) ==
            choc::json::toString(choc::json::parse(graph_patch_diagnostic_json(api.diagnostic)),
                                 false));
    REQUIRE(GraphSerializer::to_json(*graph) == before);
    audio_is(*graph, 0.125f);
    SECTION("cancel before commit") {
        int checkpoints = 0;
        context.checkpoint = [&] {
            return ++checkpoints == 1 ? pulp::inspect::ControlExecutionCheckpoint::Continue
                                      : pulp::inspect::ControlExecutionCheckpoint::Cancelled;
        };
        REQUIRE(executor(plan, request(patch(0.75f), 0), context).terminal_state ==
                pulp::inspect::ControlReceiptState::Failed);
        REQUIRE(authority.generation() == 0);
        REQUIRE(GraphSerializer::to_json(*graph) == before);
        audio_is(*graph, 0.125f);
    }
    SECTION("deadline and revocation before publication preserve numeric state") {
        auto refusal = pulp::inspect::ControlExecutionCheckpoint::DeadlineExceeded;
        SECTION("deadline") {}
        SECTION("revoked") {
            refusal = pulp::inspect::ControlExecutionCheckpoint::AuthorityRevoked;
        }
        int checkpoints = 0;
        context.checkpoint = [&] {
            return ++checkpoints == 1 ? pulp::inspect::ControlExecutionCheckpoint::Continue
                                      : refusal;
        };
        REQUIRE(executor(plan, request(patch(0.75f), 0), context).terminal_state ==
                pulp::inspect::ControlReceiptState::Failed);
        REQUIRE(authority.generation() == 0);
        REQUIRE(GraphSerializer::to_json(*graph) == before);
        audio_is(*graph, 0.125f);
    }
    SECTION("strict request bound") {
        REQUIRE(executor(plan, request(std::string(512 * 1024, 'x'), 0), context).terminal_state ==
                pulp::inspect::ControlReceiptState::Failed);
        REQUIRE(authority.generation() == 0);
        REQUIRE(GraphSerializer::to_json(*graph) == before);
    }
    SECTION("valid file and stale generation") {
        const auto applied = executor(plan, request(patch(0.75f), 0), context);
        INFO(applied.result.explanation);
        REQUIRE(applied.terminal_state == pulp::inspect::ControlReceiptState::Completed);
        REQUIRE(authority.generation() == 1);
        audio_is(*graph, 0.375f);
        const auto committed = GraphSerializer::to_json(*graph);
        const auto stale = executor(plan, request(patch(0.25f), 0), context);
        REQUIRE(stale.terminal_state == pulp::inspect::ControlReceiptState::Failed);
        REQUIRE(stale.result.result_code == pulp::inspect::ControlResultCode::StateConflict);
        REQUIRE(stale.result.retry == pulp::inspect::ControlRetryClassification::AfterRefresh);
        REQUIRE(choc::json::parse(stale.result.detail_json)["generation"].getInt64() == 1);
        REQUIRE(GraphSerializer::to_json(*graph) == committed);
        REQUIRE(authority.generation() == 1);
        audio_is(*graph, 0.375f);
    }
}

TEST_CASE("Graph file reload and a route edit on one authority serialize",
          "[host][graph-file-reload][concurrency]") {
    GraphPatchCatalog catalog;
    auto graph = live(catalog);
    SignalGraphControlAuthority authority(*graph, 48000, 64);
    // A reload catalog whose processor factory blocks until released, so the
    // route edit below starts while the reload is mid-preparation.
    std::atomic<bool> factory_entered{false}, factory_release{false};
    GraphPatchCatalog slow;
    {
        pulp::host::ProcessorRestoreDescriptor descriptor;
        descriptor.type = {"com.example.PatchGain", 1};
        descriptor.input_channels = 1;
        descriptor.output_channels = 1;
        descriptor.create = [&] {
            factory_entered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!factory_release.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            return pulp::format::ProcessorNodeInstance::create(std::make_unique<PatchGain>());
        };
        REQUIRE(slow.register_processor(std::move(descriptor)) ==
                pulp::host::ProcessorRestoreCode::Restored);
    }
    authority.set_graph_file_catalog(slow);
    NodeId input = 0, voice = 0;
    for (const auto& node : graph->nodes()) {
        if (node.type == NodeType::AudioInput)
            input = node.id;
        if (node.type == NodeType::Plugin)
            voice = node.id;
    }
    REQUIRE(input != 0);
    REQUIRE(voice != 0);
    SignalGraphRouteCommand insert;
    insert.kind = SignalGraphRouteCommand::Kind::Insert;
    insert.source = input;
    insert.destination = voice;
    insert.parameter_id = 7;

    GraphFileReloadResult reloaded;
    std::thread reloader([&] {
        reloaded = authority.reload_graph_file(patch(0.75f), GraphFileFormat::Notation, 0);
    });
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!factory_entered.load() && std::chrono::steady_clock::now() < entered_deadline)
        std::this_thread::yield();
    REQUIRE(factory_entered.load());
    std::atomic<bool> applied_done{false};
    SignalGraphRouteResult applied;
    std::thread editor([&] {
        applied = authority.apply({&insert, 1});
        applied_done.store(true);
    });
    // Serialized, the edit waits for the reload; give an unserialized edit
    // ample time to publish first before letting the reload continue.
    const auto edit_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (!applied_done.load() && std::chrono::steady_clock::now() < edit_deadline)
        std::this_thread::yield();
    factory_release.store(true);
    reloader.join();
    editor.join();
    INFO(graph_file_reload_code_name(reloaded.code));
    // The reload started against generation 0 and nothing else may publish
    // until it finishes, so it commits; the edit then sees the reloaded graph.
    REQUIRE(reloaded.code == GraphFileReloadCode::Committed);
    REQUIRE(applied.generation == authority.generation());
    REQUIRE(authority.generation() == (applied ? 2u : 1u));
    if (!applied)
        audio_is(*graph, 0.375f);
}

TEST_CASE("Baseline alias defenses preserve lifecycle across both preparation modes",
          "[host][graph-file-reload][alias-defense]") {
    LifecycleCounts baseline, fresh;
    SignalGraph graph;
    const auto input = graph.add_input_node(2), output = graph.add_output_node(2);
    auto instance = pulp::format::ProcessorNodeInstance::create(
        std::make_unique<ReloadLifecycleGain>(baseline));
    const auto processor = graph.add_processor_node(instance);
    REQUIRE(graph.connect(input, 0, processor, 0));
    REQUIRE(graph.connect(processor, 0, output, 0));
    REQUIRE(graph.prepare(48000, 64));
    const auto before = GraphSerializer::to_json(graph);
    {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->remove_node(processor));
        REQUIRE(edit->add_processor_node(std::make_unique<ReloadLifecycleGain>(fresh)));
        const auto alias = edit->add_processor_node(instance);
        auto result = SignalGraph::PreparedTopologyEdit::Result::NotPrepared;
        SECTION("live preparation") {
            result = edit->prepare(48000, 64);
        }
        SECTION("quiesced preparation") {
            result = edit->prepare_quiesced(48000, 64);
        }
        REQUIRE(alias == 0);
        REQUIRE(result == SignalGraph::PreparedTopologyEdit::Result::InvalidMutation);
        REQUIRE(fresh.prepares == 0);
        REQUIRE(baseline.prepares == 1);
        REQUIRE(baseline.releases == 0);
    }
    REQUIRE(baseline.prepares == 1);
    REQUIRE(baseline.releases == 0);
    REQUIRE(GraphSerializer::to_json(graph) == before);
    audio_is(graph, 0.125f);
}

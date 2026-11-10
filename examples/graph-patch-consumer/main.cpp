#include "hello.hpp"
#include <iostream>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <utility>

int main(int argc, char** argv) {
    using namespace pulp::host;
    if (argc == 3 && std::string_view(argv[1]) == "--validate") {
        const auto result = load_graph_patch_file(argv[2]);
        std::cout << graph_patch_diagnostic_json(result.diagnostic) << '\n';
        return result ? 0 : 1;
    }
    GraphPatchCatalog catalog;
    register_patch_gain(catalog);
    auto patch = load_graph_patch_file(argc > 1 ? argv[1] : "hello.pat", catalog);
    if (!patch) {
        std::cerr << patch.diagnostic.message() << '\n';
        return 3;
    }
    const auto text = format_graph_patch(*patch.graph, catalog);
    auto restored = load_graph_patch_json(GraphSerializer::to_json(*patch.graph), catalog);
    if (!text || !restored || format_graph_patch(*restored.graph, catalog).text != text.text)
        return 4;
    OfflineSignalGraphHost host(*restored.graph);
    if (!host.prepare({.block_frames = 64}))
        return 5;
    NodeId midi_node = 0;
    for (const auto& n : restored.graph->nodes())
        if (n.type == NodeType::MidiInput)
            midi_node = n.id;
    pulp::midi::MidiBuffer events;
    if (!events.add(pulp::midi::MidiEvent::note_on(0, 60, 100)) ||
        !restored.graph->inject_midi(midi_node, events))
        return 9;
    pulp::audio::Buffer<float> input(2, 257);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (float& v : input.channel(ch))
            v = 0.5f;
    const auto audio = host.render({.frame_count = 257, .input = std::as_const(input).view()});
    if (!audio.ok)
        return 6;
    for (std::size_t i = 0; i < 257; ++i)
        if (audio.audio.channel(0)[i] != (i < 64 ? 0.375f : 0.25f))
            return 7;
    for (const auto v : audio.audio.channel(1))
        if (v != 0.0f)
            return 8;
    std::cout << "PASS notation/graph/pulpgraph/notation; numeric 257 frames: first 64 = 0.375, "
                 "remainder = 0.25\n"
              << text.text;
}

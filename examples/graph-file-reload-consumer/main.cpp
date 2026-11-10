#include "hello.hpp"
#include <fstream>
#include <iostream>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/graph_file_reload.hpp>
#include <utility>
using namespace pulp::host;
std::string patch(float gain) {
    return "# flow 1\nin : pulp/audio_in\nvoice : \"com.example.PatchGain\"\nout : pulp/audio_out\n"
           "in.audio >> voice.audio\nvoice.audio >> out.audio\nvoice.level = " +
           std::to_string(gain) + "\n";
}
bool audio_is(SignalGraph& graph, float expected) {
    pulp::audio::Buffer<float> input(2, 64), output(2, 64);
    for (auto& sample : input.channel(0))
        sample = 0.5f;
    auto destination = output.view();
    graph.process(destination, std::as_const(input).view(), 64);
    for (auto sample : output.channel(0))
        if (sample != expected)
            return false;
    for (auto sample : output.channel(1))
        if (sample != 0)
            return false;
    return true;
}
int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    GraphPatchCatalog catalog;
    register_patch_gain(catalog);
    auto live = parse_graph_patch(patch(0.25f), catalog);
    if (!live || !live.graph->prepare(48000, 64) || !audio_is(*live.graph, 0.125f))
        return 2;
    GraphFileWatch watch(argv[1]);
    {
        std::ofstream file(argv[1]);
        file << patch(0.75f);
    }
    if (watch.poll().code != GraphFileChange::Code::Pending)
        return 3;
    const auto change = watch.poll();
    if (change.code != GraphFileChange::Code::Changed)
        return 4;
    GraphFileReloadTransaction reload(*live.graph);
    if (!reload.prepare(change.text, change.format, catalog, 48000, 64) ||
        !audio_is(*live.graph, 0.125f))
        return 5;
    if (!reload.commit() || !audio_is(*live.graph, 0.375f))
        return 6;
    const auto state = GraphSerializer::to_json(*live.graph);
    GraphFileReloadTransaction invalid(*live.graph);
    if (invalid.prepare("malformed", GraphFileFormat::Notation, catalog, 48000, 64) ||
        invalid.commit() || GraphSerializer::to_json(*live.graph) != state ||
        !audio_is(*live.graph, 0.375f))
        return 7;
    std::cout << "PASS real file change, staged old=0.125, published new=0.375; malformed "
                 "preserves audio/state\n";
}

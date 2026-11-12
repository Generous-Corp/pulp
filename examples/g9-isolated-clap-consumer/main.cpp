// SPDX-License-Identifier: MIT
#include <array>
#include <chrono>
#include <iostream>
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/runtime/crypto.hpp>
#include <thread>

using namespace pulp;
using namespace pulp::host;
using namespace std::chrono_literals;

// Runs only against the dedicated test-owned stateful half-gain CLAP fixture.
// All graph/worker calls below use installed public headers and libraries.
bool render(IsolatedClapResult& result, float expected_gain) {
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "input");
    const auto effect = graph.add_plugin_node(std::move(result.slot), 2, 2, "worker");
    const auto output = graph.add_output_node(2, "output");
    for (PortIndex c = 0; c < 2; ++c)
        if (!graph.connect(input, c, effect, c) || !graph.connect(effect, c, output, c))
            return false;
    if (!graph.prepare(48000, 64) || graph.latency_samples() != 64)
        return false;
    audio::Buffer<float> source(2, 64), destination(2, 64);
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t n = 0; n < 64; ++n)
            source.channel(c)[n] = static_cast<float>(1 + n + c * 64) / 256;
    auto in = static_cast<const audio::Buffer<float>&>(source).view();
    auto out = destination.view();
    std::array<float, 2 * 64 * 8> inputs{}, outputs{};
    for (int block = 0; block < 8; ++block) {
        if (block)
            std::this_thread::sleep_for(10ms);
        graph.process(out, in, 64);
        if (result.control->status().reason != IsolationReason::none)
            return false;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 64; ++n) {
                if (destination.channel(c)[n] != (block ? expected_gain * source.channel(c)[n] : 0))
                    return false;
                inputs[block * 128 + c * 64 + n] = source.channel(c)[n];
                outputs[block * 128 + c * 64 + n] = destination.channel(c)[n];
            }
    }
    std::cout << "gain=" << expected_gain << " input_sha256="
              << runtime::sha256_hex(reinterpret_cast<const uint8_t*>(inputs.data()),
                                     sizeof(inputs))
              << " output_sha256="
              << runtime::sha256_hex(reinterpret_cast<const uint8_t*>(outputs.data()),
                                     sizeof(outputs))
              << '\n';
    // Readers are quiescent here; capture state before graph release terminates proxy.
    return result.control->pause() && result.control->checkpoint();
}

int main(int argc, char** argv) {
    if (argc != 3)
        return 64;
    const auto plugin_hash = runtime::sha256_file_hex(argv[2], std::uint64_t{1} << 32);
    const auto worker_hash = runtime::sha256_file_hex(argv[1], std::uint64_t{1} << 32);
    if (!plugin_hash || !worker_hash)
        return 65;
    IsolatedClapOptions options{
        {argv[2], {"clap", "dev.pulp.g9.lifecycle.state"}}, argv[1], *plugin_hash, *worker_hash};
    auto original = load_isolated_clap(options);
    if (!original || !render(original, 0.5f))
        return 66;
    const auto old_generation = original.control->status().generation;
    if (original.control->stop() != IsolationCleanup::complete)
        return 67;
    auto recovered = recover_isolated_clap(options, *original.control);
    if (!recovered || recovered.control->status().generation == old_generation ||
        !render(recovered, 0.75f))
        return 68;
    if (recovered.control->stop() != IsolationCleanup::complete)
        return 69;
    std::cout << "G9 installed consumer: half-gain, checkpoint recovery, fresh generation, "
                 "64-sample PDC passed\n"
              << "plugin_sha256=" << *plugin_hash << "\nworker_sha256=" << *worker_hash << '\n';
    return 0;
}

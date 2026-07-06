// Hot-Reload Synth — standalone application.
//
// Runs the reloadable synth against the default audio device + any connected
// MIDI input, so you can test synth-DSP hot-reload without a DAW: start this,
// play notes on a MIDI keyboard, then edit logic_synth.cpp + run rebuild_logic.sh
// — the held notes change timbre live. (Audio plays out the default device.)
#include "hot_reload_synth_shell.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

static std::atomic<bool> should_quit{false};
static void on_signal(int) { should_quit.store(true); }

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    pulp::format::StandaloneApp app(pulp::examples::create_hot_reload_synth);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.input_channels = 0;      // instrument: no audio in
    config.output_channels = 2;
    app.set_config(config);

    if (!app.start()) {
        pulp::runtime::log_error("Failed to start standalone synth");
        return 1;
    }
    std::cout << "\nPulp Hot-Reload Synth running (play a connected MIDI keyboard).\n"
              << "Logic library: " << pulp::examples::hot_reload_synth_logic_path() << "\n"
              << "Edit examples/hot-reload-synth/logic_synth.cpp, run rebuild_logic.sh,\n"
              << "and held notes change timbre live. Ctrl+C to quit.\n" << std::endl;

    while (!should_quit.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    app.stop();
    return 0;
}

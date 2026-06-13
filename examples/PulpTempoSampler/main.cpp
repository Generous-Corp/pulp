// PulpTempoSampler standalone — runs the tempo-matching sampler as a desktop
// audio app. Optionally loads a loop from the command line; the loop is
// BPM-detected, sliced, and rendered to the host/built-in tempo on a background
// thread, then played back via MIDI input.
//
//   pulp-tempo-sampler [loop.wav]

#include "pulp_tempo_sampler.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

void load_loop_file(pulp::format::StandaloneApp& app, const char* path) {
    auto data = pulp::audio::FormatRegistry::instance().read(path);
    if (!data || data->empty()) {
        pulp::runtime::log_error("PulpTempoSampler: could not read loop file");
        return;
    }
    auto* ts = dynamic_cast<pulp::examples::PulpTempoSamplerProcessor*>(app.processor());
    if (!ts) return;
    std::vector<const float*> ch(data->num_channels());
    for (std::uint32_t c = 0; c < data->num_channels(); ++c) ch[c] = data->channels[c].data();
    ts->load_loop(ch.data(), static_cast<int>(data->num_channels()),
                  static_cast<long>(data->num_frames()), data->sample_rate);
    pulp::runtime::log_info("PulpTempoSampler: loaded loop, detecting tempo + slices");
}

} // namespace

int main(int argc, char** argv) {
    using namespace pulp;
    runtime::log_info("PulpTempoSampler Standalone v1.0.0");

    format::StandaloneApp app(examples::create_pulp_tempo_sampler);
    format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 0;
    config.persist_settings = false;
    app.set_config(config);

    if (!app.start()) {
        runtime::log_error("PulpTempoSampler: failed to start audio");
        return 1;
    }
    if (argc > 1) load_loop_file(app, argv[1]);

    std::puts("PulpTempoSampler running — send MIDI to play tempo-matched slices. Ctrl-C to quit.");
    while (app.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    app.stop();
    return 0;
}

// Tests for `pulp audio render` — the pure block stepper and the arg parser.
// Both are device-free: the stepper is exercised with a fake process callback
// (no PluginSlot, no audio device), and the parser is pure string handling.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/cmd_audio_render.hpp"
#include "../tools/cli/cmd_audio_render_step.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <cstdint>
#include <vector>

using namespace pulp;
using namespace pulp::cli;
namespace ar = pulp::cli::audio_render;

namespace {

// A passthrough callback: out[ch][i] = in[ch][i]. Time-invariant, so its output
// must be identical regardless of how the render is partitioned into blocks.
auto passthrough() {
    return [](audio::BufferView<float>& out, const audio::BufferView<const float>& in,
              const midi::MidiBuffer&, midi::MidiBuffer&,
              const state::ParameterEventQueue&, int n) {
        const auto chans = std::min(out.num_channels(), in.num_channels());
        for (std::size_t ch = 0; ch < chans; ++ch) {
            auto o = out.channel(ch);
            auto i = in.channel(ch);
            for (int s = 0; s < n; ++s) o[s] = i[s];
        }
    };
}

audio::Buffer<float> ramp(std::uint32_t channels, std::uint64_t frames) {
    audio::Buffer<float> b(channels, static_cast<std::size_t>(frames));
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        auto c = b.channel(ch);
        for (std::uint64_t n = 0; n < frames; ++n)
            c[n] = static_cast<float>(n) + static_cast<float>(ch) * 0.25f;
    }
    return b;
}

ar::StepSpec spec_for(std::uint32_t block, std::uint64_t frames) {
    ar::StepSpec s;
    s.input_channels = 2;
    s.output_channels = 2;
    s.max_block_frames = block;
    s.block_frames = block;
    s.frame_count = frames;
    return s;
}

}  // namespace

TEST_CASE("audio render stepper: block-partition invariance", "[audio-render][parity]") {
    const std::uint64_t frames = 2000;
    const auto input = ramp(2, frames);

    audio::Buffer<float> out_small, out_large;
    ar::StepStats st_small, st_large;
    ar::StepEvents none;

    REQUIRE(ar::render_blocks(spec_for(64, frames), input.view(), none, out_small,
                              st_small, passthrough()));
    REQUIRE(ar::render_blocks(spec_for(512, frames), input.view(), none, out_large,
                              st_large, passthrough()));

    REQUIRE(out_small.num_channels() == 2);
    REQUIRE(out_small.num_samples() == frames);
    REQUIRE(out_large.num_samples() == frames);

    // Bit-identical across block sizes, and equal to the input (passthrough).
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::uint64_t n = 0; n < frames; ++n) {
            REQUIRE(out_small.channel(ch)[n] == out_large.channel(ch)[n]);
            REQUIRE(out_small.channel(ch)[n] == input.channel(ch)[n]);
        }
    }
    REQUIRE(st_small.frames_rendered == frames);
    REQUIRE(st_large.frames_rendered == frames);
}

TEST_CASE("audio render stepper: event windowing, incl. exactly on a block boundary",
          "[audio-render]") {
    const std::uint32_t block = 256;
    const std::uint64_t frames = 1024;

    // Param events at frames 0, 256 (exactly on a boundary), 300; MIDI at 256.
    std::vector<ar::TimedParam> params;
    for (std::uint64_t f : {std::uint64_t{0}, std::uint64_t{256}, std::uint64_t{300}}) {
        state::ParameterEvent e;
        e.param_id = 7;
        e.value = static_cast<float>(f);
        params.push_back({f, e});
    }
    std::vector<ar::TimedMidi> midi{
        {256, midi::MidiEvent::note_on(0, 60, 100)}};

    ar::StepEvents events;
    events.params = params;
    events.midi = midi;

    // Reconstruct absolute frames inside the callback from a running block start.
    // The driver forwards this exact queue to PluginSlot::process, so the event
    // VALUE must survive windowing alongside the offset — that is what makes
    // `--param @frame` sample-accurate rather than block-quantized.
    std::vector<std::uint64_t> param_frames, midi_frames;
    std::vector<float> param_values;
    std::uint64_t running = 0;
    auto capture = [&](audio::BufferView<float>&, const audio::BufferView<const float>&,
                       const midi::MidiBuffer& mi, midi::MidiBuffer&,
                       const state::ParameterEventQueue& pq, int n) {
        for (const auto& e : pq.events()) {
            param_frames.push_back(running + static_cast<std::uint64_t>(e.sample_offset));
            param_values.push_back(e.value);
        }
        for (const auto& e : mi)
            midi_frames.push_back(running + static_cast<std::uint64_t>(e.sample_offset));
        running += static_cast<std::uint64_t>(n);
    };

    audio::Buffer<float> out;
    ar::StepStats st;
    audio::BufferView<const float> no_input;
    REQUIRE(ar::render_blocks(spec_for(block, frames), no_input, events, out, st, capture));

    // Every event lands at its requested absolute frame — the frame-256 event in
    // the block that STARTS at 256 (offset 0), not the prior block — carrying its
    // value (here value == frame), so the forwarded queue is sample-accurate.
    REQUIRE(param_frames == std::vector<std::uint64_t>{0, 256, 300});
    REQUIRE(param_values == std::vector<float>{0.0f, 256.0f, 300.0f});
    REQUIRE(midi_frames == std::vector<std::uint64_t>{256});
    REQUIRE(st.params_dispatched == 3);
    REQUIRE(st.midi_dispatched == 1);
    REQUIRE(st.params_out_of_range == 0);
    REQUIRE(st.midi_out_of_range == 0);
}

TEST_CASE("audio render stepper: out-of-range events are counted", "[audio-render]") {
    const std::uint64_t frames = 512;

    state::ParameterEvent p;
    p.param_id = 7;
    p.value = 1.0f;
    std::vector<ar::TimedParam> params{{511, p}, {512, p}, {1024, p}};
    std::vector<ar::TimedMidi> midi{
        {512, midi::MidiEvent::note_on(0, 60, 100)},
        {1024, midi::MidiEvent::note_off(0, 60)}};

    ar::StepEvents events;
    events.params = params;
    events.midi = midi;

    audio::Buffer<float> out;
    ar::StepStats st;
    audio::BufferView<const float> no_input;
    REQUIRE(ar::render_blocks(spec_for(128, frames), no_input, events, out, st,
                              passthrough()));

    REQUIRE(st.params_dispatched == 1);
    REQUIRE(st.params_out_of_range == 2);
    REQUIRE(st.midi_dispatched == 0);
    REQUIRE(st.midi_out_of_range == 2);
}

TEST_CASE("audio render stepper: short input is silence-padded", "[audio-render]") {
    const std::uint64_t input_frames = 100;
    const std::uint64_t render_frames = 300;
    const auto input = ramp(2, input_frames);

    audio::Buffer<float> out;
    ar::StepStats st;
    ar::StepEvents none;
    REQUIRE(ar::render_blocks(spec_for(128, render_frames), input.view(), none, out, st,
                              passthrough()));

    REQUIRE(out.num_samples() == render_frames);
    REQUIRE(st.input_truncated);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::uint64_t n = 0; n < input_frames; ++n)
            REQUIRE(out.channel(ch)[n] == input.channel(ch)[n]);
        for (std::uint64_t n = input_frames; n < render_frames; ++n)
            REQUIRE(out.channel(ch)[n] == 0.0f);
    }
}

TEST_CASE("audio render stepper: invalid spec / unsorted events rejected", "[audio-render]") {
    audio::Buffer<float> out;
    ar::StepStats st;
    ar::StepEvents none;
    audio::BufferView<const float> no_input;

    // block_frames > max_block_frames.
    ar::StepSpec bad;
    bad.output_channels = 2;
    bad.max_block_frames = 128;
    bad.block_frames = 256;
    bad.frame_count = 512;
    REQUIRE_FALSE(ar::render_blocks(bad, no_input, none, out, st, passthrough()));

    // Unsorted parameter events.
    std::vector<ar::TimedParam> unsorted;
    state::ParameterEvent e;
    e.param_id = 1;
    unsorted.push_back({100, e});
    unsorted.push_back({10, e});
    ar::StepEvents bad_events;
    bad_events.params = unsorted;
    REQUIRE_FALSE(ar::render_blocks(spec_for(64, 256), no_input, bad_events, out, st,
                                    passthrough()));
}

TEST_CASE("audio render parser: minimal valid request", "[audio-render]") {
    const auto r = parse_audio_render_args(
        {"--plugin", "p.clap", "--out", "o.wav", "--duration-frames", "480"});
    REQUIRE(r.ok);
    REQUIRE(r.plugin_path == "p.clap");
    REQUIRE(r.out_wav == "o.wav");
    REQUIRE(r.duration_frames == 480);
    REQUIRE(r.format == "clap");
    REQUIRE(r.sample_rate == 48000.0);
    REQUIRE(r.block == 480);  // clamped down to the duration
    REQUIRE(r.input_kind == AudioRenderInputKind::Silence);
}

TEST_CASE("audio render parser: zero input channels are instrument-only",
          "[audio-render]") {
    const auto instrument = parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "128",
         "--in-channels", "0"});
    REQUIRE(instrument.ok);
    REQUIRE(instrument.in_channels == 0);

    REQUIRE_FALSE(parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "128",
         "--in-channels", "0", "--input-signal", "sine:440"}).ok);
    REQUIRE_FALSE(parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "128",
         "--in-channels", "0", "--input", "i.wav"}).ok);
}

TEST_CASE("audio render parser: --duration-ms resolves against sample rate", "[audio-render]") {
    const auto r = parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--sample-rate", "48000", "--duration-ms", "100"});
    REQUIRE(r.ok);
    REQUIRE(r.duration_frames == 4800);
}

TEST_CASE("audio render parser: isolated-host lifecycle and analysis WAV flags",
          "[audio-render][plugin-lab]") {
    const auto r = parse_audio_render_args(
        {"--plugin", "p.component", "--format", "au", "--out", "o.wav",
         "--duration-ms", "100", "--tail-ms", "50", "--warmup-ms", "1200",
         "--initial-param", "17=0.75", "--settle-ms", "300",
         "--timeout-ms", "45000", "--wav-format", "float32"});
    REQUIRE(r.ok);
    REQUIRE(r.duration_frames == 4800);
    REQUIRE(r.tail_frames == 2400);
    REQUIRE(r.warmup_ms == 1200);
    REQUIRE(r.settle_ms == 300);
    REQUIRE(r.timeout_ms == 45000);
    REQUIRE(r.wav_format == AudioRenderWavFormat::Float32);
    REQUIRE(r.initial_params.size() == 1);
    REQUIRE(r.initial_params[0].id == 17);
    REQUIRE(r.initial_params[0].value == 0.75f);

    REQUIRE_FALSE(parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "10",
         "--initial-param", "17=0.5@4"}).ok);
    REQUIRE_FALSE(parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "10",
         "--wav-format", "pcm20"}).ok);
}

TEST_CASE("audio render parser rejects frame-count overflow",
          "[audio-render][plugin-lab]") {
    const auto result = parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-ms", "1",
         "--tail-ms", "18446744073709551615"});
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("too long") != std::string::npos);
}

TEST_CASE("audio render parser: required + mutually-exclusive flags", "[audio-render]") {
    REQUIRE_FALSE(parse_audio_render_args({"--out", "o", "--duration-frames", "10"}).ok);
    REQUIRE_FALSE(parse_audio_render_args({"--plugin", "p", "--duration-frames", "10"}).ok);
    REQUIRE_FALSE(parse_audio_render_args({"--plugin", "p", "--out", "o"}).ok);
    REQUIRE_FALSE(parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-ms", "10", "--duration-frames", "10"}).ok);
    REQUIRE_FALSE(parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "10",
         "--input", "i.wav", "--input-signal", "silence"}).ok);
    REQUIRE_FALSE(parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "10", "--bogus"}).ok);

    const auto help = parse_audio_render_args({"--help"});
    REQUIRE(help.help);
    REQUIRE(help.exit_code == 0);
}

TEST_CASE("audio render parser: --param grammar (plain domain, optional @frame)",
          "[audio-render]") {
    const auto r = parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "1000",
         "--param", "3=1.5@128", "--param", "4=-12.0"});
    REQUIRE(r.ok);
    REQUIRE(r.params.size() == 2);
    REQUIRE(r.params[0].id == 3);
    REQUIRE(r.params[0].value == 1.5f);
    REQUIRE(r.params[0].frame == 128);
    REQUIRE(r.params[1].id == 4);
    REQUIRE(r.params[1].value == -12.0f);
    REQUIRE(r.params[1].frame == 0);

    REQUIRE_FALSE(parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "10", "--param", "3"}).ok);
}

TEST_CASE("audio render parser: --midi and --input-signal grammars + = forms",
          "[audio-render]") {
    const auto r = parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "9600",
         "--midi", "note:60,100,0,4800", "--midi", "note:64,90,1000",
         "--input-signal", "sine:440,-6", "--sample-rate=44100"});
    REQUIRE(r.ok);
    REQUIRE(r.sample_rate == 44100.0);
    REQUIRE(r.input_kind == AudioRenderInputKind::Sine);
    REQUIRE(r.sine_hz == 440.0);
    REQUIRE(r.sine_dbfs == -6.0);
    REQUIRE(r.midi.size() == 2);
    REQUIRE(r.midi[0].note == 60);
    REQUIRE(r.midi[0].velocity == 100);
    REQUIRE(r.midi[0].on_frame == 0);
    REQUIRE(r.midi[0].has_off);
    REQUIRE(r.midi[0].off_frame == 4800);
    REQUIRE(r.midi[1].has_off == false);

    REQUIRE_FALSE(parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "10",
         "--input-signal", "sawtooth"}).ok);
}

TEST_CASE("audio render parser: noise and impulse stimuli", "[audio-render]") {
    // The probe stimuli. Both are seeded/positioned so a latency artifact is
    // reproducible.
    const auto noise = parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "9600",
         "--input-signal", "noise:42"});
    REQUIRE(noise.ok);
    REQUIRE(noise.input_kind == AudioRenderInputKind::Noise);
    REQUIRE(noise.noise_seed == 42);

    const auto bare = parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "9600",
         "--input-signal", "noise"});
    REQUIRE(bare.ok);
    REQUIRE(bare.input_kind == AudioRenderInputKind::Noise);

    const auto impulse = parse_audio_render_args(
        {"--plugin", "p", "--out", "o", "--duration-frames", "9600",
         "--input-signal", "impulse:64"});
    REQUIRE(impulse.ok);
    REQUIRE(impulse.input_kind == AudioRenderInputKind::Impulse);
    REQUIRE(impulse.impulse_frame == 64);
}

TEST_CASE("audio render parser: the latency probe refuses a stimulus that cannot "
          "measure a delay", "[audio-render][latency]") {
    const auto base = std::vector<std::string>{
        "--plugin", "p", "--out", "o", "--duration-frames", "9600"};
    auto with = [&](std::vector<std::string> extra) {
        auto args = base;
        args.insert(args.end(), extra.begin(), extra.end());
        return parse_audio_render_args(args);
    };

    // Silence carries no information about a delay. Fail at parse time rather
    // than emitting an inconclusive artifact the caller has to decode.
    REQUIRE_FALSE(with({"--latency-report", "l.json"}).ok);

    // A tone whose period is a whole number of samples nulls just as well one
    // period late, so delayed-null cannot pin the delay down.
    REQUIRE_FALSE(with({"--input-signal", "sine:440", "--latency-report", "l.json",
                        "--latency-policy", "delayed-null"}).ok);

    // The marker policy needs exactly one onset.
    REQUIRE_FALSE(with({"--input-signal", "noise", "--latency-report", "l.json",
                        "--latency-policy", "marker"}).ok);

    // A report path is what makes the probe produce anything.
    REQUIRE_FALSE(with({"--input-signal", "noise", "--latency-policy",
                        "delayed-null"}).ok);

    // An intrinsic offset only means something to the marker policy.
    REQUIRE_FALSE(with({"--input-signal", "noise", "--latency-report", "l.json",
                        "--latency-intrinsic", "100"}).ok);

    // Valid: noise defaults to delayed-null.
    const auto null_probe = with({"--input-signal", "noise", "--latency-report", "l.json"});
    REQUIRE(null_probe.ok);
    REQUIRE(null_probe.latency_policy == AudioRenderLatencyPolicy::DelayedNull);

    // Valid: an impulse defaults to the marker policy, and carries its intrinsic offset.
    const auto marker_probe = with({"--input-signal", "impulse:32", "--latency-report",
                                    "l.json", "--latency-intrinsic", "100",
                                    "--latency-tolerance", "2"});
    REQUIRE(marker_probe.ok);
    REQUIRE(marker_probe.latency_policy == AudioRenderLatencyPolicy::Marker);
    REQUIRE(marker_probe.impulse_frame == 32);
    REQUIRE(marker_probe.latency_intrinsic == 100);
    REQUIRE(marker_probe.latency_tolerance == 2);

    // An expected value pins the latency the plugin is SUPPOSED to have, which
    // is only meaningful once a proof is being drawn at all.
    REQUIRE_FALSE(with({"--input-signal", "noise", "--latency-expect", "512"}).ok);

    const auto pinned = with({"--input-signal", "noise", "--latency-report", "l.json",
                              "--latency-expect", "512"});
    REQUIRE(pinned.ok);
    REQUIRE(pinned.latency_expect == 512);

    // Absent by default — the pin is opt-in, and zero is a real expectation, not
    // the same thing as "not asked for".
    REQUIRE_FALSE(null_probe.latency_expect.has_value());
    const auto pinned_zero = with({"--input-signal", "noise", "--latency-report",
                                   "l.json", "--latency-expect", "0"});
    REQUIRE(pinned_zero.ok);
    REQUIRE(pinned_zero.latency_expect == 0);
}

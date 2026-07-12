// cmd_audio_render.cpp — `pulp audio render`
//
// Loads a plugin bundle through pulp::host::PluginSlot, renders it offline from
// declarative flags (input signal, parameter changes, MIDI notes), writes the
// result to a WAV, and emits the same metrics JSON as `pulp audio validate
// summarize --json`. No DAW, no audio device — the render loop is the
// device-free pure stepper in cmd_audio_render_step.hpp.

#include "cmd_audio_render.hpp"

#include "au_info_plist.hpp"
#include "cmd_audio_render_step.hpp"

#include <pulp/audio/analysis/audio_artifacts.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/analysis/latency_evidence.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::cli {
namespace {

namespace analysis = pulp::test::audio;

void print_render_usage() {
    std::cout <<
        "pulp audio render — offline scenario render of a plugin bundle\n\n"
        "Usage:\n"
        "  pulp audio render --plugin <bundle> --out <file.wav> \\\n"
        "       (--duration-ms <n> | --duration-frames <n>) [options]\n\n"
        "Loads the plugin via the host slot, renders it offline (no DAW, no audio\n"
        "device), writes a WAV, and prints/writes the same metrics JSON as\n"
        "`pulp audio validate summarize --json`.\n\n"
        "Required:\n"
        "  --plugin <bundle>            Plugin bundle path\n"
        "  --out <file.wav>             Output WAV (int16)\n"
        "  --duration-ms|-frames <n>    Render length (one is required)\n\n"
        "Plugin selection:\n"
        "  --format clap|vst3|au|auv3|lv2   (default: clap)\n"
        "  --id <unique-id>             Descriptor URI / unique-id (LV2, multi-plugin CLAP)\n\n"
        "Render setup:\n"
        "  --sample-rate <hz>           (default: 48000)\n"
        "  --block <n>                  Max block size (default: 512)\n"
        "  --in-channels <n>            (default: 2; use 0 for no input bus)\n"
        "  --out-channels <n>           (default: 2)\n\n"
        "Input signal (mutually exclusive; default silence):\n"
        "  --input <file.wav>           Use a WAV as input (used as-is at --sample-rate;\n"
        "                               no resampling — a rate mismatch shifts pitch)\n"
        "  --input-signal silence|sine:<hz>[,<dbfs>]|noise[:<seed>]|impulse[:<frame>]\n"
        "                               (sine dbfs default: -6; noise seed default: 1)\n\n"
        "Automation (repeatable):\n"
        "  --param <id>=<value>[@frame] Parameter change in the PLAIN domain (native\n"
        "                               min..max, NOT normalized); @frame is sample-accurate\n"
        "                               (block-rate on LV2 by its control-port nature)\n"
        "  --midi note:<note>,<vel>,<on>[,<off>]   Note on at <on>, optional off at <off>\n\n"
        "Output:\n"
        "  --manifest <file.json>       Write the metrics manifest to a file\n"
        "  --json                       Print the metrics manifest to stdout\n\n"
        "Latency proof (does the plugin's reported latency match the audio?):\n"
        "  --latency-report <file.json> Write a latency-evidence artifact. Exits NONZERO\n"
        "                               if the report is disproven, or if it was asked for\n"
        "                               and could not be proven.\n"
        "  --latency-policy delayed-null|marker\n"
        "                               delayed-null (default with noise): null the output\n"
        "                                 against the input delayed by D, sweeping D. Needs\n"
        "                                 the plugin in a pass-through/bypass/dry mode --\n"
        "                                 arrange it with --param.\n"
        "                               marker (default with impulse): find the one onset.\n"
        "                                 For plugins that reshape the signal.\n"
        "  --latency-tolerance <n>      Samples of drift allowed (default: 0)\n"
        "  --latency-intrinsic <n>      Delay the plugin adds that is NOT latency (leading\n"
        "                               silence in a known IR). --latency-policy marker only.\n"
        "  --latency-expect <n>         Pin the INTENDED latency. Without it the proof is\n"
        "                               self-consistency only -- a plugin whose true delay\n"
        "                               AND report both moved together still passes.\n\n"
        "  Example (a bypassed plugin must report its true delay):\n"
        "    pulp audio render --plugin My.clap --out /tmp/o.wav --duration-ms 500 \\\n"
        "        --input-signal noise --param 3=1 --latency-report /tmp/latency.json\n";
}

host::PluginFormat parse_format(std::string_view s, bool& known) {
    known = true;
    if (s == "clap" || s == "CLAP") return host::PluginFormat::CLAP;
    if (s == "vst3" || s == "VST3") return host::PluginFormat::VST3;
    if (s == "au" || s == "AU") return host::PluginFormat::AudioUnit;
    if (s == "auv3" || s == "AUv3") return host::PluginFormat::AudioUnitV3;
    if (s == "lv2" || s == "LV2") return host::PluginFormat::LV2;
    known = false;
    return host::PluginFormat::CLAP;
}

// Build the input buffer: silence, a sine tone, or a decoded WAV (used as-is at
// the render sample rate). Width is in_channels; the stepper handles length.
audio::Buffer<float> materialize_input(const ParseAudioRenderResult& req, bool& ok) {
    ok = true;
    const std::uint32_t channels = req.in_channels;
    if (channels == 0) return {};

    if (req.input_kind == AudioRenderInputKind::Wav) {
        const auto decoded = audio::read_audio_file(req.input_wav);
        if (!decoded || decoded->empty()) {
            ok = false;
            return {};
        }
        if (decoded->sample_rate != 0 &&
            static_cast<double>(decoded->sample_rate) != req.sample_rate) {
            std::fprintf(stderr,
                         "pulp audio render: warning: input '%s' is %u Hz but rendering at "
                         "%.0f Hz; used as-is without resampling — pitch/duration will shift\n",
                         req.input_wav.c_str(), decoded->sample_rate, req.sample_rate);
        }
        const auto frames = static_cast<std::size_t>(decoded->num_frames());
        audio::Buffer<float> buf(channels, frames);
        buf.clear();
        const auto copy_ch = std::min<std::size_t>(channels, decoded->channels.size());
        for (std::size_t ch = 0; ch < copy_ch; ++ch) {
            auto dst = buf.channel(ch);
            const auto& src = decoded->channels[ch];
            std::copy_n(src.begin(), std::min(src.size(), dst.size()), dst.begin());
        }
        return buf;
    }

    const auto frames = static_cast<std::size_t>(req.duration_frames);
    audio::Buffer<float> buf(channels, frames);
    buf.clear();
    if (req.input_kind == AudioRenderInputKind::Sine && frames > 0) {
        const double amp = std::pow(10.0, req.sine_dbfs / 20.0);
        const double w = 2.0 * std::numbers::pi_v<double> * req.sine_hz / req.sample_rate;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            auto dst = buf.channel(ch);
            for (std::size_t n = 0; n < frames; ++n)
                dst[n] = static_cast<float>(amp * std::sin(w * static_cast<double>(n)));
        }
    } else if (req.input_kind == AudioRenderInputKind::Noise && frames > 0) {
        // SplitMix64 -> [-0.5, 0.5). Seeded and self-contained, so the same
        // --input-signal noise:<seed> renders bit-identically on every platform
        // and a latency artifact is reproducible.
        for (std::size_t ch = 0; ch < channels; ++ch) {
            std::uint64_t state = req.noise_seed + 0x9E3779B97F4A7C15ull *
                                                       (static_cast<std::uint64_t>(ch) + 1);
            auto dst = buf.channel(ch);
            for (std::size_t n = 0; n < frames; ++n) {
                state += 0x9E3779B97F4A7C15ull;
                std::uint64_t z = state;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                z ^= z >> 31;
                // Top 24 bits -> [0,1), centered.
                const double u = static_cast<double>(z >> 40) / 16777216.0;
                dst[n] = static_cast<float>(u - 0.5);
            }
        }
    } else if (req.input_kind == AudioRenderInputKind::Impulse && frames > 0) {
        const auto pos = static_cast<std::size_t>(req.impulse_frame);
        if (pos < frames)
            for (std::size_t ch = 0; ch < channels; ++ch)
                buf.channel(ch)[pos] = 1.0f;
    }
    return buf;
}

// Map the host slot's latency query onto the analysis layer's report status.
// core/host cannot depend on the tool-tier analysis library, so the two enums
// are deliberately separate and meet here.
analysis::LatencyReportStatus to_report_status(host::PluginSlot::LatencyQuery query) {
    switch (query) {
        case host::PluginSlot::LatencyQuery::Available:
            return analysis::LatencyReportStatus::available;
        case host::PluginSlot::LatencyQuery::Unsupported:
            return analysis::LatencyReportStatus::unsupported;
        case host::PluginSlot::LatencyQuery::QueryFailed:
            return analysis::LatencyReportStatus::query_failed;
    }
    return analysis::LatencyReportStatus::unsupported;
}

}  // namespace
}  // namespace pulp::cli

int cmd_audio_render(const std::vector<std::string>& args) {
    using namespace pulp::cli;
    using namespace pulp;

    if (args.empty()) {
        print_render_usage();
        return 1;
    }

    const auto req = parse_audio_render_args(args);
    if (req.help) {
        print_render_usage();
        return 0;
    }
    if (!req.ok) {
        std::fprintf(stderr, "pulp audio render: %s\n", req.error.c_str());
        return req.exit_code;
    }

    bool format_known = false;
    const auto format = parse_format(req.format, format_known);
    if (!format_known) {
        std::fprintf(stderr, "pulp audio render: unknown --format '%s'\n",
                     req.format.c_str());
        return 2;
    }

    host::PluginInfo info;
    info.path = req.plugin_path;
    info.format = format;
    info.unique_id = req.unique_id;
    if (info.unique_id.empty() && format == host::PluginFormat::AudioUnit)
        info.unique_id = pulp::cli::au_info_plist::unique_id_from_bundle(req.plugin_path);

    auto slot = host::PluginSlot::load(info);
    if (!slot) {
        std::fprintf(stderr, "pulp audio render: failed to load '%s'\n",
                     req.plugin_path.c_str());
        std::fprintf(stderr,
                     "  The bundle may be missing, malformed, the wrong --format, or its\n"
                     "  host loader may not be compiled into this build. Try `pulp host`\n"
                     "  / `pulp scan` for a structured diagnosis.\n");
        return 1;
    }

    if (!slot->prepare(req.sample_rate, static_cast<int>(req.block))) {
        std::fprintf(stderr, "pulp audio render: prepare() failed\n");
        slot->release();
        return 2;
    }

    bool input_ok = true;
    const audio::Buffer<float> input = materialize_input(req, input_ok);
    if (!input_ok) {
        std::fprintf(stderr, "pulp audio render: failed to read input '%s'\n",
                     req.input_wav.c_str());
        slot->release();
        return 1;
    }

    // Build absolute-frame event lists, sorted by frame (the stepper requires it).
    std::vector<audio_render::TimedParam> params;
    params.reserve(req.params.size());
    for (const auto& p : req.params) {
        state::ParameterEvent e;
        e.param_id = p.id;
        e.value = p.value;  // PLAIN domain
        params.push_back({p.frame, e});
    }
    std::stable_sort(params.begin(), params.end(),
                     [](const auto& a, const auto& b) { return a.frame < b.frame; });

    std::vector<audio_render::TimedMidi> midi;
    midi.reserve(req.midi.size() * 2);
    for (const auto& m : req.midi) {
        midi.push_back({m.on_frame, midi::MidiEvent::note_on(m.channel, m.note, m.velocity)});
        if (m.has_off)
            midi.push_back({m.off_frame, midi::MidiEvent::note_off(m.channel, m.note)});
    }
    std::stable_sort(midi.begin(), midi.end(),
                     [](const auto& a, const auto& b) { return a.frame < b.frame; });

    audio_render::StepSpec spec;
    spec.input_channels = req.in_channels;
    spec.output_channels = req.out_channels;
    spec.max_block_frames = req.block;
    spec.frame_count = req.duration_frames;
    spec.block_frames = req.block;

    audio_render::StepEvents events;
    events.midi = midi;
    events.params = params;

    audio::Buffer<float> output;
    audio_render::StepStats stats;

    // Latency facts, gathered WHILE the slot is alive. A hosted slot exposes no
    // latency-changed flag, so the only way to notice a moving report is to read
    // it at every block boundary.
    const auto latency_query = slot->latency_query();
    const bool latency_readable =
        latency_query == host::PluginSlot::LatencyQuery::Available;
    const int initial_latency = latency_readable ? slot->latency_samples() : 0;
    int final_latency = initial_latency;
    bool latency_changed = false;

    const auto process = [&](audio::BufferView<float>& out,
                             const audio::BufferView<const float>& in,
                             const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                             const state::ParameterEventQueue& pq, int n) {
        // Parameters are delivered SAMPLE-ACCURATELY: the stepper has already
        // windowed this block's events with per-block sample offsets, so we
        // forward the queue straight to process(). Every loader consumes it at
        // the event offset — CLAP (clap_event_param_value at header.time), VST3
        // (IParameterChanges addPoint at the sample offset), AU
        // (AudioUnitScheduleParameters bufferOffset); LV2 applies the value
        // block-rate, which is LV2's control-port contract, not a loss here.
        // We do NOT also call set_parameter: that would double-deliver each
        // change (once at offset 0, once at its real offset) and smear the
        // automation the queue exists to make precise.
        slot->process(out, in, midi_in, midi_out, pq, n);

        if (latency_readable) {
            const int now = slot->latency_samples();
            if (now != final_latency) latency_changed = true;
            final_latency = now;
        }
    };

    if (!audio_render::render_blocks(spec, input.view(), events, output, stats, process)) {
        std::fprintf(stderr, "pulp audio render: render failed (invalid spec)\n");
        slot->release();
        return 2;
    }
    slot->release();

    if (stats.params_dropped > 0) {
        std::fprintf(stderr,
                     "pulp audio render: warning: %u parameter event(s) dropped "
                     "(per-block queue capacity exceeded)\n",
                     stats.params_dropped);
    }
    if (stats.params_out_of_range > 0) {
        std::fprintf(stderr,
                     "pulp audio render: warning: %u parameter event(s) scheduled at or "
                     "beyond the render duration were ignored\n",
                     stats.params_out_of_range);
    }
    if (stats.midi_out_of_range > 0) {
        std::fprintf(stderr,
                     "pulp audio render: warning: %u MIDI event(s) scheduled at or beyond "
                     "the render duration were ignored\n",
                     stats.midi_out_of_range);
    }

    // Write the WAV (int16).
    audio::AudioFileData data;
    data.sample_rate = static_cast<std::uint32_t>(std::lround(req.sample_rate));
    data.channels.resize(output.num_channels());
    for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
        const auto src = output.channel(ch);
        data.channels[ch].assign(src.begin(), src.end());
    }
    if (!audio::write_wav_file(req.out_wav, data)) {
        std::fprintf(stderr, "pulp audio render: failed to write '%s'\n",
                     req.out_wav.c_str());
        return 1;
    }

    // Metrics manifest (reuses the validate analysis path). NOTE: metrics are
    // computed from the float render; the WAV is int16, so a re-analysis of the
    // file matches except below the ~-96 dBFS int16 floor — and at clipping,
    // where the float peak exceeds the hard-clamped file. Surface the latter.
    const auto metrics = analysis::analyze(output, req.sample_rate);
    const auto manifest = analysis::metrics_to_json(metrics, req.out_wav);

    if (metrics.max_peak() > 1.0) {
        std::fprintf(stderr,
                     "pulp audio render: warning: float peak %.3f exceeds 0 dBFS; the int16 "
                     "WAV is hard-clipped to +/-1.0 and will not match the manifest peak\n",
                     metrics.max_peak());
    }

    if (!req.manifest_path.empty()) {
        std::ofstream out(req.manifest_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "pulp audio render: failed to write manifest '%s'\n",
                         req.manifest_path.c_str());
            return 1;
        }
        out << manifest << "\n";
    }

    if (req.json) {
        std::cout << manifest << "\n";
    } else {
        analysis::FrequencyEstimate freq;
        if (output.num_channels() > 0 && output.num_samples() > 0)
            freq = analysis::estimate_frequency(output.channel(0), req.sample_rate);
        std::cout << "Rendered: " << req.out_wav << "\n";
        std::cout << "  frames: " << stats.frames_rendered
                  << "  blocks: " << stats.blocks_rendered << "\n";
        std::cout << analysis::summarize(metrics, freq) << "\n";
    }

    // ── Latency proof ───────────────────────────────────────────────────────
    // A separate, versioned artifact. The metrics manifest and --json stdout
    // above keep their exact shape, so nothing that parses them today breaks.
    if (!req.latency_report_path.empty()) {
        analysis::LatencyEvidence evidence;

        // The report is a FACT about the plugin. An unsupported query must not
        // become a confident zero — that is precisely how an unverifiable plugin
        // would get certified.
        std::optional<int> reported;
        if (latency_readable) reported = initial_latency;

        if (req.latency_policy == AudioRenderLatencyPolicy::Marker) {
            analysis::MarkerOffsetOptions options;
            options.input_marker_frame = static_cast<std::int64_t>(req.impulse_frame);
            options.intrinsic_response_offset = req.latency_intrinsic;
            options.tolerance_samples = req.latency_tolerance;
            evidence = analysis::measure_marker_offset(input, output, reported, options);
        } else {
            analysis::DelayedNullOptions options;
            options.tolerance_samples = req.latency_tolerance;
            evidence = analysis::measure_delayed_passthrough(input, output, reported,
                                                             options);
        }

        evidence.report_status = to_report_status(latency_query);
        evidence.reported_samples = reported;
        if (latency_readable) evidence.final_reported_samples = final_latency;
        evidence.observation_mode = analysis::LatencyObservationMode::per_block_poll;
        evidence.report_observation =
            !latency_readable  ? analysis::LatencyReportObservation::unobservable
            : latency_changed  ? analysis::LatencyReportObservation::changed
                               : analysis::LatencyReportObservation::stable;
        analysis::apply_report_observation(evidence);

        // Pin the intended VALUE too, when the caller declared one. Without this
        // the proof is self-consistency only: a plugin whose true delay and whose
        // report both moved together still passes.
        if (req.latency_expect)
            analysis::apply_expected_samples(evidence, *req.latency_expect,
                                             req.latency_tolerance);

        std::ofstream report(req.latency_report_path, std::ios::binary | std::ios::trunc);
        if (!report) {
            std::fprintf(stderr, "pulp audio render: failed to write latency report '%s'\n",
                         req.latency_report_path.c_str());
            return 1;
        }
        report << analysis::latency_evidence_to_json(evidence) << "\n";

        std::fprintf(stderr, "pulp audio render: %s\n",
                     analysis::latency_evidence_summary(evidence).c_str());

        // The artifact exists either way. Its EXISTENCE is not the result —
        // a disproven or unprovable claim must fail, or a caller that only
        // checks "did the file appear" would read a violation as a pass.
        if (evidence.gates_failure()) return 1;
    }

    return 0;
}

// Offline AU instrument probe.
//
// Scans installed Audio Units, loads a named instrument through PluginSlot,
// prepares it at a fixed sample rate, sweeps MIDI notes to discover which
// ones produce audio, and renders the loudest note to a WAV.
//
// Rendering is entirely offline: AudioUnitRender is driven from this thread
// and no output device is ever opened.
//
// Usage:
//   au_instrument_probe --name "TR-808" [--out /tmp/tr808.wav]
//                       [--note N] [--sweep-low 24] [--sweep-high 96]
//                       [--seconds 2.0] [--list-params] [--set-param ID=VAL]
//                       [--hits MS:VEL,MS:VEL,...] [--allow-silent]

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;

struct RenderResult {
    float peak = 0.0f;
    float rms = 0.0f;
    std::vector<std::vector<float>> channels;
};

const char* format_name(pulp::host::PluginFormat f) {
    switch (f) {
        case pulp::host::PluginFormat::VST3: return "VST3";
        case pulp::host::PluginFormat::AudioUnit: return "AU";
        case pulp::host::PluginFormat::AudioUnitV3: return "AUv3";
        case pulp::host::PluginFormat::CLAP: return "CLAP";
        case pulp::host::PluginFormat::LV2: return "LV2";
    }
    return "?";
}

// One scheduled strike: when to fire, and how hard.
struct Hit {
    double at_seconds = 0.0;
    int velocity = 100;
};

// Render a sequence of strikes into one continuous pass, so a hit that lands
// while a previous one is still ringing meets the instrument's live state.
// That state interaction is the whole point: a voice that resets per trigger
// renders `pair` exactly equal to `solo1 + solo2`, and a resonator that is
// re-excited rather than restarted does not. Percussion voices are one-shot,
// so no note-offs are sent.
RenderResult render_hits(pulp::host::PluginSlot& slot, int note,
                         const std::vector<Hit>& hits, double seconds) {
    const int total = static_cast<int>(kSampleRate * seconds);
    const int channels = 2;

    RenderResult r;
    r.channels.assign(channels, std::vector<float>(total, 0.0f));

    std::vector<std::vector<float>> block(channels, std::vector<float>(kBlockSize, 0.0f));
    std::vector<float*> block_ptrs(channels);
    for (int c = 0; c < channels; ++c) block_ptrs[c] = block[c].data();

    pulp::audio::BufferView<const float> input;
    pulp::host::ParameterEventQueue params;
    pulp::midi::MidiBuffer midi_out;

    double sum_sq = 0.0;
    for (int pos = 0; pos < total; pos += kBlockSize) {
        const int n = std::min(kBlockSize, total - pos);
        for (int c = 0; c < channels; ++c)
            std::fill(block[c].begin(), block[c].end(), 0.0f);

        pulp::midi::MidiBuffer midi_in;
        for (const auto& h : hits) {
            const int frame = static_cast<int>(h.at_seconds * kSampleRate);
            if (frame >= pos && frame < pos + n) {
                auto on = pulp::midi::MidiEvent::note_on(
                    0, static_cast<uint8_t>(note), static_cast<uint8_t>(h.velocity));
                on.sample_offset = frame - pos;
                midi_in.add(on);
            }
        }

        pulp::audio::BufferView<float> out(block_ptrs.data(), channels, n);
        slot.process(out, input, midi_in, midi_out, params, n);

        for (int c = 0; c < channels; ++c) {
            for (int i = 0; i < n; ++i) {
                const float s = block[c][i];
                r.channels[c][pos + i] = s;
                r.peak = std::max(r.peak, std::fabs(s));
                sum_sq += static_cast<double>(s) * s;
            }
        }
    }
    const double count = static_cast<double>(total) * channels;
    r.rms = count > 0.0 ? static_cast<float>(std::sqrt(sum_sq / count)) : 0.0f;
    return r;
}

// Render `note` for `seconds`, holding it for `hold_seconds` then releasing.
// A note that is never triggered still renders silence, which is the signal
// the sweep uses to reject it.
RenderResult render_note(pulp::host::PluginSlot& slot, int note, int velocity,
                         double seconds, double hold_seconds, bool keep_audio) {
    const int total = static_cast<int>(kSampleRate * seconds);
    const int hold = static_cast<int>(kSampleRate * hold_seconds);
    const int channels = 2;

    RenderResult r;
    r.channels.assign(channels, std::vector<float>(keep_audio ? total : 0, 0.0f));

    std::vector<std::vector<float>> block(channels, std::vector<float>(kBlockSize, 0.0f));
    std::vector<float*> block_ptrs(channels);
    for (int c = 0; c < channels; ++c) block_ptrs[c] = block[c].data();

    // Instruments have no input bus; hand the slot an empty input view.
    pulp::audio::BufferView<const float> input;
    pulp::host::ParameterEventQueue params;
    pulp::midi::MidiBuffer midi_out;

    double sum_sq = 0.0;
    for (int pos = 0; pos < total; pos += kBlockSize) {
        const int n = std::min(kBlockSize, total - pos);
        for (int c = 0; c < channels; ++c)
            std::fill(block[c].begin(), block[c].end(), 0.0f);

        pulp::midi::MidiBuffer midi_in;
        if (pos == 0) {
            midi_in.add(pulp::midi::MidiEvent::note_on(
                0, static_cast<uint8_t>(note), static_cast<uint8_t>(velocity)));
        }
        if (hold >= pos && hold < pos + n) {
            auto off = pulp::midi::MidiEvent::note_off(0, static_cast<uint8_t>(note), 0);
            off.sample_offset = hold - pos;
            midi_in.add(off);
        }

        pulp::audio::BufferView<float> out(block_ptrs.data(), channels, n);
        slot.process(out, input, midi_in, midi_out, params, n);

        for (int c = 0; c < channels; ++c) {
            for (int i = 0; i < n; ++i) {
                const float s = block[c][i];
                r.peak = std::max(r.peak, std::fabs(s));
                sum_sq += static_cast<double>(s) * s;
                if (keep_audio) r.channels[c][pos + i] = s;
            }
        }
    }
    const double count = static_cast<double>(total) * channels;
    r.rms = count > 0.0 ? static_cast<float>(std::sqrt(sum_sq / count)) : 0.0f;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::string want_name = "TR-808";
    std::string out_path = "/tmp/au_probe.wav";
    int forced_note = -1;
    int sweep_low = 24, sweep_high = 96;
    double seconds = 2.0;
    bool list_params = false;
    bool allow_silent = false;
    std::vector<std::pair<uint32_t, float>> param_sets;
    std::vector<Hit> hits;

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string(argv[i]);
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--name") want_name = next();
        else if (arg == "--out") out_path = next();
        else if (arg == "--note") forced_note = std::atoi(next().c_str());
        else if (arg == "--sweep-low") sweep_low = std::atoi(next().c_str());
        else if (arg == "--sweep-high") sweep_high = std::atoi(next().c_str());
        else if (arg == "--seconds") seconds = std::atof(next().c_str());
        else if (arg == "--hits") {
            // MS:VEL[,MS:VEL...] — strike times in milliseconds from the start
            // of the render, each with its own velocity.
            auto spec = next();
            std::size_t start = 0;
            while (start <= spec.size()) {
                const auto comma = spec.find(',', start);
                const auto item = spec.substr(start, comma == std::string::npos
                                                        ? std::string::npos
                                                        : comma - start);
                if (!item.empty()) {
                    const auto colon = item.find(':');
                    if (colon == std::string::npos) {
                        std::printf("FAIL: --hits expects MS:VEL, got '%s'\n", item.c_str());
                        return 1;
                    }
                    Hit h;
                    h.at_seconds = std::atof(item.substr(0, colon).c_str()) / 1000.0;
                    h.velocity = std::atoi(item.substr(colon + 1).c_str());
                    hits.push_back(h);
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        else if (arg == "--list-params") list_params = true;
        else if (arg == "--allow-silent") allow_silent = true;
        else if (arg == "--set-param") {
            // ID=VALUE, in the parameter's plain (not normalized) domain.
            auto spec = next();
            auto eq = spec.find('=');
            if (eq == std::string::npos) {
                std::printf("FAIL: --set-param expects ID=VALUE, got '%s'\n", spec.c_str());
                return 1;
            }
            param_sets.emplace_back(
                static_cast<uint32_t>(std::strtoul(spec.substr(0, eq).c_str(), nullptr, 10)),
                static_cast<float>(std::atof(spec.substr(eq + 1).c_str())));
        }
    }

    pulp::host::PluginScanner scanner;
    pulp::host::ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_clap = false;
    opts.scan_lv2 = false;
    auto plugins = scanner.scan(opts);
    std::printf("scan: %zu AU components found\n", plugins.size());

    const pulp::host::PluginInfo* match = nullptr;
    for (const auto& p : plugins) {
        if (p.name.find(want_name) != std::string::npos && p.is_instrument) {
            match = &p;
            break;
        }
    }
    if (!match) {
        std::printf("FAIL: no instrument AU matching '%s'\n", want_name.c_str());
        return 1;
    }

    std::printf("found: '%s' by '%s' | id=%s | format=%s | version=%s\n"
                "       is_instrument=%d in=%d out=%d\n",
                match->name.c_str(), match->manufacturer.c_str(),
                match->unique_id.c_str(), format_name(match->format),
                match->version.c_str(), match->is_instrument ? 1 : 0,
                match->num_inputs, match->num_outputs);

    auto slot = pulp::host::PluginSlot::load(*match);
    if (!slot || !slot->is_loaded()) {
        std::printf("FAIL: PluginSlot::load returned %s\n", slot ? "unloaded slot" : "nullptr");
        return 2;
    }
    std::printf("load: OK\n");

    if (!slot->prepare(kSampleRate, kBlockSize)) {
        std::printf("FAIL: prepare(%.0f, %d) returned false\n", kSampleRate, kBlockSize);
        return 3;
    }
    std::printf("prepare: OK @ %.0f Hz / %d frames\n", kSampleRate, kBlockSize);
    std::printf("latency=%d samples  tail=%d samples\n",
                slot->latency_samples(), slot->tail_samples());

    for (const auto& [id, value] : param_sets) {
        slot->set_parameter(id, value);
        std::printf("set-param: [%u] := %.3f (readback %.3f)\n",
                    id, value, slot->get_parameter(id));
    }

    auto params = slot->parameters();
    std::printf("params: %zu exposed\n", params.size());
    if (list_params) {
        for (size_t i = 0; i < params.size(); ++i) {
            const auto& p = params[i];
            std::printf("  [%3u] %-40s min=%-10.3f max=%-10.3f def=%-10.3f cur=%.3f\n",
                        p.id, p.name.c_str(), p.min_value, p.max_value,
                        p.default_value, slot->get_parameter(p.id));
        }
    }

    int best_note = forced_note;
    if (forced_note < 0) {
        std::printf("\nsweeping MIDI notes %d..%d (0.6s each, offline)\n", sweep_low, sweep_high);
        float best_peak = 0.0f;
        for (int n = sweep_low; n <= sweep_high; ++n) {
            auto r = render_note(*slot, n, 100, 0.6, 0.3, false);
            if (r.peak > 1e-5f) {
                std::printf("  note %3d: peak=%.5f rms=%.6f\n", n, r.peak, r.rms);
            }
            if (r.peak > best_peak) { best_peak = r.peak; best_note = n; }
        }
        if (best_peak <= 1e-5f) {
            std::printf("RESULT: plugin loaded and rendered, but ALL notes silent\n");
            return 4;
        }
        std::printf("loudest note: %d (peak=%.5f)\n", best_note, best_peak);
    }

    auto final_r = hits.empty()
                       ? render_note(*slot, best_note, 100, seconds, seconds * 0.5, true)
                       : render_hits(*slot, best_note, hits, seconds);
    if (!hits.empty()) {
        std::printf("\nhit sequence (%zu strikes):", hits.size());
        for (const auto& h : hits)
            std::printf(" %.1fms@v%d", h.at_seconds * 1000.0, h.velocity);
        std::printf("\n");
    }
    std::printf("\nrender note %d for %.2fs: peak=%.6f rms=%.6f (%.2f dBFS peak)\n",
                best_note, seconds, final_r.peak, final_r.rms,
                final_r.peak > 0 ? 20.0 * std::log10(final_r.peak) : -999.0);

    if (final_r.peak <= 1e-6f && !allow_silent) {
        std::printf("RESULT: SILENT render\n");
        return 5;
    }

    pulp::audio::AudioFileData data;
    data.sample_rate = static_cast<uint32_t>(kSampleRate);
    data.channels = final_r.channels;
    if (!pulp::audio::write_wav_file(out_path, data, pulp::audio::WavBitDepth::Float32)) {
        std::printf("FAIL: write_wav_file('%s')\n", out_path.c_str());
        return 6;
    }
    std::printf("wrote: %s (%llu frames, float32)\n", out_path.c_str(),
                (unsigned long long)data.num_frames());
    std::printf("RESULT: %s\n",
                final_r.peak <= 1e-6f ? "SILENT (allowed)" : "WORKING");
    return 0;
}

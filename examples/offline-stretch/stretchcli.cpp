// stretchcli — development harness for pulp::signal::OfflineStretch.
//
// The only "app" for the offline engine: file in -> render -> file out, plus
// --analyze for BPM/onset inspection. The engine now renders through the live
// OfflineStretch paths; material-adaptive windowing is automatic unless --fft /
// --hop override the analysis geometry.
//
//   stretchcli in.wav out.wav [--ratio R] [--pitch S] [--formant MODE]
//              [--formant-semitones X] [--repitch] [--quality Q]
//              [--max-ratio M] [--max-pitch P] [--fft N] [--hop N]
//              [--character clean|varispeed|phase_vocoder|granular]
//              [--transient-sens X] [--stn|--no-stn]
//              [--preset FILE] [--save-preset FILE]
//   stretchcli in.wav --analyze            # JSON: sr, frames, bpm, onsets
//   stretchcli in.wav out.wav --bpm-to T   # ratio chosen from detected BPM

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/built_in_key_tempo_analyzer.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/signal/offline_stretch.hpp>
#include <pulp/signal/stretch_preset.hpp>
#include <fstream>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using pulp::audio::AudioFileData;
using pulp::audio::BufferView;
using pulp::audio::FormatRegistry;

void usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  stretchcli IN OUT [--ratio R] [--pitch S] [--formant follow|preserve|independent]\n"
        "                    [--formant-semitones X] [--repitch] [--quality 0..2]\n"
        "                    [--max-ratio M] [--max-pitch P] [--fft N] [--hop N]\n"
        "                    [--character clean|varispeed|phase_vocoder|granular]\n"
        "                    [--transient-sens X] [--stn|--no-stn]\n"
        "                    [--preset FILE] [--save-preset FILE]\n"
        "  stretchcli IN --analyze            # JSON BPM + onsets\n"
        "  stretchcli IN OUT --bpm-to TARGET  # ratio = source_bpm / TARGET\n");
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Lossless 32-bit IEEE-float WAV writer. The CHOC-backed FormatRegistry writer
// defaults to 16-bit PCM, which quantizes the output and would mask the null
// test and the quality metrics. The dev harness must round-trip float exactly,
// so .wav output goes through here; other extensions fall back to the registry.
bool write_float32_wav(const std::string& path, const AudioFileData& d) {
    const std::uint32_t nch = d.num_channels();
    const std::uint64_t nframes = d.num_frames();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(nframes * nch * 4);

    std::fwrite("RIFF", 1, 4, f); u32(36 + data_bytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(3 /*IEEE float*/); u16(static_cast<std::uint16_t>(nch));
    u32(d.sample_rate); u32(d.sample_rate * nch * 4); u16(static_cast<std::uint16_t>(nch * 4)); u16(32);
    std::fwrite("data", 1, 4, f); u32(data_bytes);
    for (std::uint64_t i = 0; i < nframes; ++i)
        for (std::uint32_t c = 0; c < nch; ++c) {
            float v = d.channels[c][static_cast<std::size_t>(i)];
            std::fwrite(&v, 4, 1, f);
        }
    return std::fclose(f) == 0;
}

bool write_output(const std::string& path, const AudioFileData& d) {
    if (ends_with(path, ".wav") || ends_with(path, ".WAV"))
        return write_float32_wav(path, d);
    std::fprintf(stderr, "[stretchcli] note: '%s' is not .wav; writing via FormatRegistry "
                         "(may be 16-bit/lossy). Use a .wav target for lossless float.\n",
                 path.c_str());
    return FormatRegistry::instance().write(path, d);
}

std::vector<const float*> channel_ptrs(const AudioFileData& d) {
    std::vector<const float*> p(d.num_channels());
    for (std::uint32_t c = 0; c < d.num_channels(); ++c) p[c] = d.channels[c].data();
    return p;
}

int analyze(const std::string& in) {
    auto data = FormatRegistry::instance().read(in);
    if (!data || data->empty()) {
        std::fprintf(stderr, "error: cannot read '%s'\n", in.c_str());
        return 2;
    }
    const auto nframes = data->num_frames();
    const auto nch = data->num_channels();
    auto ptrs = channel_ptrs(*data);
    BufferView<const float> view(ptrs.data(), nch, static_cast<std::size_t>(nframes));

    pulp::audio::BuiltInKeyTempoAnalyzer kt;
    pulp::audio::KeyTempoAnalysisConfig kc;
    kc.source_sample_rate = static_cast<double>(data->sample_rate);
    kc.channels = nch;
    const auto kr = kt.analyze(view, kc);

    pulp::audio::OnsetDetector od;
    const auto onsets = od.detect(view);

    std::printf("{\n");
    std::printf("  \"file\": \"%s\",\n", in.c_str());
    std::printf("  \"sample_rate\": %u,\n", data->sample_rate);
    std::printf("  \"channels\": %u,\n", nch);
    std::printf("  \"frames\": %llu,\n", static_cast<unsigned long long>(nframes));
    std::printf("  \"bpm\": %.4f,\n", kr.tempo_bpm);
    std::printf("  \"bpm_confidence\": %.4f,\n", kr.tempo_confidence);
    std::printf("  \"onset_frames\": [");
    for (std::size_t i = 0; i < onsets.markers.size(); ++i)
        std::printf("%s%llu", i ? ", " : "",
                    static_cast<unsigned long long>(onsets.markers[i].frame));
    std::printf("]\n}\n");
    return 0;
}

pulp::signal::StretchCharacter parse_character(const char* s, bool* ok) {
    *ok = true;
    if (std::strcmp(s, "clean") == 0) return pulp::signal::StretchCharacter::clean;
    if (std::strcmp(s, "varispeed") == 0) return pulp::signal::StretchCharacter::varispeed;
    if (std::strcmp(s, "phase_vocoder") == 0) return pulp::signal::StretchCharacter::phase_vocoder;
    if (std::strcmp(s, "granular") == 0) return pulp::signal::StretchCharacter::granular;
    *ok = false;
    return pulp::signal::StretchCharacter::clean;
}

pulp::signal::OfflineFormantMode parse_formant(const char* s, bool* ok) {
    *ok = true;
    if (std::strcmp(s, "follow") == 0) return pulp::signal::OfflineFormantMode::follow_pitch;
    if (std::strcmp(s, "preserve") == 0) return pulp::signal::OfflineFormantMode::preserve_original;
    if (std::strcmp(s, "independent") == 0) return pulp::signal::OfflineFormantMode::shift_independently;
    *ok = false;
    return pulp::signal::OfflineFormantMode::preserve_original;
}

int render(const std::string& in, const std::string& out,
           pulp::signal::OfflineStretchOptions opts, std::optional<double> bpm_to) {
    auto data = FormatRegistry::instance().read(in);
    if (!data || data->empty()) {
        std::fprintf(stderr, "error: cannot read '%s'\n", in.c_str());
        return 2;
    }
    const auto in_frames = static_cast<long>(data->num_frames());
    const auto nch = data->num_channels();

    if (bpm_to) {
        auto ptrs = channel_ptrs(*data);
        BufferView<const float> view(ptrs.data(), nch, static_cast<std::size_t>(in_frames));
        pulp::audio::BuiltInKeyTempoAnalyzer kt;
        pulp::audio::KeyTempoAnalysisConfig kc;
        kc.source_sample_rate = static_cast<double>(data->sample_rate);
        kc.channels = nch;
        const auto kr = kt.analyze(view, kc);
        if (!(kr.tempo_bpm > 0.0)) {
            std::fprintf(stderr, "error: could not detect source BPM for --bpm-to\n");
            return 2;
        }
        opts.time_ratio = kr.tempo_bpm / *bpm_to; // slower target => longer => ratio > 1
        std::fprintf(stderr, "[stretchcli] detected %.2f BPM, target %.2f -> ratio %.4f\n",
                     kr.tempo_bpm, *bpm_to, opts.time_ratio);
    }

    // Size the engine to the user-declared bounds (default [0.25x,4x] / +/-24 st).
    // These are a HARD cap: a --ratio/--pitch beyond them is REJECTED, not
    // silently widened. Raise --max-ratio / --max-pitch for extremes.
    pulp::signal::OfflineStretchOptions sizing;
    sizing.max_time_ratio = opts.max_time_ratio;
    sizing.max_pitch_semitones = opts.max_pitch_semitones;
    sizing.route_noise_stn = opts.route_noise_stn; // honor --no-stn at prepare()

    // Material-adaptive STFT window/overlap: analyze the actual audio and pick a
    // window suited to it (large+high-overlap for bass so low partials resolve;
    // small for percussion so attacks stay sharp). Honor an explicit --fft/--hop
    // override (0 = auto). Carried through opts too so the spectral paths size
    // identically.
    {
        std::vector<const float*> ap(nch);
        for (std::uint32_t c = 0; c < nch; ++c) ap[c] = data->channels[c].data();
        const auto w = pulp::signal::OfflineStretch::recommend_window(
            ap.data(), in_frames, static_cast<int>(nch),
            static_cast<double>(data->sample_rate));
        sizing.fft_size = opts.fft_size > 0 ? opts.fft_size : w.fft_size;
        sizing.analysis_hop = opts.fft_size > 0 ? opts.analysis_hop : w.analysis_hop;
        // Transient sensitivity stays at the engine default unless explicitly set
        // (--transient-sens) — a fine-tune knob, not auto-applied, so the percussive
        // output keeps matching the validated drum_pl reference.
        sizing.transient_sensitivity = opts.transient_sensitivity;
        std::fprintf(stderr, "[stretchcli] adaptive window: fft=%d hop=%d%s\n",
                     sizing.fft_size ? sizing.fft_size : 4096,
                     sizing.analysis_hop ? sizing.analysis_hop : 512,
                     opts.fft_size > 0 ? " (manual)" : " (auto)");
    }

    pulp::signal::OfflineStretch eng;
    eng.prepare(static_cast<double>(data->sample_rate), static_cast<int>(nch), sizing);

    const long out_frames = pulp::signal::offline_stretch_output_frames(in_frames, opts.time_ratio);

    AudioFileData result;
    result.sample_rate = data->sample_rate;
    result.channels.assign(nch, std::vector<float>(static_cast<std::size_t>(out_frames), 0.0f));

    std::vector<const float*> in_ptrs(nch);
    std::vector<float*> out_ptrs(nch);
    for (std::uint32_t c = 0; c < nch; ++c) {
        in_ptrs[c] = data->channels[c].data();
        out_ptrs[c] = result.channels[c].data();
    }

    std::string err;
    if (!eng.process(in_ptrs.data(), in_frames, out_ptrs.data(), out_frames, opts, &err)) {
        std::fprintf(stderr, "error: render failed: %s\n", err.c_str());
        return 1;
    }
    if (!write_output(out, result)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", out.c_str());
        return 2;
    }
    std::fprintf(stderr, "[stretchcli] %s (%ld fr) -> %s (%ld fr), ratio %.4f pitch %.2f\n",
                 in.c_str(), in_frames, out.c_str(), out_frames,
                 opts.time_ratio, opts.pitch_semitones);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    pulp::signal::OfflineStretchOptions opts;
    bool want_analyze = false;
    std::optional<double> bpm_to;
    std::string save_preset_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: %s needs a value\n", name); std::exit(2); }
            return argv[++i];
        };
        if (a == "--analyze") want_analyze = true;
        else if (a == "--ratio") opts.time_ratio = std::atof(next("--ratio"));
        else if (a == "--pitch") opts.pitch_semitones = std::atof(next("--pitch"));
        else if (a == "--formant-semitones") opts.formant_semitones = std::atof(next("--formant-semitones"));
        else if (a == "--formant") { bool ok; opts.formant_mode = parse_formant(next("--formant"), &ok);
                                     if (!ok) { std::fprintf(stderr, "error: bad --formant\n"); return 2; } }
        else if (a == "--repitch") opts.repitch_linked = true;
        else if (a == "--quality") opts.quality = std::atoi(next("--quality"));
        else if (a == "--max-ratio") opts.max_time_ratio = std::atof(next("--max-ratio"));
        else if (a == "--max-pitch") opts.max_pitch_semitones = std::atof(next("--max-pitch"));
        else if (a == "--fft") opts.fft_size = std::atoi(next("--fft"));      // manual window override (0=auto)
        else if (a == "--hop") opts.analysis_hop = std::atoi(next("--hop"));
        else if (a == "--no-stn") opts.route_noise_stn = false;               // (default) bypass STN noise morph
        else if (a == "--stn") opts.route_noise_stn = true;                   // opt into STN noise morph
        else if (a == "--transient-sens") opts.transient_sensitivity = std::atof(next("--transient-sens"));
        else if (a == "--relocate") opts.transient_mode = pulp::signal::StretchTransientMode::verbatim_relocate;
        else if (a == "--character") {
            bool ok = false; opts.character = parse_character(next("--character"), &ok);
            if (!ok) { std::fprintf(stderr, "error: --character must be clean|varispeed|phase_vocoder|granular\n"); return 2; }
        }
        else if (a == "--preset") {
            const char* path = next("--preset");
            std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
            pulp::signal::StretchPreset p; std::string perr;
            if (!f || !pulp::signal::preset_from_text(ss.str(), p, &perr)) {
                std::fprintf(stderr, "error: --preset '%s': %s\n", path, perr.empty() ? "cannot read" : perr.c_str());
                return 2;
            }
            pulp::signal::apply_preset(opts, p); // later flags can still override
        }
        else if (a == "--save-preset") save_preset_path = next("--save-preset");
        else if (a == "--bpm-to") bpm_to = std::atof(next("--bpm-to"));
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "error: unknown flag %s\n", a.c_str()); usage(); return 2; }
        else pos.push_back(a);
    }

    if (!save_preset_path.empty()) {
        std::ofstream f(save_preset_path);
        if (!f) { std::fprintf(stderr, "error: cannot write preset '%s'\n", save_preset_path.c_str()); return 2; }
        f << pulp::signal::preset_to_text(pulp::signal::capture_preset(opts, "stretchcli"));
        std::fprintf(stderr, "[stretchcli] wrote preset %s\n", save_preset_path.c_str());
    }

    if (want_analyze) {
        if (pos.size() != 1) { usage(); return 2; }
        return analyze(pos[0]);
    }
    if (pos.size() != 2) { usage(); return 2; }
    return render(pos[0], pos[1], opts, bpm_to);
}

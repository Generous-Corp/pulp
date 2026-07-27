// Offline delay-effect A/B probe.
//
// Renders ONE deterministic source through either a locally installed AU effect
// (addressed by its component identity) or Pulp's own CharacterDelay, and writes
// the result to a WAV. Running it once per engine with the same source and the
// same --seconds gives sample-aligned renders that the audio quality lab can
// compare directly.
//
// Rendering is entirely offline: AudioUnitRender is driven from this thread and
// no output device is ever opened, so a comparison run makes no sound.
//
// An AU is addressed by identity (TYPE:SUBT:MANU) rather than by scanning: the
// identity IS the complete loader descriptor, so we never walk every installed
// bundle and never instantiate unrelated third-party plugins. Read the codes off
// a bundle with:
//   /usr/libexec/PlistBuddy -c "Print :AudioComponents:0:type" \
//     -c "Print :AudioComponents:0:subtype" -c "Print :AudioComponents:0:manufacturer" \
//     "/Library/Audio/Plug-Ins/Components/<Name>.component/Contents/Info.plist"
//
// The third-party plugins this targets are machine-local and are a LISTENING
// REFERENCE only — this tool exists to characterise our own delay against a
// familiar yardstick, never to fit our tables to another vendor's output. It is
// deliberately a manual dev tool and is not registered as a test.
//
// Usage:
//   pulp-au-effect-ab --au aumf:MeBr:Artu --out /tmp/brigade.wav
//   pulp-au-effect-ab --pulp tape --out /tmp/pulp-tape.wav
//   pulp-au-effect-ab --au aumf:Eter:Artu --list-params
//
//   --src-gen impulse|pluck|tone   deterministic source (default pluck)
//   --src FILE                     use a WAV instead of a generated source
//   --seconds N                    render length (default 6)
//   --time-ms N / --feedback N     Pulp engine controls (ignored for --au)
//   --tier standard|physical|bbd-ish via --pulp NAME
//   --set-param ID=VALUE           AU parameter, normalised 0..1, repeatable

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/character_delay.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;

using Character = pulp::signal::CharacterDelay::Character;
using TapeTier = pulp::signal::CharacterDelay::TapeTier;

struct Engine {
    const char* name;
    Character character;
    TapeTier tier;
};

// The characters worth A/B-ing against a hardware-modelled delay. `physical` is
// the Jiles-Atherton tape tier; `tape` is the cheaper analytic one.
constexpr Engine kEngines[] = {
    {"clean", Character::clean, TapeTier::standard},
    {"tape", Character::tape, TapeTier::standard},
    {"tape-physical", Character::tape, TapeTier::physical},
    {"bbd", Character::bbd, TapeTier::standard},
    {"vintage-digital", Character::vintage_digital, TapeTier::standard},
    {"diffusion", Character::diffusion, TapeTier::standard},
};

const Engine* find_engine(const std::string& name) {
    for (const auto& e : kEngines)
        if (name == e.name) return &e;
    return nullptr;
}

void print_usage(const char* program) {
    std::printf(
        "Usage: %s (--au TYPE:SUBT:MANU | --pulp ENGINE) [--out PATH]\n"
        "       [--src-gen impulse|pluck|tone] [--src FILE] [--seconds N]\n"
        "       [--time-ms N] [--feedback N] [--list-params] [--set-param ID=VAL]\n"
        "\nPulp engines:",
        program);
    for (const auto& e : kEngines) std::printf(" %s", e.name);
    std::printf("\n");
}

// Deterministic mono source, duplicated to stereo by the caller. Kept in-tool
// so an A/B needs no asset and is bit-identical across machines and runs.
std::vector<float> generate_source(const std::string& kind, int total) {
    std::vector<float> s(static_cast<std::size_t>(total), 0.0f);
    const auto sr = static_cast<double>(kSampleRate);
    if (kind == "impulse") {
        // A single unit impulse exposes the repeat structure directly: each
        // echo's amplitude, spacing, and per-pass filtering are readable
        // straight off the render.
        if (total > 0) s[0] = 0.9f;
        return s;
    }
    if (kind == "tone") {
        for (int i = 0; i < total; ++i)
            s[static_cast<std::size_t>(i)] =
                static_cast<float>(0.3 * std::sin(2.0 * M_PI * 440.0 * i / sr));
        return s;
    }
    // Default: four short plucks. Transients reveal smear and the tails reveal
    // per-repeat dulling, which is what the character tables actually change.
    const double hits[] = {0.0, 0.75, 1.5, 2.25};
    for (double at : hits) {
        const int start = static_cast<int>(at * sr);
        const int len = static_cast<int>(0.18 * sr);
        for (int i = 0; i < len && start + i < total; ++i) {
            const double t = i / sr;
            const double env = std::exp(-t * 26.0);
            const double body = std::sin(2.0 * M_PI * 220.0 * t) +
                                0.5 * std::sin(2.0 * M_PI * 440.0 * t) +
                                0.25 * std::sin(2.0 * M_PI * 880.0 * t);
            s[static_cast<std::size_t>(start + i)] +=
                static_cast<float>(0.28 * env * body);
        }
    }
    return s;
}

struct Stats {
    float peak = 0.0f;
    double rms = 0.0;
};

Stats measure(const std::vector<std::vector<float>>& ch) {
    Stats st;
    double sum_sq = 0.0;
    std::size_t n = 0;
    for (const auto& c : ch)
        for (float v : c) {
            st.peak = std::max(st.peak, std::abs(v));
            sum_sq += static_cast<double>(v) * v;
            ++n;
        }
    st.rms = n ? std::sqrt(sum_sq / static_cast<double>(n)) : 0.0;
    return st;
}

int render_pulp(const Engine& engine, const std::vector<float>& src, double time_ms,
                double feedback, std::vector<std::vector<float>>& out) {
    pulp::signal::CharacterDelay delay;
    delay.set_character(engine.character);
    delay.set_tape_tier(engine.tier);
    delay.set_sample_rate(kSampleRate);
    delay.set_time_ms(static_cast<float>(time_ms));
    delay.set_feedback(static_cast<float>(feedback));
    delay.set_character_amount(0.6f);
    delay.set_crossfeed(0.0f);
    delay.set_mod(0.25f, 0.05f);
    delay.set_duck(0.0f);
    delay.reset();

    const int total = static_cast<int>(src.size());
    out.assign(2, std::vector<float>(static_cast<std::size_t>(total), 0.0f));
    for (int i = 0; i < total; ++i) {
        out[0][static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(i)];
        out[1][static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(i)];
    }
    // CharacterDelay processes in place, one stereo pair at a time.
    for (int pos = 0; pos < total; pos += kBlockSize) {
        const int n = std::min(kBlockSize, total - pos);
        delay.process(out[0].data() + pos, out[1].data() + pos, n);
    }
    return 0;
}

int render_au(const std::string& identity, const std::vector<float>& src, bool list_params,
              const std::vector<std::pair<uint32_t, float>>& param_sets,
              std::vector<std::vector<float>>& out) {
    pulp::host::PluginInfo info;
    if (!pulp::host::plugin_info_from_au_identity(identity, info)) {
        std::printf("FAIL: '%s' is not a well-formed AU identity (want TYPE:SUBT:MANU)\n",
                    identity.c_str());
        return 2;
    }
    auto slot = pulp::host::PluginSlot::load(info);
    if (!slot || !slot->is_loaded()) {
        std::printf("FAIL: PluginSlot::load('%s') returned %s\n", identity.c_str(),
                    slot ? "an unloaded slot" : "nullptr");
        return 3;
    }
    std::printf("loaded: %s — %s\n", slot->info().name.c_str(),
                slot->info().manufacturer.c_str());

    // prepare() BEFORE touching parameters. An Audio Unit generally does not
    // publish its parameter list until the unit is initialised, so enumerating
    // first reports zero params for every plugin — including ones that
    // obviously have them (Apple's own aufx:dely:appl), which reads as "this
    // plugin exposes nothing" rather than "we asked too early".
    if (!slot->prepare(kSampleRate, kBlockSize)) {
        std::printf("FAIL: prepare(%.0f, %d)\n", kSampleRate, kBlockSize);
        return 4;
    }

    if (list_params) {
        const auto params_list = slot->parameters();
        std::printf("  %zu parameter(s)\n", params_list.size());
        for (const auto& p : params_list)
            std::printf("  param %u  %-38s = %.4f\n", p.id, p.name.c_str(),
                        slot->get_parameter(p.id));
        slot->release();
        return 0;
    }
    for (const auto& [id, value] : param_sets) slot->set_parameter(id, value);

    const int total = static_cast<int>(src.size());
    out.assign(2, std::vector<float>(static_cast<std::size_t>(total), 0.0f));

    std::vector<std::vector<float>> in_block(2, std::vector<float>(kBlockSize, 0.0f));
    std::vector<std::vector<float>> out_block(2, std::vector<float>(kBlockSize, 0.0f));
    std::vector<const float*> in_ptrs(2);
    std::vector<float*> out_ptrs(2);
    for (int c = 0; c < 2; ++c) {
        in_ptrs[static_cast<std::size_t>(c)] = in_block[static_cast<std::size_t>(c)].data();
        out_ptrs[static_cast<std::size_t>(c)] = out_block[static_cast<std::size_t>(c)].data();
    }

    pulp::host::ParameterEventQueue params;
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;

    for (int pos = 0; pos < total; pos += kBlockSize) {
        const int n = std::min(kBlockSize, total - pos);
        for (int c = 0; c < 2; ++c) {
            std::fill(out_block[static_cast<std::size_t>(c)].begin(),
                      out_block[static_cast<std::size_t>(c)].end(), 0.0f);
            for (int i = 0; i < n; ++i)
                in_block[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                    src[static_cast<std::size_t>(pos + i)];
        }
        pulp::audio::BufferView<const float> in(in_ptrs.data(), 2, n);
        pulp::audio::BufferView<float> ob(out_ptrs.data(), 2, n);
        slot->process(ob, in, midi_in, midi_out, params, n);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                out[static_cast<std::size_t>(c)][static_cast<std::size_t>(pos + i)] =
                    out_block[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
    }
    slot->release();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string au_identity, pulp_engine, out_path = "/tmp/pulp-effect-ab.wav";
    std::string src_kind = "pluck", src_file;
    double seconds = 6.0, time_ms = 375.0, feedback = 0.45;
    bool list_params = false;
    std::vector<std::pair<uint32_t, float>> param_sets;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--au") { if (auto v = next()) au_identity = v; }
        else if (a == "--pulp") { if (auto v = next()) pulp_engine = v; }
        else if (a == "--out") { if (auto v = next()) out_path = v; }
        else if (a == "--src-gen") { if (auto v = next()) src_kind = v; }
        else if (a == "--src") { if (auto v = next()) src_file = v; }
        else if (a == "--seconds") { if (auto v = next()) seconds = std::atof(v); }
        else if (a == "--time-ms") { if (auto v = next()) time_ms = std::atof(v); }
        else if (a == "--feedback") { if (auto v = next()) feedback = std::atof(v); }
        else if (a == "--list-params") { list_params = true; }
        else if (a == "--set-param") {
            if (auto v = next()) {
                const char* eq = std::strchr(v, '=');
                if (eq) param_sets.emplace_back(
                    static_cast<uint32_t>(std::atoi(std::string(v, eq).c_str())),
                    static_cast<float>(std::atof(eq + 1)));
            }
        } else { print_usage(argv[0]); return 1; }
    }

    if (au_identity.empty() == pulp_engine.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    const int total = static_cast<int>(seconds * kSampleRate);
    std::vector<float> src;
    if (!src_file.empty()) {
        auto loaded = pulp::audio::read_audio_file(src_file);
        if (!loaded || loaded->empty()) {
            std::printf("FAIL: could not read source '%s'\n", src_file.c_str());
            return 2;
        }
        src.assign(loaded->channels[0].begin(), loaded->channels[0].end());
        src.resize(static_cast<std::size_t>(total), 0.0f);
    } else {
        src = generate_source(src_kind, total);
    }

    std::vector<std::vector<float>> out;
    int rc = 0;
    if (!au_identity.empty()) {
        rc = render_au(au_identity, src, list_params, param_sets, out);
        if (list_params || rc != 0) return rc;
    } else {
        const Engine* e = find_engine(pulp_engine);
        if (!e) {
            std::printf("FAIL: unknown Pulp engine '%s'\n", pulp_engine.c_str());
            print_usage(argv[0]);
            return 2;
        }
        rc = render_pulp(*e, src, time_ms, feedback, out);
        if (rc != 0) return rc;
    }

    const Stats st = measure(out);
    std::printf("rendered: %.2fs @ %.0f Hz  peak=%.4f (%.1f dBFS)  rms=%.5f\n", seconds,
                kSampleRate, st.peak,
                st.peak > 0 ? 20.0 * std::log10(st.peak) : -999.0, st.rms);

    if (st.peak <= 1e-6f) {
        std::printf("RESULT: SILENT render — check the engine actually passed audio\n");
        return 5;
    }

    pulp::audio::AudioFileData data;
    data.sample_rate = static_cast<uint32_t>(kSampleRate);
    data.channels = out;
    if (!pulp::audio::write_wav_file(out_path, data, pulp::audio::WavBitDepth::Float32)) {
        std::printf("FAIL: write_wav_file('%s')\n", out_path.c_str());
        return 6;
    }
    std::printf("wrote: %s (%llu frames, float32)\nRESULT: WORKING\n", out_path.c_str(),
                static_cast<unsigned long long>(data.num_frames()));
    return 0;
}

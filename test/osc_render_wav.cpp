// osc-render-wav — render an in-tree Pulp oscillator to a WAV file, so the
// offline Python Quality Lab can analyze oscillator output without a plugin
// bundle. It drives the in-tree `VcoOscillator` through a Processor and
// HeadlessHost (RenderScenario), then writes the render with the shipped WAV
// writer. Deterministic: identical arguments produce a byte-identical file.
//
// Usage:
//   osc-render-wav --out PATH [--shape sine|saw|square|triangle]
//                  [--freq HZ] [--sr HZ] [--dur-ms MS] [--channels N]
//                  [--drift-cents C] [--jitter-cents C] [--bits float|int24]
//
// Defaults render a 250 ms 440 Hz sine at 48 kHz, mono, float32.

#include "support/osc_wav_scenario.hpp"
#include "support/wav_bridge.hpp"

#include <pulp/audio/audio_file.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using pulp::test::audio::OscRenderSpec;

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --out PATH [--shape sine|saw|square|triangle] "
                 "[--freq HZ] [--sr HZ] [--dur-ms MS] [--channels N] "
                 "[--drift-cents C] [--jitter-cents C] [--bits float|int24]\n",
                 argv0);
}

// Read the value that follows `flag` at argv[i]; advances i. Returns nullptr
// (and reports) when the value is missing.
const char* value_for(int argc, char** argv, int& i, const char* flag) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "osc-render-wav: %s needs a value\n", flag);
        return nullptr;
    }
    return argv[++i];
}

} // namespace

int main(int argc, char** argv) {
    OscRenderSpec spec;
    std::string out_path;
    auto bit_depth = pulp::audio::WavBitDepth::Float32;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--out") == 0) {
            const char* v = value_for(argc, argv, i, "--out");
            if (!v) return 2;
            out_path = v;
        } else if (std::strcmp(arg, "--shape") == 0) {
            const char* v = value_for(argc, argv, i, "--shape");
            if (!v) return 2;
            auto shape = pulp::test::audio::parse_shape(v);
            if (!shape) {
                std::fprintf(stderr, "osc-render-wav: unknown shape '%s'\n", v);
                return 2;
            }
            spec.shape = *shape;
        } else if (std::strcmp(arg, "--freq") == 0) {
            const char* v = value_for(argc, argv, i, "--freq");
            if (!v) return 2;
            spec.frequency_hz = std::atof(v);
        } else if (std::strcmp(arg, "--sr") == 0) {
            const char* v = value_for(argc, argv, i, "--sr");
            if (!v) return 2;
            spec.sample_rate = std::atof(v);
        } else if (std::strcmp(arg, "--dur-ms") == 0) {
            const char* v = value_for(argc, argv, i, "--dur-ms");
            if (!v) return 2;
            spec.duration_ms = std::atof(v);
        } else if (std::strcmp(arg, "--channels") == 0) {
            const char* v = value_for(argc, argv, i, "--channels");
            if (!v) return 2;
            spec.channels = std::atoi(v);
        } else if (std::strcmp(arg, "--drift-cents") == 0) {
            const char* v = value_for(argc, argv, i, "--drift-cents");
            if (!v) return 2;
            spec.drift_cents = std::atof(v);
        } else if (std::strcmp(arg, "--jitter-cents") == 0) {
            const char* v = value_for(argc, argv, i, "--jitter-cents");
            if (!v) return 2;
            spec.jitter_cents = std::atof(v);
        } else if (std::strcmp(arg, "--bits") == 0) {
            const char* v = value_for(argc, argv, i, "--bits");
            if (!v) return 2;
            if (std::strcmp(v, "float") == 0) {
                bit_depth = pulp::audio::WavBitDepth::Float32;
            } else if (std::strcmp(v, "int24") == 0) {
                bit_depth = pulp::audio::WavBitDepth::Int24;
            } else {
                std::fprintf(stderr, "osc-render-wav: unknown --bits '%s' "
                                     "(want float or int24)\n", v);
                return 2;
            }
        } else if (std::strcmp(arg, "--help") == 0 ||
                   std::strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "osc-render-wav: unknown argument '%s'\n", arg);
            usage(argv[0]);
            return 2;
        }
    }

    if (out_path.empty()) {
        std::fprintf(stderr, "osc-render-wav: --out is required\n");
        usage(argv[0]);
        return 2;
    }
    if (spec.channels < 1) {
        std::fprintf(stderr, "osc-render-wav: --channels must be >= 1\n");
        return 2;
    }

    pulp::test::audio::ScenarioResult result;
    try {
        result = pulp::test::audio::make_oscillator_scenario(spec).render();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "osc-render-wav: render failed: %s\n", e.what());
        return 1;
    }

    if (!pulp::test::audio::write_scenario_wav(result, out_path, bit_depth)) {
        std::fprintf(stderr, "osc-render-wav: could not write %s\n",
                     out_path.c_str());
        return 1;
    }

    std::printf("wrote %s: shape=%s freq=%.4f Hz sr=%.1f frames=%zu "
                "channels=%d bits=%s\n",
                out_path.c_str(), pulp::test::audio::shape_name(spec.shape),
                spec.frequency_hz, spec.sample_rate,
                result.output.num_samples(), spec.channels,
                bit_depth == pulp::audio::WavBitDepth::Float32 ? "float"
                                                              : "int24");
    return 0;
}

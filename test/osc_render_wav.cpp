// osc-render-wav — render an in-tree Pulp oscillator to a WAV file, so the
// offline Python Quality Lab can analyze oscillator output without a plugin
// bundle. It drives an in-tree oscillator engine (vco/dco/wt) through a
// Processor and HeadlessHost (RenderScenario), then writes the render with
// the shipped WAV writer. Deterministic: identical arguments produce a
// byte-identical file.
//
// Usage:
//   osc-render-wav --out PATH [--engine vco|dco|wt]
//                  [--shape sine|saw|square|triangle]
//                  [--freq HZ] [--sr HZ] [--dur-ms MS] [--channels N]
//                  [--drift-cents C] [--jitter-cents C] [--seed N]
//                  [--bits float|int24]
//
// Defaults render a 250 ms 440 Hz sine at 48 kHz, mono, float32, through the
// vco engine. --drift-cents/--jitter-cents/--seed apply only to vco (dco has
// no drift/jitter — its imperfection is pitch quantization; wt has no noise
// source at all); --shape does not apply to wt, which plays a fixed
// wavetable set instead of a classical shape.

#include "support/osc_wav_scenario.hpp"
#include "support/wav_bridge.hpp"

#include <pulp/audio/audio_file.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using pulp::test::audio::OscEngine;
using pulp::test::audio::OscRenderSpec;

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --out PATH [--engine vco|dco|wt] "
                 "[--shape sine|saw|square|triangle] "
                 "[--freq HZ] [--sr HZ] [--dur-ms MS] [--channels N] "
                 "[--drift-cents C] [--jitter-cents C] [--seed N] "
                 "[--bits float|int24]\n",
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
    bool shape_given = false;
    bool drift_given = false;
    bool jitter_given = false;
    bool seed_given = false;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--out") == 0) {
            const char* v = value_for(argc, argv, i, "--out");
            if (!v) return 2;
            out_path = v;
        } else if (std::strcmp(arg, "--engine") == 0) {
            const char* v = value_for(argc, argv, i, "--engine");
            if (!v) return 2;
            auto engine = pulp::test::audio::parse_engine(v);
            if (!engine) {
                std::fprintf(stderr, "osc-render-wav: unknown engine '%s' "
                                     "(want vco, dco, or wt)\n", v);
                return 2;
            }
            spec.engine = *engine;
        } else if (std::strcmp(arg, "--shape") == 0) {
            const char* v = value_for(argc, argv, i, "--shape");
            if (!v) return 2;
            auto shape = pulp::test::audio::parse_shape(v);
            if (!shape) {
                std::fprintf(stderr, "osc-render-wav: unknown shape '%s'\n", v);
                return 2;
            }
            spec.shape = *shape;
            shape_given = true;
        } else if (std::strcmp(arg, "--freq") == 0) {
            const char* v = value_for(argc, argv, i, "--freq");
            if (!v) return 2;
            // Note: the frequency reaches the oscillator through a float32 StateStore
            // parameter, so a non-float32-exact value renders at float32(freq) — up to
            // ~3e-4 Hz off the double parsed here. Harmless for musical use; pick a
            // float32-exact frequency when cross-checking a double-math model bit-for-bit.
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
            drift_given = true;
        } else if (std::strcmp(arg, "--jitter-cents") == 0) {
            const char* v = value_for(argc, argv, i, "--jitter-cents");
            if (!v) return 2;
            spec.jitter_cents = std::atof(v);
            jitter_given = true;
        } else if (std::strcmp(arg, "--seed") == 0) {
            const char* v = value_for(argc, argv, i, "--seed");
            if (!v) return 2;
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(v, &end, 10);
            if (end == v || *end != '\0' || errno == ERANGE ||
                parsed > pulp::test::audio::kOscMaxExactSeed) {
                std::fprintf(stderr,
                             "osc-render-wav: --seed must be an integer in "
                             "[0, %llu] (exact in float32)\n",
                             static_cast<unsigned long long>(
                                 pulp::test::audio::kOscMaxExactSeed));
                return 2;
            }
            spec.seed = static_cast<std::uint64_t>(parsed);
            seed_given = true;
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
    if (spec.engine != OscEngine::vco && (drift_given || jitter_given)) {
        std::fprintf(stderr,
                     "osc-render-wav: --drift-cents/--jitter-cents apply "
                     "only to --engine vco (%s has no seeded pitch noise)\n",
                     pulp::test::audio::engine_name(spec.engine));
        return 2;
    }
    if (spec.engine != OscEngine::vco && seed_given) {
        std::fprintf(stderr,
                     "osc-render-wav: --seed applies only to --engine vco "
                     "(%s has no noise seed)\n",
                     pulp::test::audio::engine_name(spec.engine));
        return 2;
    }
    if (spec.engine == OscEngine::wt && shape_given) {
        std::fprintf(stderr,
                     "osc-render-wav: --shape does not apply to --engine wt "
                     "(it plays a fixed wavetable set)\n");
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

    // `wt` has no shape parameter — report "n/a" rather than the spec's
    // unused default, which would misleadingly imply the render used it.
    const char* shape_field = spec.engine == OscEngine::wt
                                   ? "n/a"
                                   : pulp::test::audio::shape_name(spec.shape);
    std::printf("wrote %s: engine=%s shape=%s freq=%.4f Hz sr=%.1f frames=%zu "
                "channels=%d bits=%s\n",
                out_path.c_str(), pulp::test::audio::engine_name(spec.engine),
                shape_field, spec.frequency_hz, spec.sample_rate,
                result.output.num_samples(), spec.channels,
                bit_depth == pulp::audio::WavBitDepth::Float32 ? "float"
                                                              : "int24");
    return 0;
}

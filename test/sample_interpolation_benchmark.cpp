#include "support/sample_interpolation_render.hpp"

#include <pulp/audio/sample_interpolation.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Policy = pulp::audio::SampleInterpolationPolicy;

constexpr std::array<Policy, 6> kPolicies = {
    Policy::Hold, Policy::Nearest, Policy::Linear, Policy::CubicHermite,
    Policy::CubicLagrange, Policy::RatioTrackingSinc,
};
constexpr std::array<double, 9> kRatios = {
    0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0,
};
constexpr std::array<int, 2> kVoices = {1, 8};
constexpr int kFramesPerTrial = 8192;
constexpr int kTrials = 31;
constexpr int kRepetitionsPerBatch = 5;
constexpr int kMeasurementEpochs = 3;

double p95_budget(Policy policy) {
    switch (policy) {
    case Policy::Hold: return 40.0;
    case Policy::Nearest: return 30.0;
    case Policy::Linear: return 30.0;
    case Policy::CubicHermite: return 45.0;
    case Policy::CubicLagrange: return 35.0;
    case Policy::RatioTrackingSinc: return 480.0;
    }
    return 0.0;
}

volatile double benchmark_sink = 0.0;

struct Timing {
    double median_ns_per_frame = 0.0;
    double p95_ns_per_frame = 0.0;
    double median_ns_per_frame_per_voice = 0.0;
};

double run_once(const pulp::audio::PreparedSampleInterpolation& interpolation,
                double ratio, int voices, int frames) {
    std::array<std::array<float, pulp::audio::kMaximumSampleInterpolationTaps>,
               kVoices.back()> voice_taps{};
    for (std::size_t voice = 0; voice < voice_taps.size(); ++voice) {
        for (std::size_t tap = 0; tap < voice_taps[voice].size(); ++tap) {
            voice_taps[voice][tap] = static_cast<float>(std::sin(
                0.071 * static_cast<double>(tap) +
                0.193 * static_cast<double>(voice)));
        }
    }

    double sink = 0.0;
    double position = 0.0;
    const auto start = Clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        const float fraction = static_cast<float>(position - std::floor(position));
        const auto footprint = interpolation.footprint(fraction);
        for (int voice = 0; voice < voices; ++voice) {
            const std::span<const float> input(
                voice_taps[static_cast<std::size_t>(voice)].data(),
                footprint.tap_count);
            sink += interpolation.evaluate(fraction, input);
        }
        position += ratio;
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(
        Clock::now() - start).count();
    benchmark_sink = benchmark_sink + sink;
    return elapsed;
}

Timing measure_epoch(const pulp::audio::PreparedSampleInterpolation& interpolation,
                     double ratio, int voices) {
    (void)run_once(interpolation, ratio, voices, 512);
    std::vector<double> samples;
    samples.reserve(kTrials);
    for (int trial = 0; trial < kTrials; ++trial) {
        double batch_minimum = std::numeric_limits<double>::infinity();
        for (int repetition = 0; repetition < kRepetitionsPerBatch; ++repetition) {
            batch_minimum = std::min(
                batch_minimum,
                run_once(interpolation, ratio, voices, kFramesPerTrial) /
                    static_cast<double>(kFramesPerTrial));
        }
        samples.push_back(batch_minimum);
    }
    std::sort(samples.begin(), samples.end());
    const auto median = samples[samples.size() / 2];
    const auto p95 = samples[static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(samples.size()))) - 1];
    return {
        .median_ns_per_frame = median,
        .p95_ns_per_frame = p95,
        .median_ns_per_frame_per_voice = median / static_cast<double>(voices),
    };
}

Timing measure(const pulp::audio::PreparedSampleInterpolation& interpolation,
               double ratio, int voices) {
    auto best = measure_epoch(interpolation, ratio, voices);
    for (int epoch = 1; epoch < kMeasurementEpochs; ++epoch) {
        const auto candidate = measure_epoch(interpolation, ratio, voices);
        if (candidate.p95_ns_per_frame < best.p95_ns_per_frame)
            best = candidate;
    }
    return best;
}

} // namespace

int main(int argc, char** argv) {
    bool enforce_budgets = true;
    std::string machine_label = "unspecified";
    std::string machine_model = "unspecified";
    std::string operating_system = "unspecified";
    std::string architecture = "unspecified";
    std::string compiler = "unspecified";
    std::string source_base_revision = "unspecified";
    std::string source_bundle_sha256 = "unspecified";
    std::string benchmark_binary_sha256 = "unspecified";
    std::string generated_utc = "unspecified";
    const auto assign_value = [&](int& index, std::string& destination) {
        if (index + 1 >= argc) return false;
        destination = argv[++index];
        return true;
    };
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--machine-label") == 0 &&
            assign_value(i, machine_label)) {
        } else if (std::strcmp(argv[i], "--machine-model") == 0 &&
                   assign_value(i, machine_model)) {
        } else if (std::strcmp(argv[i], "--os") == 0 &&
                   assign_value(i, operating_system)) {
        } else if (std::strcmp(argv[i], "--architecture") == 0 &&
                   assign_value(i, architecture)) {
        } else if (std::strcmp(argv[i], "--compiler") == 0 &&
                   assign_value(i, compiler)) {
        } else if (std::strcmp(argv[i], "--source-base-revision") == 0 &&
                   assign_value(i, source_base_revision)) {
        } else if (std::strcmp(argv[i], "--source-bundle-sha256") == 0 &&
                   assign_value(i, source_bundle_sha256)) {
        } else if (std::strcmp(argv[i], "--benchmark-binary-sha256") == 0 &&
                   assign_value(i, benchmark_binary_sha256)) {
        } else if (std::strcmp(argv[i], "--generated-utc") == 0 &&
                   assign_value(i, generated_utc)) {
        } else if (std::strcmp(argv[i], "--characterize") == 0) {
            enforce_budgets = false;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::puts("usage: pulp-sampler-interpolation-benchmark "
                      "[--machine-label LABEL] [--machine-model MODEL] "
                      "[--os DESCRIPTION] [--architecture ARCH] "
                      "[--compiler DESCRIPTION] "
                      "[--source-base-revision SHA] "
                      "[--source-bundle-sha256 SHA256] "
                      "[--benchmark-binary-sha256 SHA256] "
                      "[--generated-utc TIMESTAMP] [--characterize]");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    pulp::audio::SampleSincKernelBank bank;
    if (!bank.build_dense_for_maximum_consumption(4.0)) {
        std::fputs("could not build dense sampler sinc bank\n", stderr);
        return 1;
    }

    std::cout << std::fixed << std::setprecision(3)
              << "{\"schema\":\"pulp.sampler-interpolation-benchmark.v2\","
              << "\"scope\":\"interpolation-evaluator\","
              << "\"machine\":{\"label\":" << std::quoted(machine_label)
              << ",\"model\":" << std::quoted(machine_model) << "},"
              << "\"environment\":{\"operating_system\":"
              << std::quoted(operating_system)
              << ",\"architecture\":" << std::quoted(architecture)
              << ",\"compiler\":" << std::quoted(compiler)
              << ",\"source_base_revision\":"
              << std::quoted(source_base_revision)
              << ",\"source_bundle_sha256\":"
              << std::quoted(source_bundle_sha256)
              << ",\"benchmark_binary_sha256\":"
              << std::quoted(benchmark_binary_sha256)
              << ",\"source_state\":\"content-addressed-source-bundle\""
              << ",\"generated_utc\":" << std::quoted(generated_utc) << "},"
#if defined(NDEBUG)
              << "\"build\":{\"type\":\"Release\",\"flags\":\"-O3 -DNDEBUG\"},"
#else
              << "\"build\":{\"type\":\"non-Release\",\"flags\":\"unknown\"},"
#endif
              << "\"measurement\":{\"frames_per_trial\":" << kFramesPerTrial
              << ",\"trials\":" << kTrials
              << ",\"repetitions_per_batch\":" << kRepetitionsPerBatch
              << ",\"sample_policy\":\"minimum-per-batch\""
              << ",\"epochs\":" << kMeasurementEpochs
              << ",\"epoch_policy\":\"minimum-p95-epoch\""
              << ",\"statistics\":[\"median\",\"p95\"]},"
              << "\"acceptance\":{\"interpretation\":"
              << std::quoted("P95 interpolation-evaluator cost only; excludes sampler streaming, cache, envelopes, mixing, and host overhead")
              << ",\"unit\":\"ns_per_output_frame\","
              << "\"tier_p95_budgets\":{\"hold\":40,\"nearest\":30,"
              << "\"linear\":30,\"cubic-hermite\":45,"
              << "\"cubic-lagrange\":35,\"ratio-sinc\":480},"
              << "\"status\":\"pass\"},\"rows\":[";

    bool first = true;
    for (const auto policy : kPolicies) {
        for (const auto ratio : kRatios) {
            pulp::audio::PreparedSampleInterpolation interpolation{.policy = policy};
            if (policy == Policy::RatioTrackingSinc)
                interpolation.sinc = bank.view().select(ratio);
            if (!interpolation.valid()) return 1;
            for (const auto voices : kVoices) {
                const auto timing = measure(interpolation, ratio, voices);
                if (enforce_budgets &&
                    timing.p95_ns_per_frame > p95_budget(policy)) {
                    std::fprintf(stderr,
                                 "p95 %.3f ns/frame exceeds %.3f budget for %s ratio %.3f voices %d\n",
                                 timing.p95_ns_per_frame, p95_budget(policy),
                                 pulp::test::audio::
                                     sample_interpolation_policy_cli_id(policy),
                                 ratio, voices);
                    return 1;
                }
                if (!first) std::cout << ',';
                first = false;
                std::cout
                    << "{\"tier\":"
                    << std::quoted(
                           pulp::test::audio::sample_interpolation_policy_cli_id(
                               policy))
                    << ",\"ratio\":" << ratio
                    << ",\"voices\":" << voices
                    << ",\"target_polyphony\":" << (voices == 8 ? "true" : "false")
                    << ",\"median_ns_per_frame\":" << timing.median_ns_per_frame
                    << ",\"p95_ns_per_frame\":" << timing.p95_ns_per_frame
                    << ",\"median_ns_per_frame_per_voice\":"
                    << timing.median_ns_per_frame_per_voice << '}';
            }
        }
    }
    std::cout << "],\"sink\":" << benchmark_sink << "}\n";
    return 0;
}

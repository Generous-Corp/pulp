// Advisory Apple-only screening and actual-consumer fast-trig benchmark.
//
// The model compares challengers; the matrix measures the selected opt-in
// profile inside AdditiveBankT. Timing never gates CI.

#include <pulp/signal/additive_bank.hpp>
#include <pulp/signal/fast_math.hpp>

#include <Accelerate/Accelerate.h>
#include <simd/math.h>
#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kPartials = 64;
constexpr std::size_t kFrames = 1u << 12;
constexpr int kWarmups = 2;
constexpr int kPasses = 5;
constexpr int kTrials = 15;
constexpr float kTwoPi = 6.28318530717958647692f;

volatile double g_sink = 0.0;

struct State {
    std::array<float, kPartials> phase{};
    std::array<float, kPartials> increment{};
    std::array<float, kPartials> weight{};
};

State make_state(std::size_t seed) {
    State state;
    for (std::size_t i = 0; i < kPartials; ++i) {
        state.phase[i] = static_cast<float>((seed + i * 17) & 1023u) / 1024.0f;
        state.increment[i] = 0.000113f * static_cast<float>(i + 1);
        state.weight[i] = 1.0f / static_cast<float>(i + 1);
    }
    return state;
}

float wrap(float phase) {
    return phase - std::floor(phase);
}

template <typename Sine> double render_scalar(State state, float* output, Sine&& sine) {
    double checksum = 0.0;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        double sample = 0.0;
        for (std::size_t i = 0; i < kPartials; ++i) {
            state.phase[i] = wrap(state.phase[i] + state.increment[i]);
            sample += static_cast<double>(state.weight[i] * sine(state.phase[i]));
        }
        if (output)
            output[frame] = static_cast<float>(sample);
        checksum += sample * static_cast<double>(frame + 1);
    }
    return checksum;
}

enum class AppleVectorKernel { sinpi, polynomial };

double render_apple_vector(State state, float* output, AppleVectorKernel kernel) {
    std::array<simd_float4, kPartials / 4> phase{};
    std::array<simd_float4, kPartials / 4> increment{};
    std::array<simd_float4, kPartials / 4> weight{};
    for (std::size_t v = 0; v < phase.size(); ++v) {
        const std::size_t i = v * 4;
        phase[v] = simd_make_float4(state.phase[i], state.phase[i + 1], state.phase[i + 2],
                                    state.phase[i + 3]);
        increment[v] = simd_make_float4(state.increment[i], state.increment[i + 1],
                                        state.increment[i + 2], state.increment[i + 3]);
        weight[v] = simd_make_float4(state.weight[i], state.weight[i + 1], state.weight[i + 2],
                                     state.weight[i + 3]);
    }

    double checksum = 0.0;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        simd_float4 sum = simd_make_float4(0.0f);
        for (std::size_t v = 0; v < phase.size(); ++v) {
            phase[v] += increment[v];
            phase[v] -= simd::floor(phase[v]);
            const simd_float4 sine = kernel == AppleVectorKernel::sinpi
                                         ? simd::sinpi(phase[v] * 2.0f)
                                         : pulp::signal::FastMath::sin_cycles_precise(phase[v]);
            sum += weight[v] * sine;
        }
        const float sample = simd_reduce_add(sum);
        if (output)
            output[frame] = sample;
        checksum += static_cast<double>(sample) * static_cast<double>(frame + 1);
    }
    return checksum;
}

double render_vforce(State state, float* output) {
    std::array<float, kPartials> input{};
    std::array<float, kPartials> sine{};
    constexpr int count = static_cast<int>(kPartials);
    double checksum = 0.0;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        for (std::size_t i = 0; i < kPartials; ++i) {
            state.phase[i] = wrap(state.phase[i] + state.increment[i]);
            input[i] = 2.0f * state.phase[i];
        }
        vvsinpif(sine.data(), input.data(), &count);
        double sample = 0.0;
        for (std::size_t i = 0; i < kPartials; ++i)
            sample += static_cast<double>(state.weight[i] * sine[i]);
        if (output)
            output[frame] = static_cast<float>(sample);
        checksum += sample * static_cast<double>(frame + 1);
    }
    return checksum;
}

double render_recurrence(State state, float* output) {
    std::array<float, kPartials> sine{};
    std::array<float, kPartials> cosine{};
    std::array<float, kPartials> delta_sine{};
    std::array<float, kPartials> delta_cosine{};
    for (std::size_t i = 0; i < kPartials; ++i) {
        const float phase = kTwoPi * state.phase[i];
        const float delta = kTwoPi * state.increment[i];
        sine[i] = std::sin(phase);
        cosine[i] = std::cos(phase);
        delta_sine[i] = std::sin(delta);
        delta_cosine[i] = std::cos(delta);
    }

    double checksum = 0.0;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        double sample = 0.0;
        for (std::size_t i = 0; i < kPartials; ++i) {
            const float next_sine = sine[i] * delta_cosine[i] + cosine[i] * delta_sine[i];
            const float next_cosine = cosine[i] * delta_cosine[i] - sine[i] * delta_sine[i];
            sine[i] = next_sine;
            cosine[i] = next_cosine;
            sample += static_cast<double>(state.weight[i] * next_sine);
        }
        if (output)
            output[frame] = static_cast<float>(sample);
        checksum += sample * static_cast<double>(frame + 1);
    }
    return checksum;
}

struct Timing {
    double median_ns_per_frame;
    double p95_ns_per_frame;
    double checksum;
};

template <typename Render> Timing measure(Render&& render) {
    for (int warmup = 0; warmup < kWarmups; ++warmup)
        g_sink = render(make_state(static_cast<std::size_t>(warmup + 1)), nullptr);

    std::array<double, kTrials> trials{};
    double checksum = 0.0;
    for (int trial = 0; trial < kTrials; ++trial) {
        const auto begin = std::chrono::steady_clock::now();
        for (int pass = 0; pass < kPasses; ++pass)
            checksum = render(make_state(static_cast<std::size_t>(trial + pass + 7)), nullptr);
        const auto end = std::chrono::steady_clock::now();
        g_sink = checksum;
        trials[static_cast<std::size_t>(trial)] =
            std::chrono::duration<double, std::nano>(end - begin).count() /
            static_cast<double>(kPasses * kFrames);
    }
    std::sort(trials.begin(), trials.end());
    return {trials[trials.size() / 2], trials[(trials.size() * 95) / 100], checksum};
}

struct Error {
    double max_absolute;
    double rms;
};

template <typename Render> Error compare(Render&& render, const std::vector<float>& reference) {
    std::vector<float> candidate(kFrames);
    render(make_state(7), candidate.data());
    double maximum = 0.0;
    double squared = 0.0;
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        const double error = static_cast<double>(candidate[i]) - reference[i];
        maximum = std::max(maximum, std::abs(error));
        squared += error * error;
    }
    return {maximum, std::sqrt(squared / static_cast<double>(candidate.size()))};
}

pulp::signal::AdditiveBank make_bank(pulp::signal::FastTrigProfile profile,
                                     double sample_rate = 48000.0, int partials = 64,
                                     bool bell = false, bool doublet = false) {
    pulp::signal::AdditiveBank bank;
    bank.prepare(sample_rate, partials);
    bank.load_voice(bell ? pulp::signal::make_bell_voice(partials)
                         : pulp::signal::make_organ_voice(partials));
    bank.set_partial_count(partials);
    bank.set_attack_ms(pulp::signal::AdditiveBank::kAttackMinMs);
    bank.set_envelope_mode(pulp::signal::AdditiveBank::EnvelopeMode::shared_ar);
    bank.set_trig_profile(profile);
    bank.set_detune_cents(doublet ? 7.0 : 0.0);
    bank.retrigger();
    return bank;
}

struct BankCase {
    double sample_rate;
    int partials;
    int block_size;
    bool bell;
};

struct BankPairTiming {
    Timing reference;
    Timing candidate;
};

BankPairTiming measure_bank_pair(const BankCase& test_case) {
    constexpr int matrix_trials = 9;
    constexpr int matrix_passes = 3;
    std::array<double, matrix_trials> reference_trials{};
    std::array<double, matrix_trials> candidate_trials{};
    std::array<float, kFrames> output{};
    double reference_checksum = 0.0;
    double candidate_checksum = 0.0;

    const auto run = [&](pulp::signal::FastTrigProfile profile, double& checksum) {
        auto bank = make_bank(profile, test_case.sample_rate, test_case.partials, test_case.bell,
                              test_case.bell);
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t offset = 0; offset < output.size();
             offset += static_cast<std::size_t>(test_case.block_size)) {
            const int count = static_cast<int>(std::min<std::size_t>(
                static_cast<std::size_t>(test_case.block_size), output.size() - offset));
            bank.process(output.data() + offset, count);
        }
        const auto end = std::chrono::steady_clock::now();
        for (std::size_t frame = 0; frame < output.size(); ++frame)
            checksum += static_cast<double>(output[frame]) * static_cast<double>(frame + 1);
        g_sink = checksum;
        return std::chrono::duration<double, std::nano>(end - begin).count();
    };

    for (int trial = 0; trial < matrix_trials; ++trial) {
        double reference_elapsed = 0.0;
        double candidate_elapsed = 0.0;
        for (int pass = 0; pass < matrix_passes; ++pass) {
            if ((trial + pass) % 2 == 0) {
                reference_elapsed +=
                    run(pulp::signal::FastTrigProfile::reference, reference_checksum);
                candidate_elapsed +=
                    run(pulp::signal::FastTrigProfile::realtime_precise, candidate_checksum);
            } else {
                candidate_elapsed +=
                    run(pulp::signal::FastTrigProfile::realtime_precise, candidate_checksum);
                reference_elapsed +=
                    run(pulp::signal::FastTrigProfile::reference, reference_checksum);
            }
        }
        reference_trials[static_cast<std::size_t>(trial)] =
            reference_elapsed / static_cast<double>(matrix_passes * kFrames);
        candidate_trials[static_cast<std::size_t>(trial)] =
            candidate_elapsed / static_cast<double>(matrix_passes * kFrames);
    }
    std::sort(reference_trials.begin(), reference_trials.end());
    std::sort(candidate_trials.begin(), candidate_trials.end());
    return {{reference_trials[reference_trials.size() / 2], reference_trials.back(),
             reference_checksum},
            {candidate_trials[candidate_trials.size() / 2], candidate_trials.back(),
             candidate_checksum}};
}

Timing measure_bank(pulp::signal::FastTrigProfile profile) {
    std::array<float, kFrames> output{};
    for (int warmup = 0; warmup < kWarmups; ++warmup) {
        auto bank = make_bank(profile);
        bank.process(output.data(), static_cast<int>(output.size()));
        g_sink = output.back();
    }

    std::array<double, kTrials> trials{};
    double checksum = 0.0;
    for (int trial = 0; trial < kTrials; ++trial) {
        double elapsed = 0.0;
        for (int pass = 0; pass < kPasses; ++pass) {
            auto bank = make_bank(profile);
            const auto begin = std::chrono::steady_clock::now();
            bank.process(output.data(), static_cast<int>(output.size()));
            const auto end = std::chrono::steady_clock::now();
            elapsed += std::chrono::duration<double, std::nano>(end - begin).count();
            checksum += output[static_cast<std::size_t>((trial + pass) % kFrames)];
        }
        g_sink = checksum;
        trials[static_cast<std::size_t>(trial)] = elapsed / static_cast<double>(kPasses * kFrames);
    }
    std::sort(trials.begin(), trials.end());
    return {trials[trials.size() / 2], trials[(trials.size() * 95) / 100], checksum};
}

Timing measure_bank_next(pulp::signal::FastTrigProfile profile) {
    std::array<float, kFrames> output{};
    std::array<double, kTrials> trials{};
    double checksum = 0.0;
    for (int trial = 0; trial < kTrials; ++trial) {
        double elapsed = 0.0;
        for (int pass = 0; pass < kPasses; ++pass) {
            auto bank = make_bank(profile);
            const auto begin = std::chrono::steady_clock::now();
            for (auto& sample : output)
                sample = bank.next();
            const auto end = std::chrono::steady_clock::now();
            elapsed += std::chrono::duration<double, std::nano>(end - begin).count();
            for (std::size_t frame = 0; frame < output.size(); ++frame)
                checksum += static_cast<double>(output[frame]) * static_cast<double>(frame + 1);
        }
        g_sink = checksum;
        trials[static_cast<std::size_t>(trial)] = elapsed / static_cast<double>(kPasses * kFrames);
    }
    std::sort(trials.begin(), trials.end());
    return {trials[trials.size() / 2], trials[(trials.size() * 95) / 100], checksum};
}

Error compare_banks() {
    auto reference = make_bank(pulp::signal::FastTrigProfile::reference);
    auto candidate = make_bank(pulp::signal::FastTrigProfile::realtime_precise);
    std::array<float, kFrames> expected{};
    std::array<float, kFrames> actual{};
    reference.process(expected.data(), static_cast<int>(expected.size()));
    candidate.process(actual.data(), static_cast<int>(actual.size()));
    double maximum = 0.0;
    double squared = 0.0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const double error = static_cast<double>(actual[i]) - expected[i];
        maximum = std::max(maximum, std::abs(error));
        squared += error * error;
    }
    return {maximum, std::sqrt(squared / static_cast<double>(actual.size()))};
}

template <typename Render>
void report(std::string_view name, Render&& render, const std::vector<float>& reference,
            double reference_ns) {
    const auto timing = measure(render);
    const auto error = compare(render, reference);
    std::cout << "{\"kernel\":\"" << name
              << "\",\"median_ns_per_frame\":" << timing.median_ns_per_frame
              << ",\"p95_ns_per_frame\":" << timing.p95_ns_per_frame << ",\"speedup_percent\":"
              << 100.0 * (reference_ns - timing.median_ns_per_frame) / reference_ns
              << ",\"max_abs_error\":" << error.max_absolute << ",\"rms_error\":" << error.rms
              << ",\"checksum\":" << timing.checksum << "}\n";
}

} // namespace

int main() {
#if !defined(NDEBUG)
    std::cerr << "error: configure with -DCMAKE_BUILD_TYPE=Release\n";
    return 2;
#endif

    const auto scalar_standard = [](State state, float* output) {
        return render_scalar(state, output, [](float phase) { return std::sin(kTwoPi * phase); });
    };
    const auto scalar_precise = [](State state, float* output) {
        return render_scalar(state, output, [](float phase) {
            return pulp::signal::FastMath::sin_cycles<
                pulp::signal::FastTrigProfile::realtime_precise>(phase);
        });
    };
    const auto apple_sinpi = [](State state, float* output) {
        return render_apple_vector(state, output, AppleVectorKernel::sinpi);
    };
    const auto apple_polynomial = [](State state, float* output) {
        return render_apple_vector(state, output, AppleVectorKernel::polynomial);
    };

    std::vector<float> reference(kFrames);
    scalar_standard(make_state(7), reference.data());
    const auto baseline = measure(scalar_standard);

    std::cout << std::setprecision(10)
              << "{\"schema\":\"pulp.fast-trig-apple-bank-benchmark.v1\","
                 "\"partials\":"
              << kPartials << ",\"frames\":" << kFrames << ",\"trials\":" << kTrials
              << ",\"passes\":" << kPasses << "}\n";
    report("scalar-std-sinf", scalar_standard, reference, baseline.median_ns_per_frame);
    report("scalar-degree9", scalar_precise, reference, baseline.median_ns_per_frame);
    report("apple-simd-sinpi-f4", apple_sinpi, reference, baseline.median_ns_per_frame);
    report("apple-simd-degree9-f4", apple_polynomial, reference, baseline.median_ns_per_frame);
    report("apple-vforce-sinpi-f64-batch", render_vforce, reference, baseline.median_ns_per_frame);
    report("oscillator-recurrence", render_recurrence, reference, baseline.median_ns_per_frame);

    const auto bank_reference = measure_bank(pulp::signal::FastTrigProfile::reference);
    const auto bank_precise = measure_bank(pulp::signal::FastTrigProfile::realtime_precise);
    const auto bank_reference_next =
        measure_bank_next(pulp::signal::FastTrigProfile::reference);
    const auto bank_error = compare_banks();
    std::cout << "{\"kernel\":\"additive-bank-reference\","
                 "\"median_ns_per_frame\":"
              << bank_reference.median_ns_per_frame
              << ",\"p95_ns_per_frame\":" << bank_reference.p95_ns_per_frame
              << ",\"speedup_percent\":0,\"max_abs_error\":0,"
                 "\"rms_error\":0,\"checksum\":"
              << bank_reference.checksum << "}\n";
    std::cout << "{\"kernel\":\"additive-bank-reference-next\","
                 "\"median_ns_per_frame\":"
              << bank_reference_next.median_ns_per_frame
              << ",\"p95_ns_per_frame\":" << bank_reference_next.p95_ns_per_frame
              << ",\"speedup_percent\":0,\"max_abs_error\":0,"
                 "\"rms_error\":0,\"checksum\":"
              << bank_reference_next.checksum << "}\n";
    std::cout << "{\"kernel\":\"additive-bank-precise-simd\","
                 "\"median_ns_per_frame\":"
              << bank_precise.median_ns_per_frame
              << ",\"p95_ns_per_frame\":" << bank_precise.p95_ns_per_frame
              << ",\"speedup_percent\":"
              << 100.0 * (bank_reference.median_ns_per_frame - bank_precise.median_ns_per_frame) /
                     bank_reference.median_ns_per_frame
              << ",\"max_abs_error\":" << bank_error.max_absolute
              << ",\"rms_error\":" << bank_error.rms << ",\"checksum\":" << bank_precise.checksum
              << "}\n";

    for (bool bell : {false, true}) {
        for (double sample_rate : {44100.0, 48000.0, 96000.0}) {
            for (int partials : {16, 64, 128}) {
                for (int block_size : {32, 128, 512}) {
                    const BankCase test_case{sample_rate, partials, block_size, bell};
                    const auto timing = measure_bank_pair(test_case);
                    std::cout << "{\"kernel\":\"additive-bank-matrix\",\"voice\":\""
                              << (bell ? "bell" : "organ") << "\",\"sample_rate\":" << sample_rate
                              << ",\"partials\":" << partials << ",\"block_size\":" << block_size
                              << ",\"reference_ns_per_frame\":"
                              << timing.reference.median_ns_per_frame
                              << ",\"candidate_ns_per_frame\":"
                              << timing.candidate.median_ns_per_frame << ",\"speedup_percent\":"
                              << 100.0 *
                                     (timing.reference.median_ns_per_frame -
                                      timing.candidate.median_ns_per_frame) /
                                     timing.reference.median_ns_per_frame
                              << ",\"reference_checksum\":" << timing.reference.checksum
                              << ",\"reference_p95_ns_per_frame\":"
                              << timing.reference.p95_ns_per_frame
                              << ",\"candidate_p95_ns_per_frame\":"
                              << timing.candidate.p95_ns_per_frame
                              << ",\"candidate_checksum\":" << timing.candidate.checksum
                              << ",\"checksum_delta\":"
                              << timing.candidate.checksum - timing.reference.checksum
                              << "}\n";
                }
            }
        }
    }

    return std::isfinite(g_sink) ? 0 : 1;
}

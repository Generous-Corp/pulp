// Advisory Release-only complete-hit benchmark for production drum voices.
// Timing is reported as evidence and never used as a heterogeneous CI gate.

#include <pulp/signal/drum/engine_registry.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <string_view>
#include <vector>

namespace allocation_probe {
thread_local bool enabled = false;
thread_local std::size_t count = 0;
} // namespace allocation_probe

void* operator new(std::size_t size) {
    if (allocation_probe::enabled) ++allocation_probe::count;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size) {
    if (allocation_probe::enabled) ++allocation_probe::count;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using pulp::signal::drum::EngineId;
using pulp::signal::drum::TomVoice;
using pulp::signal::drum::Voice;

constexpr double kMaxHitSeconds = 8.0;
constexpr int kTrials = 11;
constexpr int kPasses = 3;
constexpr float kVelocity = 0.82f;
volatile double g_sink = 0.0;

struct Scenario {
    const char* name;
    EngineId engine;
    bool shaped_output;
};

// Stable engine identities are the production preset surface. This benchmark
// covers the non-FM percussion families; FM engines have dedicated benchmarks.
constexpr std::array<Scenario, 13> kScenarios{{
    {"kick.oscillator", EngineId::kick_oscillator, false},
    {"kick.resonant", EngineId::kick_resonant, false},
    {"kick.circuit", EngineId::kick_circuit, false},
    {"snare", EngineId::snare, false},
    {"hat", EngineId::hat, false},
    {"clap", EngineId::clap, false},
    {"tom.generic", EngineId::tom_generic, false},
    {"tom.simmons", EngineId::tom_simmons, false},
    {"cymbal.comb", EngineId::cymbal_comb, false},
    {"membrane.modal", EngineId::membrane_modal, false},
    {"string.karplus-strong", EngineId::string_karplus_strong, false},
    {"zap.cz", EngineId::zap_cz, false},
    {"zap.cz.output-fold", EngineId::zap_cz, true},
}};

struct RenderTiming {
    double total_ns = 0.0;
    double trigger_ns = 0.0;
    double process_ns = 0.0;
    double checksum = 0.0;
    std::size_t frames = 0;
    bool active_at_end = false;
};

struct Summary {
    double median_total_ns_per_frame = 0.0;
    double p95_total_ns_per_frame = 0.0;
    double median_process_ns_per_frame = 0.0;
    double p95_process_ns_per_frame = 0.0;
    double median_trigger_ns_per_hit = 0.0;
    double p95_trigger_ns_per_hit = 0.0;
    double checksum = 0.0;
    std::size_t frames_per_hit = 0;
    bool active_at_end = false;
    bool valid = false;
};

struct QualityMetrics {
    double peak = 0.0;
    double rms = 0.0;
    double mean = 0.0;
    std::array<double, 7> window_rms{};
    std::array<double, 9> spectral_probe{};
    std::uint64_t hash = 1469598103934665603ull;
    std::size_t allocations = 0;
    bool finite = true;
    bool repeat_equal = false;
};

std::unique_ptr<Voice> make_voice(const Scenario& scenario, double sample_rate) {
    auto voice = pulp::signal::drum::create_engine(scenario.engine);
    if (!voice) return nullptr;
#if PULP_SIGNAL_FAST_LADDER_TANH
    if (auto* tom = dynamic_cast<TomVoice*>(voice.get()))
        if (!tom->set_ladder_math_profile(
                TomVoice::LadderMathProfile::realtime_efficient))
            return nullptr;
#endif
    voice->prepare(sample_rate);
    if (scenario.shaped_output) {
        if (auto* stage = voice->output_stage()) {
            stage->set_fold(0.65);
            stage->set_drive(0.75);
        }
    }
    return voice;
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
void run_trigonometry_control(std::size_t frames, double& state) {
    double phase = state;
    double sum = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        phase += 0.00017320508075688773;
        if (phase >= 1.0) phase -= 1.0;
        const double angle = phase * 6.28318530717958647692;
        sum += std::sin(angle);
        sum += std::sin(angle * 1.37);
        sum += std::sin(angle * 1.91);
        sum += std::sin(angle * 2.53);
    }
    state = phase + sum * 1.0e-30;
}

RenderTiming render_hit(Voice& voice, int block_size, std::vector<float>& output,
                        bool negative_control) {
    std::fill(output.begin(), output.end(), 0.0f);
    voice.reset();

    const auto total_begin = std::chrono::steady_clock::now();
    voice.note_on(kVelocity);
    const auto process_begin = std::chrono::steady_clock::now();
    double control_state = 0.125;
    std::size_t rendered_frames = 0;
    while (rendered_frames < output.size() && voice.is_active()) {
        const int count = std::min<int>(block_size,
                                        static_cast<int>(output.size() - rendered_frames));
        voice.process(output.data() + rendered_frames, count);
        if (negative_control)
            run_trigonometry_control(static_cast<std::size_t>(count), control_state);
        rendered_frames += static_cast<std::size_t>(count);
    }
    const auto end = std::chrono::steady_clock::now();

    double checksum = control_state * 1.0e-12;
    for (std::size_t frame = 0; frame < rendered_frames; ++frame)
        checksum += static_cast<double>(output[frame]) * static_cast<double>(frame + 1);
    g_sink = g_sink + checksum;

    return {
        std::chrono::duration<double, std::nano>(end - total_begin).count(),
        std::chrono::duration<double, std::nano>(process_begin - total_begin).count(),
        std::chrono::duration<double, std::nano>(end - process_begin).count(),
        checksum,
        rendered_frames,
        voice.is_active(),
    };
}

double quantile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::floor(fraction * static_cast<double>(values.size() - 1)));
    return values[index];
}

Summary measure(const Scenario& scenario, double sample_rate, int block_size,
                bool negative_control) {
    auto voice = make_voice(scenario, sample_rate);
    if (!voice) return {};
    const auto max_frames = static_cast<std::size_t>(
        std::ceil(kMaxHitSeconds * sample_rate));
    std::vector<float> output(max_frames);
    std::vector<double> total;
    std::vector<double> process;
    std::vector<double> trigger;
    total.reserve(kTrials);
    process.reserve(kTrials);
    trigger.reserve(kTrials);
    double checksum = 0.0;
    bool active_at_end = false;
    std::size_t frames_per_hit = 0;

    for (int trial = 0; trial < kTrials; ++trial) {
        double trial_total = 0.0;
        double trial_process = 0.0;
        double trial_trigger = 0.0;
        std::size_t trial_frames = 0;
        for (int pass = 0; pass < kPasses; ++pass) {
            const auto timing = render_hit(*voice, block_size, output, negative_control);
            trial_total += timing.total_ns;
            trial_process += timing.process_ns;
            trial_trigger += timing.trigger_ns;
            trial_frames += timing.frames;
            checksum += timing.checksum;
            active_at_end = active_at_end || timing.active_at_end;
            frames_per_hit = std::max(frames_per_hit, timing.frames);
        }
        if (trial_frames == 0) return {};
        total.push_back(trial_total / static_cast<double>(trial_frames));
        process.push_back(trial_process / static_cast<double>(trial_frames));
        trigger.push_back(trial_trigger / kPasses);
    }

    const bool valid = frames_per_hit > 0 &&
        std::all_of(total.begin(), total.end(), [](double value) {
            return std::isfinite(value) && value > 0.0;
        }) &&
        std::all_of(process.begin(), process.end(), [](double value) {
            return std::isfinite(value) && value >= 0.0;
        }) &&
        std::all_of(trigger.begin(), trigger.end(), [](double value) {
            return std::isfinite(value) && value >= 0.0;
        });
    return {quantile(total, 0.5), quantile(total, 0.95),
            quantile(process, 0.5), quantile(process, 0.95),
            quantile(trigger, 0.5), quantile(trigger, 0.95),
            checksum, frames_per_hit, active_at_end, valid};
}

const Scenario* find_scenario(std::string_view name) {
    const auto found = std::find_if(kScenarios.begin(), kScenarios.end(),
                                    [name](const Scenario& scenario) {
                                        return scenario.name == name;
                                    });
    return found == kScenarios.end() ? nullptr : &*found;
}

void print_header(std::string_view mode) {
    std::cout << std::setprecision(12)
              << "{\"schema\":\"pulp.drum-whole-voice-benchmark.v2\""
              << ",\"mode\":\"" << mode << "\""
              << ",\"max_hit_seconds\":" << kMaxHitSeconds
              << ",\"trials\":" << kTrials
              << ",\"passes\":" << kPasses
              << ",\"compiler\":\"" << __VERSION__ << "\""
#if defined(NDEBUG)
              << ",\"ndebug\":true"
#else
              << ",\"ndebug\":false"
#endif
#if PULP_SIGNAL_FAST_LADDER_TANH
              << ",\"fast_ladder_tanh\":true"
#else
              << ",\"fast_ladder_tanh\":false"
#endif
              << "}\n";
}

void print_result(const Scenario& scenario, double sample_rate, int block_size,
                  const Summary& summary, bool negative_control) {
    std::cout << "{\"scenario\":\"" << scenario.name
              << "\",\"sample_rate\":" << sample_rate
              << ",\"block_size\":" << block_size
              << ",\"negative_control\":"
              << (negative_control ? "true" : "false")
              << ",\"median_total_ns_per_frame\":"
              << summary.median_total_ns_per_frame
              << ",\"p95_total_ns_per_frame\":" << summary.p95_total_ns_per_frame
              << ",\"median_process_ns_per_frame\":"
              << summary.median_process_ns_per_frame
              << ",\"p95_process_ns_per_frame\":" << summary.p95_process_ns_per_frame
              << ",\"median_trigger_ns_per_hit\":"
              << summary.median_trigger_ns_per_hit
              << ",\"p95_trigger_ns_per_hit\":" << summary.p95_trigger_ns_per_hit
              << ",\"checksum\":" << summary.checksum
              << ",\"frames_per_hit\":" << summary.frames_per_hit
              << ",\"active_at_end\":"
              << (summary.active_at_end ? "true" : "false")
              << ",\"valid\":" << (summary.valid ? "true" : "false") << "}\n";
}

int run_matrix() {
    print_header("matrix");
    bool complete = true;
    for (const auto& scenario : kScenarios) {
        for (double sample_rate : {44100.0, 48000.0, 96000.0}) {
            for (int block_size : {32, 64, 128, 512}) {
                const auto summary = measure(scenario, sample_rate, block_size, false);
                print_result(scenario, sample_rate, block_size, summary, false);
                complete = complete && summary.valid && !summary.active_at_end;
            }
        }
    }
    return complete && std::isfinite(g_sink) ? 0 : 1;
}

int run_scenario_matrix(const Scenario& scenario) {
    print_header("scenario-matrix");
    bool complete = true;
    for (double sample_rate : {44100.0, 48000.0, 96000.0}) {
        for (int block_size : {32, 64, 128, 512}) {
            const auto summary = measure(scenario, sample_rate, block_size, false);
            print_result(scenario, sample_rate, block_size, summary, false);
            complete = complete && summary.valid && !summary.active_at_end;
        }
    }
    return complete && std::isfinite(g_sink) ? 0 : 1;
}

std::uint64_t hash_samples(const std::vector<float>& samples) {
    std::uint64_t hash = 1469598103934665603ull;
    for (float sample : samples) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= (bits >> (8 * byte)) & 0xffu;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

std::vector<float> render_quality_hit(TomVoice& voice, double sample_rate,
                                      int block_size, float velocity,
                                      std::size_t& allocations) {
    const auto frames = static_cast<std::size_t>(std::ceil(0.75 * sample_rate));
    std::vector<float> output(frames, 0.0f);
    voice.reset();
    allocation_probe::count = 0;
    allocation_probe::enabled = true;
    voice.note_on(velocity);
    for (std::size_t offset = 0; offset < frames;) {
        const int count = std::min<int>(block_size,
                                       static_cast<int>(frames - offset));
        voice.process(output.data() + offset, count);
        offset += static_cast<std::size_t>(count);
    }
    allocation_probe::enabled = false;
    allocations = allocation_probe::count;
    return output;
}

QualityMetrics quality_metrics(const std::vector<float>& samples,
                               double sample_rate) {
    QualityMetrics result;
    double sum = 0.0;
    double sum_squared = 0.0;
    for (float sample : samples) {
        result.finite = result.finite && std::isfinite(sample);
        result.peak = std::max(result.peak, std::fabs(static_cast<double>(sample)));
        sum += sample;
        sum_squared += static_cast<double>(sample) * sample;
    }
    result.mean = sum / static_cast<double>(samples.size());
    result.rms = std::sqrt(sum_squared / static_cast<double>(samples.size()));
    result.hash = hash_samples(samples);

    constexpr std::array<double, 8> kWindowEdges{
        0.0, 0.005, 0.020, 0.050, 0.100, 0.250, 0.500, 0.750};
    for (std::size_t window = 0; window < result.window_rms.size(); ++window) {
        const auto begin = static_cast<std::size_t>(kWindowEdges[window] * sample_rate);
        const auto end = std::min(samples.size(), static_cast<std::size_t>(
            kWindowEdges[window + 1] * sample_rate));
        double energy = 0.0;
        for (std::size_t frame = begin; frame < end; ++frame)
            energy += static_cast<double>(samples[frame]) * samples[frame];
        result.window_rms[window] =
            std::sqrt(energy / static_cast<double>(std::max<std::size_t>(1, end - begin)));
    }

    constexpr std::array<double, 9> kProbeHz{
        80.0, 120.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12000.0};
    for (std::size_t probe = 0; probe < kProbeHz.size(); ++probe) {
        if (kProbeHz[probe] >= 0.49 * sample_rate) continue;
        const double omega = 2.0 * 3.14159265358979323846 *
                             kProbeHz[probe] / sample_rate;
        const double coefficient = 2.0 * std::cos(omega);
        double previous = 0.0;
        double previous_two = 0.0;
        for (float sample : samples) {
            const double current = sample + coefficient * previous - previous_two;
            previous_two = previous;
            previous = current;
        }
        const double power = previous_two * previous_two + previous * previous -
                             coefficient * previous * previous_two;
        result.spectral_probe[probe] =
            std::sqrt(std::max(0.0, power)) / static_cast<double>(samples.size());
    }
    return result;
}

std::string_view preset_name(TomVoice::Preset preset) {
    for (const auto& data : TomVoice::presets)
        if (data.id == preset) return data.name;
    return "unknown";
}

int run_quality_matrix() {
    print_header("quality-matrix");
    bool valid = true;
    for (const auto& preset_data : TomVoice::presets) {
        for (double sample_rate : {44100.0, 48000.0, 96000.0}) {
            for (int block_size : {32, 512}) {
                for (float velocity : {0.2f, 0.82f, 1.0f}) {
                    auto base = pulp::signal::drum::create_engine(EngineId::tom_generic);
                    auto* voice = dynamic_cast<TomVoice*>(base.get());
                    if (!voice) return 2;
#if PULP_SIGNAL_FAST_LADDER_TANH
                    if (!voice->set_ladder_math_profile(
                            TomVoice::LadderMathProfile::realtime_efficient))
                        return 2;
#endif
                    voice->apply_preset(preset_data.id);
                    voice->prepare(sample_rate);
                    std::size_t allocations = 0;
                    const auto first = render_quality_hit(
                        *voice, sample_rate, block_size, velocity, allocations);
                    std::size_t repeat_allocations = 0;
                    const auto repeat = render_quality_hit(
                        *voice, sample_rate, block_size, velocity, repeat_allocations);
                    auto metrics = quality_metrics(first, sample_rate);
                    metrics.allocations = allocations + repeat_allocations;
                    metrics.repeat_equal = first == repeat;
                    valid = valid && metrics.finite && metrics.allocations == 0 &&
                            metrics.repeat_equal;
                    std::cout << "{\"preset\":\"" << preset_name(preset_data.id)
                              << "\",\"sample_rate\":" << sample_rate
                              << ",\"block_size\":" << block_size
                              << ",\"velocity\":" << velocity
                              << ",\"peak\":" << metrics.peak
                              << ",\"rms\":" << metrics.rms
                              << ",\"mean\":" << metrics.mean
                              << ",\"hash\":" << metrics.hash
                              << ",\"allocations\":" << metrics.allocations
                              << ",\"finite\":" << (metrics.finite ? "true" : "false")
                              << ",\"repeat_equal\":"
                              << (metrics.repeat_equal ? "true" : "false")
                              << ",\"window_rms\":[";
                    for (std::size_t i = 0; i < metrics.window_rms.size(); ++i) {
                        if (i) std::cout << ',';
                        std::cout << metrics.window_rms[i];
                    }
                    std::cout << "],\"spectral_probe\":[";
                    for (std::size_t i = 0; i < metrics.spectral_probe.size(); ++i) {
                        if (i) std::cout << ',';
                        std::cout << metrics.spectral_probe[i];
                    }
                    std::cout << "]}\n";
                }
            }
        }
    }
    return valid && std::isfinite(g_sink) ? 0 : 1;
}

int run_control() {
    constexpr double sample_rate = 48000.0;
    constexpr int block_size = 128;
    const auto& scenario = kScenarios.front();
    print_header("negative-control");
    const auto reference = measure(scenario, sample_rate, block_size, false);
    const auto control = measure(scenario, sample_rate, block_size, true);
    print_result(scenario, sample_rate, block_size, reference, false);
    print_result(scenario, sample_rate, block_size, control, true);
    const double slowdown = 100.0 *
        (control.median_total_ns_per_frame - reference.median_total_ns_per_frame) /
        reference.median_total_ns_per_frame;
    std::cout << "{\"control_slowdown_percent\":" << slowdown
              << ",\"control_symbol\":\"run_trigonometry_control\"}\n";
    return reference.valid && control.valid && !reference.active_at_end &&
           !control.active_at_end && slowdown > 10.0 &&
           std::isfinite(g_sink) ? 0 : 1;
}

int run_profile(const Scenario& scenario, double seconds, bool negative_control,
                int block_size) {
    auto voice = make_voice(scenario, 48000.0);
    if (!voice) return 2;
    std::vector<float> output(static_cast<std::size_t>(
        std::ceil(kMaxHitSeconds * 48000.0)));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(seconds);
    std::size_t hits = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        render_hit(*voice, block_size, output, negative_control);
        ++hits;
    }
    print_header("profile");
    std::cout << "{\"scenario\":\"" << scenario.name
              << "\",\"seconds\":" << seconds
              << ",\"block_size\":" << block_size
              << ",\"negative_control\":"
              << (negative_control ? "true" : "false")
              << ",\"hits\":" << hits << "}\n";
    return hits > 0 && std::isfinite(g_sink) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
#if !defined(NDEBUG)
    std::cerr << "error: configure with -DCMAKE_BUILD_TYPE=Release\n";
    return 2;
#endif
    if (argc == 1 || std::strcmp(argv[1], "--matrix") == 0)
        return run_matrix();
    if (std::strcmp(argv[1], "--negative-control") == 0)
        return run_control();
    if (std::strcmp(argv[1], "--quality-matrix") == 0)
        return run_quality_matrix();
    if (std::strcmp(argv[1], "--list") == 0) {
        for (const auto& scenario : kScenarios) std::cout << scenario.name << '\n';
        return 0;
    }
    if (std::strcmp(argv[1], "--matrix-scenario") == 0 && argc == 3) {
        const auto* scenario = find_scenario(argv[2]);
        if (!scenario) {
            std::cerr << "error: unknown scenario\n";
            return 2;
        }
        return run_scenario_matrix(*scenario);
    }
    if (std::strcmp(argv[1], "--profile") == 0 && argc >= 3) {
        const auto* scenario = find_scenario(argv[2]);
        if (!scenario) {
            std::cerr << "error: unknown scenario\n";
            return 2;
        }
        double seconds = 10.0;
        int block_size = 128;
        bool negative_control = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--negative-control") == 0) {
                negative_control = true;
            } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
                seconds = std::max(std::strtod(argv[++i], nullptr), 0.1);
            } else if (std::strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) {
                block_size = std::clamp(std::atoi(argv[++i]), 1, 4096);
            } else {
                std::cerr << "error: invalid profile argument\n";
                return 2;
            }
        }
        return run_profile(*scenario, seconds, negative_control, block_size);
    }
    std::cerr << "usage: pulp-drum-whole-voice-benchmark "
                 "[--matrix|--matrix-scenario NAME|--quality-matrix|"
                 "--negative-control|--list|"
                 "--profile NAME "
                 "[--seconds N] [--block-size N] [--negative-control]]\n";
    return 2;
}

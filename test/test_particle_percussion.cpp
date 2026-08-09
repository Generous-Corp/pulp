#include <catch2/catch_approx.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/particle_collision_exciter.hpp>
#include <pulp/signal/particle_percussion_voice.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

using pulp::signal::ParticleCollisionExciter64;
using pulp::signal::ParticlePercussionVoice64;

constexpr double kSampleRate = 48000.0;
constexpr double kDecayMs = 4000.0;
constexpr double kCollisionLoss = 0.0001;
constexpr double kSustainFloor = 0.0;

double decay_coefficient(double sample_rate, double decay_ms) {
    return std::exp(-1.0 / (sample_rate * decay_ms * 0.001));
}

double collision_probability(double sample_rate, double event_rate,
                             int particle_count, double energy) {
    const double lambda = event_rate * std::sqrt(static_cast<double>(particle_count)) * energy;
    return -std::expm1(-lambda / sample_rate);
}

struct EventSample {
    bool occurred = false;
    double amplitude = 0.0;

    friend bool operator==(const EventSample&, const EventSample&) = default;
};

std::vector<EventSample> render_events(ParticleCollisionExciter64& exciter,
                                       std::size_t frames) {
    std::vector<EventSample> events;
    events.reserve(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto event = exciter.next();
        events.push_back({event.occurred, event.amplitude});
    }
    return events;
}

void configure_exciter(ParticleCollisionExciter64& exciter, std::uint64_t seed,
                       int particles, double event_rate) {
    exciter.prepare(kSampleRate);
    exciter.set_seed(seed);
    exciter.set_particle_count(particles);
    exciter.set_event_rate(event_rate);
    exciter.set_energy_decay(kDecayMs);
    exciter.set_collision_energy_loss(kCollisionLoss);
    exciter.set_sustain_floor(kSustainFloor);
    exciter.reset();
    exciter.excite(1.0);
}

using Model = ParticlePercussionVoice64::Model;

constexpr std::array<Model, 4> kModels{
    Model::shaker,
    Model::maraca,
    Model::rattle,
    Model::tambourine,
};

void configure_voice(ParticlePercussionVoice64& voice, Model model,
                     std::uint64_t seed = 0x71f2c3d4e5a69788ULL) {
    voice.prepare(kSampleRate);
    voice.set_model(model);
    voice.set_seed(seed);
    voice.set_particle_count(2000);
    voice.set_event_rate(20000.0);
    voice.set_energy_decay(250.0);
    voice.reset();
    voice.excite(1.0);
}

std::vector<double> render_voice(Model model, std::size_t frames,
                                 std::uint64_t seed = 0x71f2c3d4e5a69788ULL) {
    ParticlePercussionVoice64 voice;
    configure_voice(voice, model, seed);
    std::vector<double> output(frames, 0.0);
    voice.process(output.data(), static_cast<int>(output.size()));
    return output;
}

double peak_magnitude(const std::vector<double>& values) {
    double peak = 0.0;
    for (double value : values) peak = std::max(peak, std::fabs(value));
    return peak;
}

// Test-only copies of the published splitmix64/xorshift32 algorithms. These
// deliberately do not call Pulp's RNG helpers: matching the production event,
// jitter, and routing streams therefore proves both purpose separation and
// consumption order.
constexpr std::uint64_t reference_mix64(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value;
}

constexpr std::uint32_t reference_purpose_seed(std::uint64_t seed,
                                               std::uint64_t purpose) {
    const auto mixed = reference_mix64(
        seed + purpose * 0xBF58476D1CE4E5B9ULL);
    const auto folded = static_cast<std::uint32_t>(mixed ^ (mixed >> 32));
    return folded == 0U ? 0x1D872B41U : folded;
}

template <typename SampleType>
class ReferenceNoise {
public:
    explicit ReferenceNoise(std::uint32_t seed) : state_(seed) {}

    SampleType white() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        const double white = static_cast<double>(state_) *
                                 (1.0 / 2147483648.0) -
                             1.0;
        return static_cast<SampleType>(white);
    }

    double unit() {
        return 0.5 * (static_cast<double>(white()) + 1.0);
    }

private:
    std::uint32_t state_;
};

template <typename SampleType>
struct ReferenceCollisionEvent {
    bool occurred = false;
    SampleType amplitude{};
    double pre_loss_energy = 0.0;
};

template <typename SampleType>
class ReferenceCollisionExciter {
public:
    ReferenceCollisionExciter(std::uint64_t seed, int particles,
                              double event_rate, double decay_ms,
                              double collision_loss, double sustain_floor,
                              double sample_rate = kSampleRate)
        : event_rng_(reference_purpose_seed(seed, 0x4556454E54ULL)),
          jitter_rng_(reference_purpose_seed(seed, 0x4A4954544552ULL)),
          particle_count_(particles),
          event_rate_(event_rate),
          collision_loss_(collision_loss),
          sustain_floor_(sustain_floor),
          sample_rate_(sample_rate),
          decay_(decay_coefficient(sample_rate, decay_ms)) {}

    void excite(double amount) {
        if (std::isfinite(amount) && amount > 0.0)
            energy_ = std::clamp(energy_ + amount, 0.0, 1.0);
    }

    ReferenceCollisionEvent<SampleType> next() {
        energy_ *= decay_;
        snap();
        const double probability = collision_probability(
            sample_rate_, event_rate_, particle_count_, energy_);
        if (!(event_rng_.unit() < probability))
            return {};

        const double pre_loss = energy_;
        const double amplitude = pre_loss * (0.5 + 0.5 * jitter_rng_.unit());
        SampleType converted_amplitude = static_cast<SampleType>(amplitude);
        if (!(static_cast<double>(converted_amplitude) < pre_loss)) {
            converted_amplitude = std::nextafter(
                static_cast<SampleType>(pre_loss),
                std::numeric_limits<SampleType>::lowest());
        }
        energy_ = std::max(0.0, energy_ - collision_loss_);
        snap();
        return {true, converted_amplitude, pre_loss};
    }

private:
    void snap() {
        if (energy_ < sustain_floor_)
            energy_ = 0.0;
    }

    ReferenceNoise<double> event_rng_;
    ReferenceNoise<double> jitter_rng_;
    int particle_count_;
    double event_rate_;
    double collision_loss_;
    double sustain_floor_;
    double sample_rate_;
    double decay_;
    double energy_ = 0.0;
};

struct ReferenceBand {
    double frequency_hz;
    double q;
    double gain;
    double weight;
};

constexpr std::array<ReferenceBand, 4> kReferenceShaker{{
    {2200.0, 1.5, 1.0, 0.35},
    {4200.0, 2.0, 0.8, 0.30},
    {6800.0, 1.8, 0.6, 0.20},
    {9500.0, 1.2, 0.4, 0.15},
}};
constexpr std::array<ReferenceBand, 4> kReferenceMaraca{{
    {2900.0, 3.0, 1.0, 0.40},
    {5100.0, 2.5, 0.5, 0.25},
    {7600.0, 2.0, 0.3, 0.20},
    {10400.0, 1.5, 0.2, 0.15},
}};
constexpr std::array<ReferenceBand, 4> kReferenceRattle{{
    {550.0, 4.0, 1.0, 0.30},
    {950.0, 3.5, 0.7, 0.30},
    {1600.0, 3.0, 0.5, 0.25},
    {2600.0, 2.5, 0.3, 0.15},
}};

struct ReferenceMode {
    float frequency_hz;
    float t60_seconds;
    float gain;
};

constexpr std::array<ReferenceMode, 5> kReferenceTambourine{{
    {3200.0f, 0.180f, 1.0f},
    {7584.0f, 0.140f, 0.7f},
    {12416.0f, 0.100f, 0.5f},
    {17952.0f, 0.070f, 0.35f},
    {23328.0f, 0.045f, 0.2f},
}};

template <typename SampleType>
class ReferenceTwoPole {
public:
    ReferenceTwoPole(const ReferenceBand& band, bool wrong_q_factor,
                     double sample_rate) {
        constexpr double pi = 3.14159265358979323846;
        const double frequency = std::clamp(band.frequency_hz, 1.0,
                                            0.499 * sample_rate);
        const double t60_ms = wrong_q_factor
                                  ? 1000.0 * band.q / frequency
                                  : 1000.0 * 3.0 * std::log(10.0) * band.q /
                                        (pi * frequency);
        const double samples = std::max(0.001 * t60_ms * sample_rate, 1.0);
        const double radius = std::pow(10.0, -3.0 / samples);
        const double theta = 2.0 * pi * frequency / sample_rate;
        a1_ = static_cast<SampleType>(2.0 * radius * std::cos(theta));
        a2_ = static_cast<SampleType>(radius * radius);
        gain_ = static_cast<SampleType>(std::max(std::sin(theta), 1e-4));
    }

    SampleType process(SampleType input) {
        const SampleType output = a1_ * y1_ - a2_ * y2_ + gain_ * input;
        y2_ = y1_;
        y1_ = snap(output);
        return output;
    }

private:
    static SampleType snap(SampleType value) {
        return value < SampleType{1e-15} && value > SampleType{-1e-15}
                   ? SampleType{}
                   : value;
    }

    SampleType a1_{};
    SampleType a2_{};
    SampleType gain_{};
    SampleType y1_{};
    SampleType y2_{};
};

template <typename SampleType>
class ReferenceModalBank {
public:
    explicit ReferenceModalBank(bool one_mode_only, double sample_rate)
        : mode_count_(one_mode_only ? 1U : kReferenceTambourine.size()) {
        constexpr double pi = 3.14159265358979323846;
        for (std::size_t index = 0; index < mode_count_; ++index) {
            const auto& mode = kReferenceTambourine[index];
            const double radius = std::pow(
                10.0, -3.0 / (mode.t60_seconds * sample_rate));
            const double frequency = std::clamp(
                static_cast<double>(mode.frequency_hz), 1.0, 0.49 * sample_rate);
            const double angle = 2.0 * pi * frequency / sample_rate;
            rc_[index] = static_cast<SampleType>(radius * std::cos(angle));
            rs_[index] = static_cast<SampleType>(radius * std::sin(angle));
            cu_[index] = static_cast<SampleType>(mode.gain * std::cos(angle));
            cv_[index] = static_cast<SampleType>(mode.gain * std::sin(angle));
        }
    }

    SampleType process(SampleType input) {
        std::array<SampleType, 16> outputs{};
        for (std::size_t index = 0; index < mode_count_; ++index) {
            const SampleType next_u = rc_[index] * u_[index] -
                                      rs_[index] * v_[index] +
                                      cu_[index] * input;
            const SampleType next_v = rs_[index] * u_[index] +
                                      rc_[index] * v_[index] +
                                      cv_[index] * input;
            u_[index] = snap(next_u);
            v_[index] = snap(next_v);
            outputs[index] = next_v;
        }
        for (int step = 8; step > 0; step /= 2)
            for (int index = 0; index < step; ++index)
                outputs[static_cast<std::size_t>(index)] +=
                    outputs[static_cast<std::size_t>(index + step)];
        return outputs[0];
    }

private:
    static SampleType snap(SampleType value) {
        return value < SampleType{1e-15} && value > SampleType{-1e-15}
                   ? SampleType{}
                   : value;
    }

    std::array<SampleType, 5> rc_{};
    std::array<SampleType, 5> rs_{};
    std::array<SampleType, 5> cu_{};
    std::array<SampleType, 5> cv_{};
    std::array<SampleType, 5> u_{};
    std::array<SampleType, 5> v_{};
    std::size_t mode_count_;
};

struct ReferenceMutation {
    bool wrong_q_factor = false;
    bool route_every_event_to_band_zero = false;
    bool one_tambourine_mode = false;
};

template <typename SampleType>
class ReferenceVoice {
public:
    using Voice = pulp::signal::ParticlePercussionVoiceT<SampleType>;
    using ModelType = typename Voice::Model;

    ReferenceVoice(ModelType model, std::uint64_t seed, double sample_rate,
                   ReferenceMutation mutation = {})
        : model_(model),
          collision_(seed, 2000, 20000.0, 250.0, 0.004, 0.02, sample_rate),
          routing_(reference_purpose_seed(seed, 0x524F5554494E47ULL)),
          bands_(bands_for(model)),
          resonators_{ReferenceTwoPole<SampleType>(bands_[0], mutation.wrong_q_factor,
                                                   sample_rate),
                      ReferenceTwoPole<SampleType>(bands_[1], mutation.wrong_q_factor,
                                                   sample_rate),
                      ReferenceTwoPole<SampleType>(bands_[2], mutation.wrong_q_factor,
                                                   sample_rate),
                      ReferenceTwoPole<SampleType>(bands_[3], mutation.wrong_q_factor,
                                                   sample_rate)},
          modal_(mutation.one_tambourine_mode, sample_rate),
          route_zero_(mutation.route_every_event_to_band_zero) {
        collision_.excite(1.0);
    }

    SampleType process() {
        const auto event = collision_.next();
        if (model_ == ModelType::tambourine)
            return modal_.process(event.occurred ? event.amplitude : SampleType{});

        std::size_t selected = bands_.size();
        if (event.occurred)
            selected = route_zero_ ? 0U : select_band();
        SampleType output{};
        for (std::size_t band = 0; band < bands_.size(); ++band) {
            const auto input = selected == band
                                   ? static_cast<SampleType>(
                                         event.amplitude *
                                         static_cast<SampleType>(bands_[band].gain))
                                   : SampleType{};
            output += resonators_[band].process(input);
        }
        return output;
    }

private:
    static std::array<ReferenceBand, 4> bands_for(ModelType model) {
        switch (model) {
        case ModelType::shaker: return kReferenceShaker;
        case ModelType::maraca: return kReferenceMaraca;
        case ModelType::rattle: return kReferenceRattle;
        case ModelType::tambourine: return kReferenceShaker;
        }
        return kReferenceShaker;
    }

    std::size_t select_band() {
        const double draw = routing_.unit();
        double cumulative = 0.0;
        for (std::size_t band = 0; band + 1 < bands_.size(); ++band) {
            cumulative += bands_[band].weight;
            if (draw < cumulative)
                return band;
        }
        return bands_.size() - 1;
    }

    ModelType model_;
    ReferenceCollisionExciter<SampleType> collision_;
    ReferenceNoise<double> routing_;
    std::array<ReferenceBand, 4> bands_;
    std::array<ReferenceTwoPole<SampleType>, 4> resonators_;
    ReferenceModalBank<SampleType> modal_;
    bool route_zero_;
};

template <typename SampleType>
double sample_difference(SampleType lhs, SampleType rhs) {
    return std::fabs(static_cast<double>(lhs) - static_cast<double>(rhs));
}

}  // namespace

TEST_CASE("Particle collisions follow a depletion-aware conditional intensity",
          "[signal][particle-percussion][statistics]") {
    constexpr std::array<std::uint64_t, 8> seeds{
        0x1020304050607080ULL, 0x91a2b3c4d5e6f708ULL,
        0x22446688aaccee11ULL, 0xfedcba9876543210ULL,
        0x0f1e2d3c4b5a6978ULL, 0x8877665544332211ULL,
        0x13579bdf2468ace0ULL, 0xc001d00d5eed1234ULL,
    };
    constexpr std::size_t frames_per_seed = 24000;
    constexpr double event_rate = 60.0;
    const double decay = decay_coefficient(kSampleRate, kDecayMs);

    for (int particles : {1, 64, 2000}) {
        double residual = 0.0;
        double variance = 0.0;

        for (std::uint64_t seed : seeds) {
            ParticleCollisionExciter64 exciter;
            configure_exciter(exciter, seed, particles, event_rate);
            double external_energy = 1.0;
            double maximum_energy_error = 0.0;

            for (std::size_t frame = 0; frame < frames_per_seed; ++frame) {
                external_energy *= decay;
                if (external_energy < kSustainFloor) external_energy = 0.0;

                const double probability = collision_probability(
                    kSampleRate, event_rate, particles, external_energy);
                const auto event = exciter.next();
                residual += (event.occurred ? 1.0 : 0.0) - probability;
                variance += probability * (1.0 - probability);

                if (event.occurred) {
                    external_energy = std::max(0.0, external_energy - kCollisionLoss);
                    if (external_energy < kSustainFloor) external_energy = 0.0;
                }

                maximum_energy_error = std::max(
                    maximum_energy_error, std::fabs(exciter.energy() - external_energy));
            }

            INFO("particles=" << particles << " seed=" << seed);
            REQUIRE(maximum_energy_error < 2e-12);
        }

        INFO("particles=" << particles << " residual=" << residual
                           << " variance=" << variance);
        REQUIRE(variance > 0.0);
        REQUIRE(std::fabs(residual) < 6.0 * std::sqrt(variance));
    }
}

TEST_CASE("Collision amplitude jitter is bounded and centered independently",
          "[signal][particle-percussion][statistics]") {
    ParticleCollisionExciter64 exciter;
    configure_exciter(exciter, 0xd15ea5e5ULL, 2000, 20000.0);

    constexpr std::size_t wanted_events = 12000;
    double ratio_sum = 0.0;
    double minimum_ratio = 1.0;
    double maximum_ratio = 0.0;
    std::size_t event_count = 0;
    for (std::size_t frame = 0; frame < 13000 && event_count < wanted_events; ++frame) {
        exciter.excite(1.0);
        const double pre_loss_energy = decay_coefficient(kSampleRate, kDecayMs);
        const auto event = exciter.next();
        if (!event.occurred) continue;

        const double ratio = event.amplitude / pre_loss_energy;
        minimum_ratio = std::min(minimum_ratio, ratio);
        maximum_ratio = std::max(maximum_ratio, ratio);
        ratio_sum += ratio;
        ++event_count;
    }

    REQUIRE(event_count == wanted_events);
    REQUIRE(minimum_ratio >= 0.5);
    REQUIRE(maximum_ratio < 1.0);
    REQUIRE(ratio_sum / static_cast<double>(event_count) == Catch::Approx(0.75).margin(0.01));
}

TEMPLATE_TEST_CASE("Collision amplitude remains strictly below pre-loss energy after conversion",
                   "[signal][particle-percussion][amplitude][precision]", float,
                   double) {
    using Exciter = pulp::signal::ParticleCollisionExciterT<TestType>;
    constexpr std::uint64_t regression_seed = 101383190ULL;

    Exciter exciter;
    exciter.prepare(kSampleRate);
    exciter.set_seed(regression_seed);
    exciter.set_particle_count(2000);
    exciter.set_event_rate(static_cast<TestType>(20000.0));
    exciter.set_energy_decay(static_cast<TestType>(kDecayMs));
    exciter.set_collision_energy_loss(static_cast<TestType>(kCollisionLoss));
    exciter.set_sustain_floor(static_cast<TestType>(0.0));
    exciter.excite(static_cast<TestType>(1.0));

    const double pre_loss_energy = decay_coefficient(kSampleRate, kDecayMs);
    const auto event = exciter.next();
    REQUIRE(event.occurred);
    REQUIRE(static_cast<double>(event.amplitude) < pre_loss_energy);
}

TEMPLATE_TEST_CASE("Collision event and jitter streams match independent RNG math",
                   "[signal][particle-percussion][rng][precision]", float, double) {
    using Exciter = pulp::signal::ParticleCollisionExciterT<TestType>;
    constexpr std::uint64_t seed = 0x6b8b4567327b23c6ULL;
    constexpr int particles = 2000;
    constexpr double event_rate = 4000.0;
    constexpr double decay_ms = 1000.0;
    constexpr double collision_loss = 0.0001;
    constexpr double sustain_floor = 0.0;

    Exciter actual;
    actual.prepare(kSampleRate);
    actual.set_seed(seed);
    actual.set_particle_count(particles);
    actual.set_event_rate(static_cast<TestType>(event_rate));
    actual.set_energy_decay(static_cast<TestType>(decay_ms));
    actual.set_collision_energy_loss(static_cast<TestType>(collision_loss));
    actual.set_sustain_floor(static_cast<TestType>(sustain_floor));
    actual.reset();

    ReferenceCollisionExciter<TestType> expected(
        seed, particles, event_rate, decay_ms, collision_loss, sustain_floor);

    // Negative control: this deliberately wrong model consumes event and
    // jitter draws from one stream. It must diverge once the first event shifts
    // subsequent event draws.
    ReferenceNoise<double> shared_rng(
        reference_purpose_seed(seed, 0x4556454E54ULL));
    double shared_energy = 0.0;
    const double decay = decay_coefficient(kSampleRate, decay_ms);
    std::size_t shared_stream_event_mismatches = 0;
    std::size_t observed_events = 0;

    for (std::size_t frame = 0; frame < 8192; ++frame) {
        if ((frame % 509U) == 0U) {
            actual.excite(static_cast<TestType>(0.35));
            const double rounded_excitation =
                static_cast<double>(static_cast<TestType>(0.35));
            expected.excite(rounded_excitation);
            shared_energy = std::clamp(
                shared_energy + rounded_excitation, 0.0, 1.0);
        }

        const auto observed = actual.next();
        const auto oracle = expected.next();
        INFO("frame=" << frame << " precision_bytes=" << sizeof(TestType));
        REQUIRE(observed.occurred == oracle.occurred);
        REQUIRE(observed.amplitude == oracle.amplitude);
        if (observed.occurred) {
            ++observed_events;
            const double amplitude_ratio =
                static_cast<double>(observed.amplitude) / oracle.pre_loss_energy;
            REQUIRE(amplitude_ratio >= 0.5);
            REQUIRE(amplitude_ratio < 1.0);
        }

        shared_energy *= decay;
        const double shared_probability = collision_probability(
            kSampleRate, event_rate, particles, shared_energy);
        const bool shared_event = shared_rng.unit() < shared_probability;
        if (shared_event) {
            static_cast<void>(shared_rng.unit());
            shared_energy = std::max(0.0, shared_energy - collision_loss);
        }
        if (shared_event != observed.occurred)
            ++shared_stream_event_mismatches;
    }

    REQUIRE(observed_events > 100);
    REQUIRE(shared_stream_event_mismatches > 100);
}

TEST_CASE("One-step collisions use the exact Poisson discretization",
          "[signal][particle-percussion][statistics]") {
    constexpr std::size_t trials = 6000;
    constexpr double event_rate = 20000.0;
    const double first_sample_energy = decay_coefficient(kSampleRate, kDecayMs);
    const double expected_probability = collision_probability(
        kSampleRate, event_rate, 1, first_sample_energy);
    std::size_t collisions = 0;

    ParticleCollisionExciter64 exciter;
    configure_exciter(exciter, 0x9e3779b97f4a7c15ULL, 1, event_rate);
    for (std::size_t trial = 0; trial < trials; ++trial) {
        exciter.set_seed(0x9e3779b97f4a7c15ULL * (trial + 1));
        exciter.reset();
        exciter.excite(1.0);
        collisions += exciter.next().occurred ? 1U : 0U;
    }

    const double expected = expected_probability * static_cast<double>(trials);
    const double sigma = std::sqrt(static_cast<double>(trials) * expected_probability *
                                   (1.0 - expected_probability));
    INFO("observed=" << collisions << " expected=" << expected << " sigma=" << sigma);
    REQUIRE(std::fabs(static_cast<double>(collisions) - expected) < 6.0 * sigma);

    const double linearized = event_rate / kSampleRate * static_cast<double>(trials);
    REQUIRE(std::fabs(static_cast<double>(collisions) - linearized) > 8.0 * sigma);
}

TEST_CASE("Particle collision reset rewinds every random stream",
          "[signal][particle-percussion][determinism]") {
    ParticleCollisionExciter64 exciter;
    configure_exciter(exciter, 0xabcddcba01234567ULL, 64, 1200.0);
    const auto first = render_events(exciter, 8192);

    exciter.reset();
    exciter.excite(1.0);
    REQUIRE(render_events(exciter, first.size()) == first);

    exciter.set_seed(0xabcddcba01234568ULL);
    exciter.reset();
    exciter.excite(1.0);
    REQUIRE(render_events(exciter, first.size()) != first);
}

TEST_CASE("Particle collision validation preserves finite bounded state",
          "[signal][particle-percussion][validation]") {
    ParticleCollisionExciter64 exciter;
    exciter.prepare(kSampleRate);
    exciter.set_particle_count(0);
    exciter.set_event_rate(std::numeric_limits<double>::quiet_NaN());
    exciter.set_energy_decay(std::numeric_limits<double>::infinity());
    exciter.set_collision_energy_loss(-std::numeric_limits<double>::infinity());
    exciter.set_sustain_floor(std::numeric_limits<double>::quiet_NaN());
    exciter.excite(std::numeric_limits<double>::quiet_NaN());

    bool all_finite_and_bounded = true;
    for (int i = 0; i < 4096; ++i) {
        const auto event = exciter.next();
        all_finite_and_bounded = all_finite_and_bounded &&
                                 std::isfinite(exciter.energy()) &&
                                 exciter.energy() >= 0.0 && exciter.energy() <= 1.0 &&
                                 std::isfinite(event.amplitude) &&
                                 event.amplitude >= 0.0 && event.amplitude <= 1.0;
    }
    REQUIRE(all_finite_and_bounded);

    exciter.set_sustain_floor(0.2);
    exciter.set_energy_decay(5.0);
    exciter.reset();
    exciter.excite(0.1);
    exciter.next();
    REQUIRE(exciter.energy() == 0.0);
}

TEST_CASE("A collision depletes energy by an absolute amount",
          "[signal][particle-percussion][energy]") {
    ParticleCollisionExciter64 exciter;
    exciter.prepare(kSampleRate);
    exciter.set_seed(0x123456789abcdef0ULL);
    exciter.set_particle_count(2000);
    exciter.set_event_rate(20000.0);
    exciter.set_energy_decay(kDecayMs);
    exciter.set_collision_energy_loss(0.05);
    exciter.set_sustain_floor(0.0);
    exciter.reset();
    exciter.excite(0.8);

    const double pre_loss = 0.8 * decay_coefficient(kSampleRate, kDecayMs);
    const auto event = exciter.next();
    REQUIRE(event.occurred);
    REQUIRE(event.amplitude <= pre_loss);
    REQUIRE(exciter.energy() == Catch::Approx(pre_loss - 0.05).margin(2e-12));
}

TEST_CASE("Particle percussion scalar and block rendering are identical",
          "[signal][particle-percussion][partition]") {
    constexpr std::size_t frames = 16384;
    for (Model model : kModels) {
        ParticlePercussionVoice64 scalar;
        configure_voice(scalar, model);
        std::vector<double> scalar_output(frames);
        for (double& sample : scalar_output) sample = scalar.process();

        ParticlePercussionVoice64 block;
        configure_voice(block, model);
        std::vector<double> block_output(frames, 0.0);
        block.process(block_output.data(), static_cast<int>(block_output.size()));

        REQUIRE(block_output == scalar_output);
        REQUIRE(block.latency_samples() == 0);
    }
}

TEST_CASE("Every particle percussion model renders a finite distinct response",
          "[signal][particle-percussion][models]") {
    std::array<std::vector<double>, kModels.size()> renders;
    for (std::size_t model = 0; model < kModels.size(); ++model) {
        renders[model] = render_voice(kModels[model], 24000);
        REQUIRE(peak_magnitude(renders[model]) > 1e-8);
        REQUIRE(std::all_of(renders[model].begin(), renders[model].end(),
                            [](double sample) { return std::isfinite(sample); }));
    }

    REQUIRE(renders[0] != renders[1]);
    REQUIRE(renders[0] != renders[2]);
    REQUIRE(renders[1] != renders[2]);
    REQUIRE(renders[3] != renders[0]);

    ParticlePercussionVoice64 silent;
    configure_voice(silent, Model::shaker);
    silent.reset();
    std::vector<double> zero_input(24000, 0.0);
    silent.process(zero_input.data(), static_cast<int>(zero_input.size()));
    REQUIRE(peak_magnitude(zero_input) == 0.0);
}

TEMPLATE_TEST_CASE("Particle voices match independent sample-level body oracles",
                   "[signal][particle-percussion][oracle][precision]", float, double) {
    using Voice = pulp::signal::ParticlePercussionVoiceT<TestType>;
    using ModelType = typename Voice::Model;
    constexpr std::uint64_t seed = 0x71f2c3d4e5a69788ULL;
    constexpr std::array<double, 3> sample_rates{44100.0, 48000.0, 96000.0};
    constexpr std::size_t frames = 4096;
    const double numerical_tolerance = std::is_same_v<TestType, float> ? 2e-6 : 2e-13;
    const double mutation_floor = std::is_same_v<TestType, float> ? 1e-4 : 1e-10;

    for (double sample_rate : sample_rates) {
        for (ModelType model : {ModelType::shaker, ModelType::maraca,
                                ModelType::rattle}) {
            Voice actual;
            actual.prepare(sample_rate);
            actual.set_model(model);
            actual.set_seed(seed);
            actual.set_particle_count(2000);
            actual.set_event_rate(static_cast<TestType>(20000.0));
            actual.set_energy_decay(static_cast<TestType>(250.0));
            actual.reset();
            actual.excite(static_cast<TestType>(1.0));

            ReferenceVoice<TestType> oracle(model, seed, sample_rate);
            ReferenceVoice<TestType> wrong_q(
                model, seed, sample_rate, {.wrong_q_factor = true});
            ReferenceVoice<TestType> band_zero(
                model, seed, sample_rate,
                {.route_every_event_to_band_zero = true});
            double maximum_oracle_error = 0.0;
            double maximum_wrong_q_error = 0.0;
            double maximum_band_zero_error = 0.0;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const TestType observed = actual.process();
                maximum_oracle_error = std::max(
                    maximum_oracle_error,
                    sample_difference(observed, oracle.process()));
                maximum_wrong_q_error = std::max(
                    maximum_wrong_q_error,
                    sample_difference(observed, wrong_q.process()));
                maximum_band_zero_error = std::max(
                    maximum_band_zero_error,
                    sample_difference(observed, band_zero.process()));
            }

            INFO("sample_rate=" << sample_rate
                                 << " model=" << static_cast<int>(model)
                                 << " precision_bytes=" << sizeof(TestType)
                                 << " oracle_error=" << maximum_oracle_error
                                 << " wrong_q_error=" << maximum_wrong_q_error
                                 << " band_zero_error=" << maximum_band_zero_error);
            REQUIRE(maximum_oracle_error <= numerical_tolerance);
            REQUIRE(maximum_wrong_q_error > mutation_floor);
            REQUIRE(maximum_band_zero_error > mutation_floor);
            REQUIRE(maximum_wrong_q_error > 100.0 * maximum_oracle_error);
            REQUIRE(maximum_band_zero_error > 100.0 * maximum_oracle_error);
        }

        Voice actual;
        actual.prepare(sample_rate);
        actual.set_model(ModelType::tambourine);
        actual.set_seed(seed);
        actual.set_particle_count(2000);
        actual.set_event_rate(static_cast<TestType>(20000.0));
        actual.set_energy_decay(static_cast<TestType>(250.0));
        actual.reset();
        actual.excite(static_cast<TestType>(1.0));

        ReferenceVoice<TestType> oracle(ModelType::tambourine, seed, sample_rate);
        ReferenceVoice<TestType> one_mode(
            ModelType::tambourine, seed, sample_rate,
            {.one_tambourine_mode = true});
        double maximum_oracle_error = 0.0;
        double maximum_one_mode_error = 0.0;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const TestType observed = actual.process();
            maximum_oracle_error = std::max(
                maximum_oracle_error,
                sample_difference(observed, oracle.process()));
            maximum_one_mode_error = std::max(
                maximum_one_mode_error,
                sample_difference(observed, one_mode.process()));
        }
        INFO("sample_rate=" << sample_rate
                             << " tambourine precision_bytes=" << sizeof(TestType)
                             << " oracle_error=" << maximum_oracle_error
                             << " one_mode_error=" << maximum_one_mode_error);
        REQUIRE(maximum_oracle_error <= numerical_tolerance);
        REQUIRE(maximum_one_mode_error > mutation_floor);
        REQUIRE(maximum_one_mode_error > 100.0 * maximum_oracle_error);
    }
}

TEMPLATE_TEST_CASE("Particle voice reset replays every model bit exactly",
                   "[signal][particle-percussion][determinism][precision]", float,
                   double) {
    using Voice = pulp::signal::ParticlePercussionVoiceT<TestType>;
    using ModelType = typename Voice::Model;
    constexpr std::uint64_t seed = 0x1234a55a9876f00dULL;
    constexpr std::size_t frames = 4096;

    for (ModelType model : {ModelType::shaker, ModelType::maraca,
                            ModelType::rattle, ModelType::tambourine}) {
        Voice voice;
        voice.prepare(kSampleRate);
        voice.set_model(model);
        voice.set_seed(seed);
        voice.set_particle_count(2000);
        voice.set_event_rate(static_cast<TestType>(20000.0));
        voice.set_energy_decay(static_cast<TestType>(250.0));
        voice.reset();
        voice.excite(static_cast<TestType>(1.0));

        std::vector<TestType> first(frames);
        voice.process(first.data(), static_cast<int>(first.size()));
        voice.reset();
        voice.excite(static_cast<TestType>(1.0));
        std::vector<TestType> replay(frames);
        voice.process(replay.data(), static_cast<int>(replay.size()));

        INFO("model=" << static_cast<int>(model)
                       << " precision_bytes=" << sizeof(TestType));
        REQUIRE(replay == first);
    }
}

TEMPLATE_TEST_CASE("Particle lifecycle reports retained state latency and invalid prepare retention",
                   "[signal][particle-percussion][lifecycle][precision]", float,
                   double) {
    using Exciter = pulp::signal::ParticleCollisionExciterT<TestType>;
    using Voice = pulp::signal::ParticlePercussionVoiceT<TestType>;

    Exciter exciter;
    REQUIRE(exciter.retained_bytes() == 0);
    REQUIRE(exciter.latency_samples() == 0);
    exciter.prepare(7999.0);
    REQUIRE(exciter.sample_rate() == kSampleRate);
    exciter.prepare(8000.0);
    REQUIRE(exciter.sample_rate() == 8000.0);
    exciter.prepare(7999.0);
    REQUIRE(exciter.sample_rate() == 8000.0);

    Voice voice;
    REQUIRE(voice.retained_bytes() == 0);
    REQUIRE(voice.latency_samples() == 0);
    voice.prepare(7999.0);
    REQUIRE(voice.sample_rate() == kSampleRate);
    voice.prepare(8000.0);
    REQUIRE(voice.sample_rate() == 8000.0);
    voice.prepare(7999.0);
    REQUIRE(voice.sample_rate() == 8000.0);
    voice.prepare(44100.0);
    const std::size_t expected_retained_bytes =
        std::is_same_v<TestType, float> ? 448U : 896U;
    REQUIRE(voice.retained_bytes() == expected_retained_bytes);
    REQUIRE(voice.sample_rate() == 44100.0);

    for (double invalid_rate : {
             1.0,
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(),
         }) {
        voice.prepare(invalid_rate);
        INFO("invalid_rate=" << invalid_rate
                              << " precision_bytes=" << sizeof(TestType));
        REQUIRE(voice.sample_rate() == 44100.0);
        REQUIRE(voice.retained_bytes() == expected_retained_bytes);
    }
}

TEMPLATE_TEST_CASE("Particle tails report unbounded floor-zero and decay by the horizon",
                   "[signal][particle-percussion][tail][precision]", float, double) {
    using Exciter = pulp::signal::ParticleCollisionExciterT<TestType>;
    using Voice = pulp::signal::ParticlePercussionVoiceT<TestType>;
    using ModelType = typename Voice::Model;

    Exciter exciter;
    exciter.prepare(kSampleRate);
    exciter.set_sustain_floor(static_cast<TestType>(0.0));
    exciter.excite(static_cast<TestType>(1.0));
    REQUIRE(exciter.tail_samples() == -1);
    exciter.reset();
    REQUIRE(exciter.tail_samples() == 0);

    for (ModelType model : {ModelType::shaker, ModelType::maraca,
                            ModelType::rattle, ModelType::tambourine}) {
        Voice voice;
        voice.prepare(kSampleRate);
        voice.set_model(model);
        voice.set_seed(0x0ddc0ffee1234567ULL);
        voice.set_particle_count(2000);
        voice.set_event_rate(static_cast<TestType>(20000.0));
        voice.set_energy_decay(static_cast<TestType>(250.0));
        voice.set_sustain_floor(static_cast<TestType>(0.02));
        voice.reset();
        voice.excite(static_cast<TestType>(1.0));
        const int horizon = voice.tail_samples();
        REQUIRE(horizon > 0);
        REQUIRE(horizon < 120000);

        double early_peak = 0.0;
        double late_peak = 0.0;
        for (int frame = 0; frame < horizon + 512; ++frame) {
            const double sample = static_cast<double>(voice.process());
            if (frame < 4096)
                early_peak = std::max(early_peak, std::fabs(sample));
            if (frame >= horizon)
                late_peak = std::max(late_peak, std::fabs(sample));
        }
        INFO("model=" << static_cast<int>(model)
                       << " precision_bytes=" << sizeof(TestType)
                       << " horizon=" << horizon
                       << " early_peak=" << early_peak
                       << " late_peak=" << late_peak);
        REQUIRE(early_peak > 1e-8);
        REQUIRE(late_peak < early_peak * 1e-10);
        REQUIRE(late_peak < 1e-12);
    }

    Voice indefinite;
    indefinite.prepare(kSampleRate);
    indefinite.set_sustain_floor(static_cast<TestType>(0.0));
    indefinite.excite(static_cast<TestType>(1.0));
    REQUIRE(indefinite.tail_samples() == -1);
}

TEST_CASE("Prepared particle percussion realtime paths allocate nothing",
          "[signal][particle-percussion][rt-safety]") {
    ParticleCollisionExciter64 exciter;
    configure_exciter(exciter, 0x55aa55aa55aa55aaULL, 2000, 20000.0);

    std::size_t exciter_allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 8192; ++i) {
            if ((i % 257) == 0) exciter.excite(0.5);
            static_cast<void>(exciter.next());
        }
        exciter.reset();
        exciter_allocations = probe.allocation_count();
    }
    REQUIRE(exciter_allocations == 0);

    std::array<double, 1024> block{};
    for (Model model : kModels) {
        ParticlePercussionVoice64 voice;
        configure_voice(voice, model);
        std::size_t allocations = 0;
        {
            pulp::test::RtAllocationProbe probe;
            static_cast<void>(voice.process());
            voice.process(block.data(), static_cast<int>(block.size()));
            voice.excite(0.5);
            voice.reset();
            allocations = probe.allocation_count();
        }
        REQUIRE(allocations == 0);
    }
}

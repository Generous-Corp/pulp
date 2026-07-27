// Forge CharacterDelay catalog-adapter throughput benchmark.
//
// ADVISORY ONLY: this target is built solely with -DPULP_BENCHMARK=ON and does
// not enforce a timing budget. It emits median measurements and ratios so a
// reviewer can see the cost of preserving sample-accurate catalog automation
// without making heterogeneous CI machines into a performance pass/fail gate.
//
// Three lanes keep the comparison honest:
//
//   direct_block       setters once per block, then one DSP block call
//   direct_automated   the same sample-varying controls as the catalog adapter
//   catalog_automated  production process_instance_baked_param callback
//
// `catalog_automated / direct_automated` isolates catalog lookup/dispatch and
// buffer-adapter cost. `catalog_automated / direct_block` states the total cost
// relative to the fastest block-constant use of the DSP, but is deliberately
// labelled separately because the direct block lane does less automation work.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/forge_character_delay_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/character_delay.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

namespace catalog = pulp::host::character_delay;
using Character = pulp::signal::CharacterDelay::Character;
using TapeTier = pulp::signal::CharacterDelay::TapeTier;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockFrames = 128;
constexpr int kWarmupBlocks = 24;
constexpr int kMeasuredBlocks = 384;
constexpr int kTrials = 7;

struct Controls {
    float time_ms;
    float time_offset;
    float feedback;
    float crossfeed;
    float character;
    float mod_rate;
    float mod_depth;
    float duck;
    bool freeze;
    bool reverse;
};

// Every continuous control moves at the sample rate. The ranges stay moderate
// so the benchmark measures the normal adapter path rather than a pathological
// self-oscillation or reverse-segment transition. Both automated lanes call
// this once for each requested parameter, keeping automation-source work equal
// while the catalog lane adds only its production lookup/dispatch path.
float control_value_at(pulp::state::ParamID id, std::uint64_t sample) noexcept {
    const double phase = static_cast<double>(sample % 16384u) / 16384.0;
    const double triangle = 1.0 - std::abs(2.0 * phase - 1.0);
    switch (id) {
        case catalog::kTimeMs: return static_cast<float>(80.0 + 120.0 * triangle);
        case catalog::kTimeOffset: return static_cast<float>(0.9 + 0.2 * phase);
        case catalog::kFeedback: return static_cast<float>(0.25 + 0.2 * triangle);
        case catalog::kCrossfeed: return static_cast<float>(0.35 * phase);
        case catalog::kCharacter: return static_cast<float>(0.35 + 0.3 * triangle);
        case catalog::kModRate: return static_cast<float>(0.2 + 0.25 * phase);
        case catalog::kModDepth: return static_cast<float>(0.08 + 0.08 * triangle);
        case catalog::kDuck: return static_cast<float>(0.1 + 0.15 * phase);
        case catalog::kFreeze:
        case catalog::kReverse: return 0.0f;
        default: return 0.0f;
    }
}

Controls controls_at(std::uint64_t sample) noexcept {
    return {
        control_value_at(catalog::kTimeMs, sample),
        control_value_at(catalog::kTimeOffset, sample),
        control_value_at(catalog::kFeedback, sample),
        control_value_at(catalog::kCrossfeed, sample),
        control_value_at(catalog::kCharacter, sample),
        control_value_at(catalog::kModRate, sample),
        control_value_at(catalog::kModDepth, sample),
        control_value_at(catalog::kDuck, sample),
        control_value_at(catalog::kFreeze, sample) != 0.0f,
        control_value_at(catalog::kReverse, sample) != 0.0f,
    };
}

void apply_controls(pulp::signal::CharacterDelay& delay, const Controls& c) noexcept {
    delay.set_time_ms(c.time_ms);
    delay.set_time_offset(c.time_offset);
    delay.set_feedback(c.feedback);
    delay.set_crossfeed(c.crossfeed);
    delay.set_character_amount(c.character);
    delay.set_mod(c.mod_rate, c.mod_depth);
    delay.set_duck(c.duck);
    delay.set_freeze(c.freeze);
    delay.set_reverse(c.reverse);
}

class AutomatedParamView final : public pulp::host::BakedParamView {
public:
    void set_block_start(std::uint64_t sample) noexcept { block_start_ = sample; }

    float value_at(pulp::state::ParamID id, std::int32_t offset) const override {
        return control_value_at(id, block_start_ + static_cast<std::uint64_t>(offset));
    }

    float value(pulp::state::ParamID id) const override { return value_at(id, 0); }

private:
    std::uint64_t block_start_ = 0;
};

struct StereoBlock {
    std::array<float, kBlockFrames> left{};
    std::array<float, kBlockFrames> right{};

    void seed() noexcept {
        for (int i = 0; i < kBlockFrames; ++i) {
            const double phase = 2.0 * 3.14159265358979323846 * 997.0 * i / kSampleRate;
            left[static_cast<std::size_t>(i)] = static_cast<float>(0.15 * std::sin(phase));
            right[static_cast<std::size_t>(i)] = static_cast<float>(0.12 * std::cos(phase));
        }
    }
};

struct DirectRunner {
    pulp::signal::CharacterDelay delay;
    StereoBlock audio;

    DirectRunner(Character character, TapeTier tier) {
        delay.set_character(character);
        delay.set_tape_tier(tier);
        delay.set_sample_rate(kSampleRate);
    }

    void restart() {
        apply_controls(delay, controls_at(0));
        delay.reset();
        audio.seed();
    }

    void process_block_constant(std::uint64_t block_start) {
        apply_controls(delay, controls_at(block_start));
        delay.process(audio.left.data(), audio.right.data(), kBlockFrames);
    }

    void process_sample_automated(std::uint64_t block_start) {
        for (int i = 0; i < kBlockFrames; ++i) {
            apply_controls(delay, controls_at(block_start + static_cast<std::uint64_t>(i)));
            delay.process(audio.left.data() + i, audio.right.data() + i, 1);
        }
    }

    double sink() const noexcept { return audio.left.back() + audio.right.back(); }
};

struct CatalogRunner {
    pulp::host::CustomNodeType type;
    void* instance = nullptr;
    StereoBlock audio;
    AutomatedParamView params;
    std::array<const float*, 2> input_pointers_{};
    std::array<float*, 2> output_pointers_{};
    pulp::audio::BufferView<const float> input;
    pulp::audio::BufferView<float> output;

    explicit CatalogRunner(Character character, TapeTier tier)
        : type(catalog::make_character_delay_node(character, tier)),
          instance(type.create()),
          input(make_const_pointers(), 2, kBlockFrames),
          output(make_mutable_pointers(), 2, kBlockFrames) {
        REQUIRE(instance != nullptr);
        type.prepare(instance, kSampleRate, kBlockFrames);
    }

    ~CatalogRunner() { type.destroy(instance); }

    CatalogRunner(const CatalogRunner&) = delete;
    CatalogRunner& operator=(const CatalogRunner&) = delete;

    void restart() {
        // Re-prepare outside the measured interval so every trial starts from
        // the factory's deterministic state even after the automation warm-up.
        type.prepare(instance, kSampleRate, kBlockFrames);
        audio.seed();
        params.set_block_start(0);
    }

    void process(std::uint64_t block_start) {
        params.set_block_start(block_start);
        type.process_instance_baked_param(instance, output, input, kBlockFrames, params);
    }

    double sink() const noexcept { return audio.left.back() + audio.right.back(); }

private:
    const float** make_const_pointers() {
        input_pointers_[0] = audio.left.data();
        input_pointers_[1] = audio.right.data();
        return input_pointers_.data();
    }

    float** make_mutable_pointers() {
        output_pointers_[0] = audio.left.data();
        output_pointers_[1] = audio.right.data();
        return output_pointers_.data();
    }

};

volatile double g_benchmark_sink = 0.0;

template <typename Restart, typename Process, typename Sink>
double median_ns_per_sample(Restart&& restart, Process&& process, Sink&& sink) {
    std::array<double, kTrials> trials{};
    for (int trial = 0; trial < kTrials; ++trial) {
        restart();
        for (int block = 0; block < kWarmupBlocks; ++block) {
            process(static_cast<std::uint64_t>(block) * kBlockFrames);
        }

        restart();
        const auto begin = std::chrono::steady_clock::now();
        for (int block = 0; block < kMeasuredBlocks; ++block) {
            process(static_cast<std::uint64_t>(block) * kBlockFrames);
        }
        const auto end = std::chrono::steady_clock::now();
        g_benchmark_sink = sink();
        const double elapsed_ns =
            std::chrono::duration<double, std::nano>(end - begin).count();
        trials[static_cast<std::size_t>(trial)] =
            elapsed_ns / static_cast<double>(kMeasuredBlocks * kBlockFrames);
    }
    std::sort(trials.begin(), trials.end());
    return trials[trials.size() / 2];
}

struct Result {
    std::string_view character;
    double direct_block_ns = 0.0;
    double direct_automated_ns = 0.0;
    double catalog_automated_ns = 0.0;
};

Result measure(std::string_view name, Character character, TapeTier tier) {
    DirectRunner direct_block(character, tier);
    DirectRunner direct_automated(character, tier);
    CatalogRunner catalog_automated(character, tier);

    return {
        name,
        median_ns_per_sample(
            [&] { direct_block.restart(); },
            [&](std::uint64_t sample) { direct_block.process_block_constant(sample); },
            [&] { return direct_block.sink(); }),
        median_ns_per_sample(
            [&] { direct_automated.restart(); },
            [&](std::uint64_t sample) { direct_automated.process_sample_automated(sample); },
            [&] { return direct_automated.sink(); }),
        median_ns_per_sample(
            [&] { catalog_automated.restart(); },
            [&](std::uint64_t sample) { catalog_automated.process(sample); },
            [&] { return catalog_automated.sink(); }),
    };
}

}  // namespace

TEST_CASE("CharacterDelay catalog adapter emits comparative Release evidence",
          "[bench][character-delay][catalog]") {
    const std::array results{
        measure("clean", Character::clean, TapeTier::standard),
        measure("tape", Character::tape, TapeTier::standard),
    };

    std::cout << std::fixed << std::setprecision(3)
              << "[character-delay-adapter-bench]\n"
              << "character direct_block_ns_per_sample direct_automated_ns_per_sample "
                 "catalog_automated_ns_per_sample catalog_vs_direct_automated "
                 "catalog_vs_direct_block\n";
    for (const auto& result : results) {
        REQUIRE(std::isfinite(result.direct_block_ns));
        REQUIRE(std::isfinite(result.direct_automated_ns));
        REQUIRE(std::isfinite(result.catalog_automated_ns));
        REQUIRE(result.direct_block_ns >= 0.0);
        REQUIRE(result.direct_automated_ns >= 0.0);
        REQUIRE(result.catalog_automated_ns >= 0.0);

        const double versus_automated =
            result.catalog_automated_ns / std::max(result.direct_automated_ns, 1.0e-12);
        const double versus_block =
            result.catalog_automated_ns / std::max(result.direct_block_ns, 1.0e-12);
        std::cout << result.character << ' ' << result.direct_block_ns << ' '
                  << result.direct_automated_ns << ' ' << result.catalog_automated_ns << ' '
                  << versus_automated << ' ' << versus_block << '\n';
    }

    // Consume the final samples without assigning any performance meaning to
    // their value. Timing numbers remain evidence, never correctness gates.
    REQUIRE(std::isfinite(g_benchmark_sink));
}

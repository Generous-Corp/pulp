#pragma once

#include "../harness/rt_allocation_probe.hpp"

#include <pulp/signal/rng.hpp>
#include <pulp/signal/waveset_transformer.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace pulp::test::waveset {

using Transformer = pulp::signal::WavesetTransformer;
inline Transformer* startup_hook_transformer = nullptr;
inline Transformer* publication_hook_transformer = nullptr;
inline bool publication_push_rejected = false;
inline bool publication_request_rejected = false;
inline bool publication_finish_latched = false;
inline bool publication_reset_excluded = false;
inline bool finish_reset_excluded = false;
inline bool reset_finish_excluded = false;
inline bool startup_finish_latched = false;
inline bool finish_pull_excluded = false;
inline bool reset_pull_excluded = false;
inline bool reset_completion_push_rejected = false;
inline bool startup_pull_rejected = false;
inline bool control_finish_latched = false;
inline bool drained_pull_excluded = false;
inline bool drained_reset_excluded = false;
inline void request_reverse_after_startup_cas() {
    REQUIRE(startup_hook_transformer != nullptr);
    REQUIRE(startup_hook_transformer->request_program_slot(1));
    Transformer::set_startup_hook(nullptr);
}
inline void finish_after_startup_cas() {
    REQUIRE(startup_hook_transformer != nullptr);
    std::array<float, 2> output{};
    startup_pull_rejected = startup_hook_transformer->pull(output.data(), 2) == 0;
    startup_hook_transformer->finish_input();
    startup_finish_latched = true;
    Transformer::set_startup_hook(nullptr);
}
inline void probe_publishing_exclusion() {
    REQUIRE(publication_hook_transformer != nullptr);
    const float sample = 1.0f;
    publication_push_rejected = publication_hook_transformer->push(&sample, 1) == 0;
    publication_request_rejected = !publication_hook_transformer->request_program_slot(0);
    Transformer::set_publication_hook(nullptr);
}
inline void finish_during_publication() {
    REQUIRE(publication_hook_transformer != nullptr);
    publication_hook_transformer->finish_input();
    publication_finish_latched = true;
    Transformer::set_publication_hook(nullptr);
}
inline void reset_during_publication() {
    REQUIRE(publication_hook_transformer != nullptr);
    publication_hook_transformer->reset();
    publication_reset_excluded = true;
    Transformer::set_publication_hook(nullptr);
}
inline void reset_during_finish() {
    REQUIRE(publication_hook_transformer != nullptr);
    std::array<float, 2> output{};
    finish_pull_excluded = publication_hook_transformer->pull(output.data(), 2) == 0;
    publication_hook_transformer->reset();
    finish_reset_excluded = true;
    Transformer::set_finish_hook(nullptr);
}
inline void finish_during_reset() {
    REQUIRE(publication_hook_transformer != nullptr);
    std::array<float, 2> output{};
    reset_pull_excluded = publication_hook_transformer->pull(output.data(), 2) == 0;
    publication_hook_transformer->finish_input();
    reset_finish_excluded = !publication_hook_transformer->drained();
    Transformer::set_reset_hook(nullptr);
}
inline void push_during_reset_completion() {
    REQUIRE(publication_hook_transformer != nullptr);
    const float sample = 1.0f;
    reset_completion_push_rejected = publication_hook_transformer->push(&sample, 1) == 0;
    Transformer::set_reset_completion_hook(nullptr);
}
inline void finish_during_control_completion() {
    REQUIRE(publication_hook_transformer != nullptr);
    publication_hook_transformer->finish_input();
    control_finish_latched = true;
    Transformer::set_control_completion_hook(nullptr);
}
inline void pull_during_drained_snapshot() {
    REQUIRE(publication_hook_transformer != nullptr);
    float output{};
    drained_pull_excluded = publication_hook_transformer->pull(&output, 1) == 0;
    Transformer::set_drained_snapshot_hook(nullptr);
}
inline void reset_during_drained_snapshot() {
    REQUIRE(publication_hook_transformer != nullptr);
    publication_hook_transformer->reset();
    const float sample = 5.0f;
    drained_reset_excluded = publication_hook_transformer->push(&sample, 1) == 0;
    Transformer::set_drained_snapshot_hook(nullptr);
}

inline Transformer::OperationProgram program(Transformer::Operation operation,
                                             std::uint8_t repeat = 1) {
    Transformer::OperationProgram result;
    result.steps.push_back({operation, repeat});
    return result;
}

constexpr std::uint64_t oracle_mix64(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

inline double oracle_coordinate(std::uint64_t seed, std::uint64_t index) {
    constexpr std::uint64_t gamma = 0x9E3779B97F4A7C15ull;
    const auto key = oracle_mix64(seed * gamma + index + gamma);
    return static_cast<double>(oracle_mix64(key) >> 11) * (1.0 / 9007199254740992.0);
}

inline std::vector<float> drain(Transformer& transformer, const std::vector<float>& input,
                                int push_size = 64) {
    std::vector<float> output;
    std::array<float, 256> block{};
    std::size_t offset = 0;
    while (offset < input.size()) {
        const int requested = std::min<int>(push_size, static_cast<int>(input.size() - offset));
        const int accepted = transformer.push(input.data() + offset, requested);
        offset += static_cast<std::size_t>(accepted);
        const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
        output.insert(output.end(), block.begin(), block.begin() + pulled);
        REQUIRE((accepted != 0 || pulled != 0));
    }
    transformer.finish_input();
    while (!transformer.drained()) {
        const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
        REQUIRE(pulled > 0);
        output.insert(output.end(), block.begin(), block.begin() + pulled);
    }
    return output;
}

inline std::vector<float> drain_partitioned(Transformer& transformer,
                                            const std::vector<float>& input, std::uint32_t seed) {
    std::vector<float> output;
    std::array<float, 31> block{};
    std::size_t offset = 0;
    while (offset < input.size()) {
        seed = seed * 1664525u + 1013904223u;
        const int requested = std::min<int>(1 + static_cast<int>(seed % 19u),
                                            static_cast<int>(input.size() - offset));
        const int accepted = transformer.push(input.data() + offset, requested);
        offset += static_cast<std::size_t>(accepted);
        seed = seed * 1664525u + 1013904223u;
        const int pulled =
            transformer.pull(block.data(), 1 + static_cast<int>(seed % block.size()));
        output.insert(output.end(), block.begin(), block.begin() + pulled);
        REQUIRE((accepted != 0 || pulled != 0));
    }
    transformer.finish_input();
    while (!transformer.drained()) {
        const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
        REQUIRE(pulled > 0);
        output.insert(output.end(), block.begin(), block.begin() + pulled);
    }
    return output;
}

inline std::vector<float> four_segments() {
    return {-4.0f, -3.0f, 1.0f, 2.0f, -2.0f, -1.0f, 3.0f, 4.0f};
}

struct ScheduledResult {
    std::vector<float> output;
    std::vector<std::size_t> backpressure_positions;
};

inline ScheduledResult run_constant_pull_schedule(int partition, std::uint32_t random_seed = 0) {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {2, 8, 1.0}));
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Repeat, 16)));
    std::vector<float> input(64);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = (i & 1u) ? 1.0f : -1.0f;
    ScheduledResult result;
    std::array<float, 7> block{};
    std::size_t accepted = 0;
    std::size_t next_milestone = 8;
    while (accepted < input.size()) {
        const auto until_milestone = next_milestone - accepted;
        if (random_seed != 0) {
            random_seed = random_seed * 1664525u + 1013904223u;
            partition = 1 + static_cast<int>(random_seed % 19u);
        }
        const int requested = static_cast<int>(std::min<std::size_t>(
            {static_cast<std::size_t>(partition), until_milestone, input.size() - accepted}));
        const int pushed = transformer.push(input.data() + accepted, requested);
        accepted += static_cast<std::size_t>(pushed);
        if (pushed < requested) {
            result.backpressure_positions.push_back(accepted);
            const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
            REQUIRE(pulled > 0);
            result.output.insert(result.output.end(), block.begin(), block.begin() + pulled);
        }
        if (accepted == next_milestone) {
            const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
            result.output.insert(result.output.end(), block.begin(), block.begin() + pulled);
            next_milestone += 8;
        }
    }
    transformer.finish_input();
    while (!transformer.drained()) {
        const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
        REQUIRE(pulled > 0);
        result.output.insert(result.output.end(), block.begin(), block.begin() + pulled);
    }
    return result;
}

} // namespace pulp::test::waveset

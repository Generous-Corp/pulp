#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/convolver.hpp>
#include <vector>
#include <cmath>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

TEST_CASE("Convolver: identity IR passes audio through", "[signal][convolver]") {
    constexpr size_t block_size = 64;
    std::vector<float> ir(block_size, 0.0f);
    ir[0] = 1.0f;

    PartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block_size);

    REQUIRE(conv.is_loaded());
    REQUIRE(conv.latency() == 0);
    REQUIRE(conv.num_partitions() == 1);

    std::vector<float> input(block_size);
    std::vector<float> output(block_size);
    for (size_t i = 0; i < block_size; ++i)
        input[i] = std::sin(2.0f * 3.14159f * i / block_size);

    // Identity IR should produce immediate output (zero latency)
    conv.process(input.data(), output.data(), block_size);

    // Output should match input for identity IR (delta at sample 0)
    float error = 0;
    for (size_t i = 0; i < block_size; ++i)
        error += std::abs(output[i] - input[i]);
    REQUIRE(error < 0.01f);
}

TEST_CASE("Convolver: reset clears state", "[signal][convolver]") {
    constexpr size_t block_size = 32;
    std::vector<float> ir = {1.0f, 0.5f, 0.25f};

    PartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block_size);

    std::vector<float> input(block_size, 1.0f);
    std::vector<float> output(block_size);
    conv.process(input.data(), output.data(), block_size);
    conv.reset();

    std::fill(input.begin(), input.end(), 0.0f);
    conv.process(input.data(), output.data(), block_size);

    float energy = 0;
    for (float s : output) energy += s * s;
    REQUIRE(energy < 0.001f);
}

TEST_CASE("Convolver: unloaded passes through", "[signal][convolver]") {
    PartitionedConvolver conv;
    REQUIRE(!conv.is_loaded());

    constexpr size_t n = 64;
    std::vector<float> input(n);
    std::vector<float> output(n);
    for (size_t i = 0; i < n; ++i) input[i] = static_cast<float>(i);
    conv.process(input.data(), output.data(), n);

    for (size_t i = 0; i < n; ++i)
        REQUIRE_THAT(output[i], WithinAbs(input[i], 0.001f));
}

TEST_CASE("Convolver: multi-partition IR", "[signal][convolver]") {
    constexpr size_t block_size = 32;
    std::vector<float> ir(block_size * 3, 0.0f);
    ir[0] = 1.0f;
    ir[block_size] = 0.5f;
    ir[block_size * 2] = 0.25f;

    PartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block_size);
    REQUIRE(conv.num_partitions() == 3);
    REQUIRE(conv.latency() == 0);
}

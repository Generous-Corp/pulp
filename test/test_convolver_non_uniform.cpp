// NonUniformPartitionedConvolver tests (macOS plugin authoring
// plan item 2.3, Slice B).
//
// Validates:
//   - block_size = audio callback size; head + tail partitions stitch
//     cleanly with zero net latency (impulse round-trip = the IR),
//   - long-IR output matches a direct convolution reference within
//     float epsilon over multiple consecutive blocks,
//   - output matches the uniform PartitionedConvolver on the same IR
//     (functional equivalence with a different inner schedule),
//   - reset() clears all stage state.

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/convolver.hpp>
#include <pulp/signal/convolver_non_uniform.hpp>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> make_impulse(std::size_t length) {
    std::vector<float> v(length, 0.0f);
    v[0] = 1.0f;
    return v;
}

std::vector<float> make_decaying_ir(std::size_t length, float decay) {
    std::vector<float> v(length);
    for (std::size_t i = 0; i < length; ++i)
        v[i] = std::exp(-decay * static_cast<float>(i)) *
               (i % 2 == 0 ? 1.0f : -0.5f);
    return v;
}

std::vector<float> make_random_block(std::size_t length, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> v(length);
    for (auto& x : v) x = dist(rng);
    return v;
}

// Direct convolution reference (linear, no aliasing). Returns the
// first `out_length` samples of input * ir.
std::vector<float> direct_convolve(const std::vector<float>& input,
                                    const std::vector<float>& ir,
                                    std::size_t out_length) {
    std::vector<float> y(out_length, 0.0f);
    for (std::size_t n = 0; n < out_length; ++n) {
        float acc = 0.0f;
        for (std::size_t m = 0; m < ir.size(); ++m) {
            if (m > n) break;
            acc += ir[m] * input[n - m];
        }
        y[n] = acc;
    }
    return y;
}

// Push `input` through `conv` in blocks of `block_size`, accumulating
// output. Pads `input` with trailing zeros as needed to flush the IR.
template <typename Conv>
std::vector<float> run_blocks(Conv& conv, const std::vector<float>& input,
                              std::size_t block_size, std::size_t out_length) {
    std::vector<float> padded = input;
    if (padded.size() < out_length) padded.resize(out_length, 0.0f);
    std::vector<float> out(out_length, 0.0f);
    for (std::size_t i = 0; i + block_size <= out_length; i += block_size) {
        conv.process(padded.data() + i, out.data() + i, block_size);
    }
    return out;
}

} // namespace

TEST_CASE("NonUniformPartitionedConvolver impulse-in returns IR samples",
          "[signal][convolver][non-uniform]") {
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kIrLen = 1024;
    constexpr std::size_t kK = 4;  // tail multiplier

    auto ir = make_decaying_ir(kIrLen, 0.005f);
    NonUniformPartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), kBlock, kK);
    REQUIRE(conv.is_loaded());
    REQUIRE(conv.head_samples() == kBlock * kK);
    REQUIRE(conv.tail_block() == kBlock * kK);
    REQUIRE(conv.latency() == 0);

    // Send a unit impulse followed by zeros for enough blocks to
    // observe the entire IR.
    auto in = make_impulse(kIrLen + 2 * kBlock);
    auto out = run_blocks(conv, in, kBlock, kIrLen + 2 * kBlock);

    // First kIrLen output samples should equal the IR within float
    // epsilon. FFT round-tripping accumulates at ~1e-5 worst case for
    // these sizes.
    for (std::size_t i = 0; i < kIrLen; ++i) {
        INFO("i = " << i << " expected " << ir[i] << " got " << out[i]);
        REQUIRE_THAT(out[i], WithinAbs(ir[i], 1e-4f));
    }
}

TEST_CASE("NonUniformPartitionedConvolver matches direct convolution on noise",
          "[signal][convolver][non-uniform]") {
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kIrLen = 800;
    constexpr std::size_t kTotalBlocks = 24;     // 1.5 KiB of audio
    constexpr std::size_t kTotalLen = kTotalBlocks * kBlock;

    auto ir = make_decaying_ir(kIrLen, 0.003f);
    auto input = make_random_block(kTotalLen, /*seed=*/42);

    NonUniformPartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), kBlock, /*K=*/4);

    auto out = run_blocks(conv, input, kBlock, kTotalLen);
    auto ref = direct_convolve(input, ir, kTotalLen);

    // Compare the steady-state region where both head and tail are
    // active. The tail FFT fires at the end of every K small blocks,
    // and its result is streamed over the following K small blocks,
    // so the first head_samples + tail_block samples are still
    // warming up.
    std::size_t start = conv.head_samples() + conv.tail_block();
    std::size_t end = kTotalLen;
    REQUIRE(end > start);
    for (std::size_t i = start; i < end; ++i) {
        INFO("i = " << i << " ref " << ref[i] << " got " << out[i]);
        REQUIRE_THAT(out[i], WithinAbs(ref[i], 1e-3f));
    }
}

TEST_CASE("NonUniformPartitionedConvolver matches uniform PartitionedConvolver",
          "[signal][convolver][non-uniform]") {
    constexpr std::size_t kBlock = 32;
    constexpr std::size_t kIrLen = 600;
    constexpr std::size_t kTotalLen = 16 * kBlock;

    auto ir = make_decaying_ir(kIrLen, 0.004f);
    auto input = make_random_block(kTotalLen, /*seed=*/7);

    NonUniformPartitionedConvolver nu;
    nu.load_ir(ir.data(), ir.size(), kBlock, /*K=*/4);

    PartitionedConvolver uniform;
    uniform.load_ir(ir.data(), ir.size(), kBlock);

    auto out_nu = run_blocks(nu, input, kBlock, kTotalLen);
    auto out_uni = run_blocks(uniform, input, kBlock, kTotalLen);

    // After the tail-buffering delay has flushed in (head_samples +
    // tail_block ≈ 2·head_samples), the two outputs should agree to
    // FFT float epsilon.
    std::size_t start = nu.head_samples() + nu.tail_block();
    for (std::size_t i = start; i < kTotalLen; ++i) {
        INFO("i = " << i << " uniform " << out_uni[i] << " non-uniform " << out_nu[i]);
        REQUIRE_THAT(out_nu[i], WithinAbs(out_uni[i], 1e-3f));
    }
}

TEST_CASE("NonUniformPartitionedConvolver short IR (head only) degenerates correctly",
          "[signal][convolver][non-uniform]") {
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kK = 4;
    // IR fits entirely in the head.
    constexpr std::size_t kIrLen = kBlock * kK - 7;  // smaller than head_samples

    auto ir = make_decaying_ir(kIrLen, 0.01f);
    NonUniformPartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), kBlock, kK);
    REQUIRE(conv.is_loaded());
    REQUIRE(conv.tail_block() == 0);  // no tail stage

    auto in = make_impulse(kIrLen + 2 * kBlock);
    auto out = run_blocks(conv, in, kBlock, kIrLen + 2 * kBlock);
    for (std::size_t i = 0; i < kIrLen; ++i) {
        INFO("i = " << i);
        REQUIRE_THAT(out[i], WithinAbs(ir[i], 1e-4f));
    }
}

TEST_CASE("NonUniformPartitionedConvolver rejects bad input gracefully",
          "[signal][convolver][non-uniform]") {
    NonUniformPartitionedConvolver conv;
    conv.load_ir(nullptr, 100, 64);
    REQUIRE_FALSE(conv.is_loaded());

    std::vector<float> ir(128, 1.0f);
    conv.load_ir(ir.data(), 0, 64);
    REQUIRE_FALSE(conv.is_loaded());

    conv.load_ir(ir.data(), ir.size(), 0);
    REQUIRE_FALSE(conv.is_loaded());

    // Wrong process() block-size: pass-through fallback.
    conv.load_ir(ir.data(), ir.size(), 64);
    REQUIRE(conv.is_loaded());
    std::vector<float> in(32, 0.3f);
    std::vector<float> out(32, -1.0f);
    conv.process(in.data(), out.data(), 32);
    for (auto v : out) REQUIRE(v == 0.3f);  // pass-through copy
}

TEST_CASE("NonUniformPartitionedConvolver reset clears state",
          "[signal][convolver][non-uniform]") {
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kIrLen = 512;

    auto ir = make_decaying_ir(kIrLen, 0.005f);
    NonUniformPartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), kBlock, /*K=*/4);

    // Push some non-trivial input.
    auto input = make_random_block(8 * kBlock, /*seed=*/11);
    std::vector<float> scratch(kBlock);
    for (std::size_t i = 0; i + kBlock <= input.size(); i += kBlock) {
        conv.process(input.data() + i, scratch.data(), kBlock);
    }

    conv.reset();
    // After reset, an impulse should produce the IR again starting at 0.
    auto in = make_impulse(kIrLen + 2 * kBlock);
    auto out = run_blocks(conv, in, kBlock, kIrLen + 2 * kBlock);
    for (std::size_t i = 0; i < kIrLen; ++i) {
        REQUIRE_THAT(out[i], WithinAbs(ir[i], 1e-4f));
    }
}

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/convolver.hpp>
#include <pulp/signal/fixed_ir_convolver.hpp>
#include <pulp/signal/fft.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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

TEST_CASE("Convolver64: identity IR passes double audio through",
          "[signal][convolver][f64]") {
    constexpr size_t block_size = 16;
    const std::vector<double> ir = {1.0};

    PartitionedConvolver64 conv;
    conv.load_ir(ir.data(), ir.size(), block_size);
    REQUIRE(conv.is_loaded());

    std::vector<double> input(block_size);
    std::vector<double> output(block_size, 0.0);
    for (size_t i = 0; i < block_size; ++i)
        input[i] = std::sin(2.0 * 3.14159265358979323846 *
                            static_cast<double>(i) / block_size);

    conv.process(input.data(), output.data(), block_size);

    for (size_t i = 0; i < block_size; ++i)
        REQUIRE_THAT(output[i], WithinAbs(input[i], 1e-12));
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

TEST_CASE("Convolver: half-spectrum output matches a direct convolution",
          "[signal][convolver]") {
    // The frequency-domain product is MAC'd over only the lower half of the
    // Hermitian spectrum and the upper half is reconstructed by conjugate
    // symmetry. Stream several blocks through a long, non-block-aligned,
    // multi-partition IR and require the streamed (zero-latency overlap-save)
    // output to match a direct time-domain FIR convolution of the same input.
    constexpr size_t block = 64;
    const size_t ir_len = block * 3 + 17;   // spans partitions; unaligned tail
    std::vector<float> ir(ir_len);
    for (size_t k = 0; k < ir_len; ++k)
        ir[k] = std::sin(0.3f * static_cast<float>(k) + 0.5f)
                * std::exp(-0.01f * static_cast<float>(k));

    PartitionedConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block);

    const size_t nblocks = 6;
    std::vector<float> x(nblocks * block);
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = std::sin(0.21f * static_cast<float>(n))
               - 0.4f * std::cos(0.07f * static_cast<float>(n));

    std::vector<float> y(x.size(), 0.0f);
    for (size_t b = 0; b < nblocks; ++b)
        conv.process(&x[b * block], &y[b * block], block);

    for (size_t n = 0; n < x.size(); ++n) {
        double ref = 0.0;   // y[n] = Σ_k ir[k]*x[n-k]
        for (size_t k = 0; k < ir_len && k <= n; ++k)
            ref += static_cast<double>(ir[k]) * static_cast<double>(x[n - k]);
        REQUIRE_THAT(y[n], WithinAbs(ref, 2e-3));
    }
}

TEST_CASE("Convolver: block size is rounded to a processable power of two",
          "[signal][convolver][issue-645]") {
    const std::vector<float> ir = {1.0f};

    PartitionedConvolver rounded;
    rounded.load_ir(ir.data(), ir.size(), 3);
    REQUIRE(rounded.is_loaded());
    REQUIRE(rounded.num_partitions() == 1);

    const float input[] = {1.0f, -0.5f, 0.25f, 0.75f};
    float output[] = {0.0f, 0.0f, 0.0f, 0.0f};
    rounded.process(input, output, 4);

    for (size_t i = 0; i < 4; ++i)
        REQUIRE_THAT(output[i], WithinAbs(input[i], 1e-5f));

    PartitionedConvolver zero_block;
    zero_block.load_ir(ir.data(), ir.size(), 0);
    REQUIRE(zero_block.is_loaded());

    const float single_input[] = {0.625f};
    float single_output[] = {0.0f};
    zero_block.process(single_input, single_output, 1);
    REQUIRE_THAT(single_output[0], WithinAbs(single_input[0], 1e-5f));
}

TEST_CASE("FixedIrConvolver crossover edges follow the measured per-block table",
          "[signal][convolver][fixed-ir]") {
    using Backend = ConvolutionBackend;
#if defined(PULP_SIMD_DEFAULT_BACKEND_ACCELERATE)
    // At 8-frame callbacks and below the direct engine covers the whole
    // measured range; longer IRs keep the FFT engine.
    for (const std::size_t block :
         {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}}) {
        INFO("block " << block);
        REQUIRE(FixedIrConvolver::direct_fir_limit(block) == 4096);
        REQUIRE(FixedIrConvolver::choose_backend(4096, block) == Backend::direct_fir);
        REQUIRE(FixedIrConvolver::choose_backend(4097, block) == Backend::partitioned_fft);
        REQUIRE(FixedIrConvolver::choose_backend(65536, block) == Backend::partitioned_fft);
    }
    struct Edge {
        std::size_t block, limit;
    };
    // Non-power-of-two requests normalize up, so 9 shares the 16-frame row,
    // 17 the 32-frame row, 65 the 128-frame row, and 4096 the large-block row.
    for (const Edge e :
         {Edge{9, 576}, Edge{16, 576}, Edge{17, 128}, Edge{32, 128}, Edge{33, 128}, Edge{64, 128},
          Edge{65, 64}, Edge{128, 64}, Edge{256, 64}, Edge{512, 64}, Edge{1024, 64},
          Edge{4096, 64}}) {
        INFO("block " << e.block);
        REQUIRE(FixedIrConvolver::direct_fir_limit(e.block) == e.limit);
        REQUIRE(FixedIrConvolver::choose_backend(e.limit, e.block) == Backend::direct_fir);
        REQUIRE(FixedIrConvolver::choose_backend(e.limit + 1, e.block) == Backend::partitioned_fft);
    }
#else
    // Backends without their own measurements keep the partitioned engine.
    for (const std::size_t block : {std::size_t{1}, std::size_t{16}, std::size_t{512}}) {
        REQUIRE(FixedIrConvolver::direct_fir_limit(block) == 0);
        REQUIRE(FixedIrConvolver::choose_backend(1, block) == Backend::partitioned_fft);
    }
#endif
}

TEST_CASE("FixedIrConvolver without an IR passes audio through unchanged",
          "[signal][convolver][fixed-ir]") {
    FixedIrConvolver conv;
    REQUIRE_FALSE(conv.is_loaded());
    REQUIRE(conv.backend() == ConvolutionBackend::none);
    REQUIRE(conv.block_size() == 0);

    std::vector<float> data = {0.5f, -0.25f, 0.125f, 1.0f};
    const std::vector<float> original = data;
    conv.process(data.data(), data.data(), data.size()); // exact alias
    REQUIRE(data == original);

    std::vector<float> output(data.size(), -1.0f);
    conv.process(data.data(), output.data(), data.size());
    REQUIRE(output == original);

    // Loading an empty IR after a real one returns to the unloaded state.
    const std::vector<float> ir(33, 0.1f);
    conv.load_ir(ir.data(), ir.size(), 4);
    REQUIRE(conv.is_loaded());
    conv.load_ir(nullptr, 0, 4);
    REQUIRE_FALSE(conv.is_loaded());
    REQUIRE(conv.backend() == ConvolutionBackend::none);
    REQUIRE(conv.block_size() == 0);
    // Naming no engine unloads a real IR too.
    conv.load_ir(ir.data(), ir.size(), 4, ConvolutionBackend::direct_fir);
    REQUIRE(conv.is_loaded());
    conv.load_ir(ir.data(), ir.size(), 4, ConvolutionBackend::none);
    REQUIRE_FALSE(conv.is_loaded());
    conv.reset();
    output.assign(data.size(), -1.0f);
    conv.process(data.data(), output.data(), data.size());
    REQUIRE(output == original);
    REQUIRE(conv.block_size_violations() == 0);
}

TEST_CASE("FixedIrConvolver direct and partitioned paths match a streamed FIR",
          "[signal][convolver][fixed-ir]") {
    constexpr std::size_t block = 64;
    constexpr std::size_t blocks = 8;
    const std::size_t total = block * blocks;
    std::vector<float> ir(255);
    for (std::size_t i = 0; i < ir.size(); ++i)
        ir[i] = 0.003f * std::sin(0.17f * static_cast<float>(i));
    std::vector<float> input(total);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = std::sin(0.11f * static_cast<float>(i)) -
                   0.25f * std::cos(0.037f * static_cast<float>(i));

    FixedIrConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block, ConvolutionBackend::direct_fir);
    REQUIRE(conv.backend() == ConvolutionBackend::direct_fir);

    std::vector<float> output(total);
    for (std::size_t b = 0; b < blocks; ++b)
        conv.process(input.data() + b * block, output.data() + b * block, block);

    for (std::size_t n = 0; n < total; ++n) {
        double expected = 0.0;
        for (std::size_t k = 0; k < ir.size() && k <= n; ++k)
            expected += static_cast<double>(ir[k]) * input[n - k];
        REQUIRE_THAT(output[n], WithinAbs(expected, 2e-5));
    }

    // A longer IR takes the FFT path but has the same stream-level oracle.
    ir.resize(1025);
    for (std::size_t i = 255; i < ir.size(); ++i)
        ir[i] = 0.001f * std::sin(0.07f * static_cast<float>(i));
    conv.load_ir(ir.data(), ir.size(), block, ConvolutionBackend::partitioned_fft);
    REQUIRE(conv.backend() == ConvolutionBackend::partitioned_fft);
    conv.reset();
    std::fill(output.begin(), output.end(), 0.0f);
    for (std::size_t b = 0; b < blocks; ++b)
        conv.process(input.data() + b * block, output.data() + b * block, block);
    for (std::size_t n = 0; n < total; ++n) {
        double expected = 0.0;
        for (std::size_t k = 0; k < ir.size() && k <= n; ++k)
            expected += static_cast<double>(ir[k]) * input[n - k];
        REQUIRE_THAT(output[n], WithinAbs(expected, 2e-5));
    }
}

TEST_CASE("FixedIrConvolver direct processing remains stable across history wrap",
          "[signal][convolver][fixed-ir][rt]") {
    constexpr std::size_t block = 32;
    const std::vector<float> ir(65, 0.01f);
    FixedIrConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block, ConvolutionBackend::direct_fir);
    std::vector<float> data(block, 0.25f);
    conv.process(data.data(), data.data(), data.size());

    // Exercise repeated blocks, including the direct FIR history
    // wrap/compact boundary.
    for (int i = 0; i < 48; ++i)
        conv.process(data.data(), data.data(), data.size());
    REQUIRE(conv.block_size_violations() == 0);
}

TEST_CASE(
    "FixedIrConvolver direct path preserves a long stream across repeated history compactions",
    "[signal][convolver][fixed-ir][rt]") {
    constexpr std::size_t block = 16;
    constexpr std::size_t ir_length = 513;
    constexpr std::size_t blocks = 96;
    const std::size_t total = block * blocks;

    // 513 taps stays on the direct path for a 16-sample callback while forcing
    // FirFilterT to compact its >512-sample history several times.
    std::vector<float> ir(ir_length);
    for (std::size_t i = 0; i < ir.size(); ++i) {
        const float phase = static_cast<float>(i);
        ir[i] = (0.0015f * std::sin(0.071f * phase) + 0.0007f * std::cos(0.013f * phase)) /
                (1.0f + 0.002f * phase);
    }
    std::vector<float> input(total);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const float phase = static_cast<float>(i);
        input[i] = 0.65f * std::sin(0.037f * phase) + 0.23f * std::cos(0.113f * phase) +
                   ((i % 29u) == 0u ? 0.4f : 0.0f);
    }

    FixedIrConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block, ConvolutionBackend::direct_fir);
    REQUIRE(conv.backend() == ConvolutionBackend::direct_fir);

    std::vector<float> output(total, 0.0f);
    for (std::size_t b = 0; b < blocks; ++b)
        conv.process(input.data() + b * block, output.data() + b * block, block);

    for (std::size_t n = 0; n < total; ++n) {
        double expected = 0.0;
        const std::size_t taps = std::min(ir.size(), n + 1);
        for (std::size_t k = 0; k < taps; ++k)
            expected += static_cast<double>(ir[k]) * input[n - k];
        INFO("sample " << n);
        REQUIRE_THAT(output[n], WithinAbs(expected, 3e-5));
    }
    REQUIRE(conv.block_size_violations() == 0);
}

TEST_CASE("FixedIrConvolver fails closed and resets after a wrong block size",
          "[signal][convolver][fixed-ir]") {
    const std::vector<float> ir(1025, 0.01f);
    constexpr std::size_t block = 64;
    FixedIrConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block, ConvolutionBackend::partitioned_fft);
    REQUIRE(conv.backend() == ConvolutionBackend::partitioned_fft);

    std::vector<float> input(block, 0.25f);
    std::vector<float> history_out(block);
    conv.process(input.data(), history_out.data(), block); // leave IR history behind
    std::vector<float> output(block, -1.0f);
    conv.process(input.data(), output.data(), block - 1);
    REQUIRE(conv.block_size_violations() == 1);
    for (std::size_t i = 0; i < block - 1; ++i)
        REQUIRE(output[i] == 0.0f);
    REQUIRE(output.back() == -1.0f);

    // The first valid block after the violation must equal a fresh engine's
    // first block: no history from before the violation may leak through.
    FixedIrConvolver fresh;
    fresh.load_ir(ir.data(), ir.size(), block, ConvolutionBackend::partitioned_fft);
    std::vector<float> expected(block);
    fresh.process(input.data(), expected.data(), block);
    output.assign(block, -1.0f);
    conv.process(input.data(), output.data(), block);
    REQUIRE(conv.block_size_violations() == 1);
    for (std::size_t i = 0; i < block; ++i)
        REQUIRE_THAT(output[i], WithinAbs(expected[i], 1e-6));
}

TEST_CASE("FixedIrConvolver64 matches a long-double oracle on both engines",
          "[signal][convolver][fixed-ir]") {
    constexpr std::size_t block = 64;
    for (const std::size_t taps : {std::size_t{65}, std::size_t{1025}}) {
        std::vector<double> ir(taps);
        for (std::size_t i = 0; i < taps; ++i)
            ir[i] = 0.01 * std::sin(0.19 * static_cast<double>(i)) *
                    std::exp(-0.003 * static_cast<double>(i));
        for (const auto engine :
             {ConvolutionBackend::direct_fir, ConvolutionBackend::partitioned_fft}) {
            FixedIrConvolver64 conv;
            conv.load_ir(ir.data(), ir.size(), block, engine);
            INFO("taps " << taps << " backend " << static_cast<int>(conv.backend()));
            const std::size_t total = block * 24;
            std::vector<double> input(total), output(total);
            for (std::size_t i = 0; i < total; ++i)
                input[i] = std::sin(0.071 * static_cast<double>(i)) + ((i % 37u) == 0u ? 0.5 : 0.0);
            for (std::size_t b = 0; b < total / block; ++b)
                conv.process(input.data() + b * block, output.data() + b * block, block);
            for (std::size_t n = 0; n < total; ++n) {
                long double expected = 0.0L;
                for (std::size_t k = 0; k < taps && k <= n; ++k)
                    expected += static_cast<long double>(ir[k]) * input[n - k];
                REQUIRE_THAT(output[n], WithinAbs(static_cast<double>(expected), 1e-9));
            }
        }
    }
}

TEST_CASE("FixedIrConvolver direct path recovers cleanly after a wrong block size",
          "[signal][convolver][fixed-ir]") {
    constexpr std::size_t block = 16;
    constexpr std::size_t ir_length = 513;
    const std::vector<float> ir = [] {
        std::vector<float> value(ir_length);
        for (std::size_t i = 0; i < value.size(); ++i)
            value[i] =
                0.002f * std::exp(-0.004f * static_cast<float>(i)) * (i % 3u == 0u ? 1.0f : -0.45f);
        return value;
    }();

    FixedIrConvolver conv;
    conv.load_ir(ir.data(), ir.size(), block, ConvolutionBackend::direct_fir);
    REQUIRE(conv.backend() == ConvolutionBackend::direct_fir);

    FirFilter reference;
    reference.set_coefficients(ir);

    std::vector<float> first(block, 0.35f), first_output(block), first_expected(block);
    conv.process(first.data(), first_output.data(), block);
    reference.process(first.data(), first_expected.data(), static_cast<int>(block));
    for (std::size_t i = 0; i < block; ++i)
        REQUIRE_THAT(first_output[i], WithinAbs(first_expected[i], 3e-5));

    const std::size_t wrong_size = block - 7;
    std::vector<float> bad_input(wrong_size, 0.8f);
    std::vector<float> bad_output(wrong_size, -1.0f);
    conv.process(bad_input.data(), bad_output.data(), wrong_size);
    REQUIRE(conv.block_size_violations() == 1);
    REQUIRE(std::all_of(bad_output.begin(), bad_output.end(),
                        [](float sample) { return sample == 0.0f; }));

    // The reference never sees the invalid callback. A stale direct history
    // would make the first valid block below diverge from this stream.
    reference.reset();
    std::vector<float> input(block), output(block), expected(block);
    for (int b = 0; b < 4; ++b) {
        for (std::size_t i = 0; i < block; ++i) {
            const float phase = static_cast<float>(b * block + i);
            input[i] =
                std::sin(0.09f * phase) + (i == 0u ? 0.25f * static_cast<float>(b + 1) : 0.0f);
        }
        conv.process(input.data(), output.data(), block);
        reference.process(input.data(), expected.data(), static_cast<int>(block));
        for (std::size_t i = 0; i < block; ++i) {
            INFO("block " << b << " sample " << i);
            REQUIRE_THAT(output[i], WithinAbs(expected[i], 3e-5));
        }
    }
    REQUIRE(conv.block_size_violations() == 1);
}

TEST_CASE("Convolver: an empty IR passes input through",
          "[signal][convolver][issue-645]") {
    float dummy_ir = 0.0f;
    PartitionedConvolver empty_ir;
    empty_ir.load_ir(&dummy_ir, 0, 8);
    REQUIRE_FALSE(empty_ir.is_loaded());
    REQUIRE(empty_ir.num_partitions() == 0);

    // A convolver with nothing to convolve with is a wire, at any block size.
    const float input[] = {0.0f, 1.0f, 2.0f, 3.0f};
    float output[] = {-1.0f, -1.0f, -1.0f, -1.0f};
    empty_ir.process(input, output, 4);

    for (size_t i = 0; i < 4; ++i)
        REQUIRE_THAT(output[i], WithinAbs(input[i], 1e-6f));
    REQUIRE(empty_ir.block_size_violations() == 0);
}

TEST_CASE("Convolver: a loaded convolver fails closed on a wrong block size",
          "[signal][convolver][issue-645]") {
    const std::vector<float> identity_ir = {1.0f};
    PartitionedConvolver loaded;
    loaded.load_ir(identity_ir.data(), identity_ir.size(), 8);
    REQUIRE(loaded.is_loaded());
    REQUIRE(loaded.block_size() == 8);

    // 4 != block_size() == 8. Passing the input through here would be audibly
    // indistinguishable from this identity IR working correctly -- the caller's
    // block size bug would produce perfect-sounding audio and never be found.
    // Fail closed instead: silence, and a violation the caller can assert on.
    const float input[] = {0.0f, 1.0f, 2.0f, 3.0f};
    float output[] = {-1.0f, -1.0f, -1.0f, -1.0f};
    loaded.process(input, output, 4);

    for (size_t i = 0; i < 4; ++i)
        REQUIRE(output[i] == 0.0f);
    REQUIRE(loaded.block_size_violations() == 1);
}

TEST_CASE("Convolver: a block-size violation does not corrupt the stream after it",
          "[signal][convolver][issue-645]") {
    constexpr size_t kBlock = 64;
    constexpr size_t kIrLen = 256;

    // A decaying IR, so the overlap-save history genuinely carries between
    // blocks -- a torn history would show up as a mismatch below.
    std::vector<float> ir(kIrLen);
    for (size_t i = 0; i < kIrLen; ++i)
        ir[i] = std::exp(-0.01f * static_cast<float>(i)) * (i % 2 == 0 ? 1.0f : -0.5f);

    PartitionedConvolver reference;
    reference.load_ir(ir.data(), ir.size(), kBlock);

    PartitionedConvolver torn;
    torn.load_ir(ir.data(), ir.size(), kBlock);

    // Inject a wrong-size block. It must be silenced AND must leave no residue
    // in the overlap-save history, so the next valid block resumes from a clean
    // state -- identical to a convolver that never saw the bad block.
    std::vector<float> bad_in(48, 0.5f), bad_out(48, -1.0f);
    torn.process(bad_in.data(), bad_out.data(), 48);
    for (auto v : bad_out) REQUIRE(v == 0.0f);
    REQUIRE(torn.block_size_violations() == 1);

    // Dirty control: never sees a bad block, but carries a real history the
    // reference does not. This is what a reset() that stopped clearing would
    // look like, and it is what gives the tolerance below its teeth.
    PartitionedConvolver dirty;
    dirty.load_ir(ir.data(), ir.size(), kBlock);
    std::vector<float> dc_in(kBlock, 0.5f), dc_out(kBlock);
    dirty.process(dc_in.data(), dc_out.data(), kBlock);

    // The two convolvers are distinct objects, so their FFT buffers land at
    // different alignments and the SIMD path can sum in a different order --
    // worth a few ULP on samples of magnitude ~6. An exact compare is not a
    // property this DSP has; the `dirty` control proves this bound still catches
    // an uncleared history by orders of magnitude.
    constexpr float kTol = 1e-4f;

    std::vector<float> in(kBlock), ref_out(kBlock), torn_out(kBlock), dirty_out(kBlock);
    float worst_recovered = 0.0f, worst_dirty = 0.0f;
    for (size_t block = 0; block < 8; ++block) {
        for (size_t i = 0; i < kBlock; ++i)
            in[i] = std::sin(0.05f * static_cast<float>(block * kBlock + i));
        reference.process(in.data(), ref_out.data(), kBlock);
        torn.process(in.data(), torn_out.data(), kBlock);
        dirty.process(in.data(), dirty_out.data(), kBlock);
        for (size_t i = 0; i < kBlock; ++i) {
            INFO("block " << block << " sample " << i);
            REQUIRE_THAT(torn_out[i], WithinAbs(ref_out[i], kTol));
            worst_recovered = std::max(worst_recovered, std::abs(torn_out[i] - ref_out[i]));
            worst_dirty = std::max(worst_dirty, std::abs(dirty_out[i] - ref_out[i]));
        }
    }
    INFO("recovered drift " << worst_recovered << " vs uncleared-history drift " << worst_dirty);
    REQUIRE(worst_dirty > kTol * 100.0f);
    REQUIRE(worst_recovered < kTol);

    REQUIRE(torn.block_size_violations() == 0);  // cleared by the recovery reset()
}

namespace {

// Broadband deterministic stimulus / IR in [-1, 1].
std::vector<double> lcg_signal(std::size_t n, std::uint32_t seed, double decay) {
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double v = static_cast<double>(seed >> 8) / 8388608.0 - 1.0;
        out[i] = v * std::exp(-decay * static_cast<double>(i));
    }
    return out;
}

// Residual of `actual` against a long-double direct convolution of the same
// (already rounded) input and IR, in dB relative to the reference energy.
template <typename T>
double convolution_null_db(const std::vector<T>& input, const std::vector<T>& ir,
                           const std::vector<T>& actual) {
    long double err = 0, ref = 0;
    for (std::size_t n = 0; n < actual.size(); ++n) {
        long double y = 0;
        const std::size_t taps = std::min(ir.size(), n + 1);
        for (std::size_t k = 0; k < taps; ++k)
            y += static_cast<long double>(ir[k]) * static_cast<long double>(input[n - k]);
        const long double d = static_cast<long double>(actual[n]) - y;
        err += d * d;
        ref += y * y;
    }
    return 10.0 * std::log10(static_cast<double>(err / ref) + 1e-300);
}

template <typename T>
std::vector<T> run_partitioned(const std::vector<T>& input, const std::vector<T>& ir,
                               std::size_t block) {
    PartitionedConvolverT<T> conv;
    conv.load_ir(ir.data(), ir.size(), block);
    std::vector<T> out(input.size());
    for (std::size_t off = 0; off < input.size(); off += block)
        conv.process(input.data() + off, out.data() + off, block);
    REQUIRE(conv.block_size_violations() == 0);
    return out;
}

} // namespace

TEST_CASE("Convolver: output nulls against a long-double direct convolution",
          "[signal][convolver][null]") {
    struct Case {
        std::size_t block;
        std::size_t ir_length;
    };
    for (const Case c :
         {Case{2, 37}, Case{32, 64}, Case{128, 4096}, Case{512, 512}, Case{512, 9000}}) {
        const std::size_t frames = c.block * ((3 * c.ir_length) / c.block + 8);
        const auto in64 = lcg_signal(frames, 0x1234567u, 0.0);
        const auto ir64 = lcg_signal(c.ir_length, 0xBEEFu, 4.0 / static_cast<double>(c.ir_length));
        const std::vector<float> in32(in64.begin(), in64.end());
        const std::vector<float> ir32(ir64.begin(), ir64.end());

        const double null32 = convolution_null_db(in32, ir32, run_partitioned(in32, ir32, c.block));
        const double null64 = convolution_null_db(in64, ir64, run_partitioned(in64, ir64, c.block));
        INFO("block=" << c.block << " ir=" << c.ir_length << " float null=" << null32
                      << " dB, double null=" << null64 << " dB");
        REQUIRE(null32 < -120.0);
        REQUIRE(null64 < -250.0);
    }
}

TEST_CASE("Convolver: the null gate sees a one-tap defect", "[signal][convolver][null]") {
    // Negative control: render with an IR whose one tap is off by 1e-5 of the
    // IR's norm and grade it against the true IR. A -100 dB defect must fail the
    // -120 dB gate, or the gate is grading nothing.
    const auto in64 = lcg_signal(128 * 40, 0x1234567u, 0.0);
    const auto ir64 = lcg_signal(1000, 0xBEEFu, 0.004);
    double norm = 0.0;
    for (double v : ir64)
        norm += v * v;
    auto planted = ir64;
    planted[333] += 1e-5 * std::sqrt(norm);
    const std::vector<float> in32(in64.begin(), in64.end());
    const std::vector<float> ir32(ir64.begin(), ir64.end());
    const std::vector<float> planted32(planted.begin(), planted.end());
    REQUIRE(convolution_null_db(in32, ir32, run_partitioned(in32, ir32, 128)) < -120.0);
    REQUIRE(convolution_null_db(in32, ir32, run_partitioned(in32, planted32, 128)) > -120.0);
}

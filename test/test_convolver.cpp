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

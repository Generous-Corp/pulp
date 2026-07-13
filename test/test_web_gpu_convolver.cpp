#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/web/web_gpu_convolver.hpp>
#include <pulp/render/gpu_compute.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

using namespace pulp::gpu_audio;
using pulp::audio::BufferView;

namespace {

constexpr uint32_t kCh = 2;
constexpr uint32_t kBlock = 64;
constexpr uint32_t kSr = 48000;
constexpr int kIrLen = 300;

// A lane with no compute device must SKIP, out loud, and never `return` — Catch2
// records a bare early return as a PASS, so a GPU-less runner would report this
// whole file green having asserted nothing about WebGpuConvolver at all.
constexpr const char* kNoGpu =
    "no GPU compute device available (Skia/Dawn not built, or no adapter)";

std::vector<float> make_ir() {
    std::vector<float> ir(kIrLen);
    for (int i = 0; i < kIrLen; ++i)
        ir[i] = std::cos(0.05f * static_cast<float>(i)) * std::exp(-0.01f * static_cast<float>(i));
    return ir;
}

// Per-channel test signal: channel 1 is deliberately NOT a scaled copy of
// channel 0, so a slot mix-up between channels or between blocks shows up as a
// mismatch rather than cancelling out.
float sample_at(uint32_t ch, uint32_t i) {
    const float t = static_cast<float>(i);
    return ch == 0 ? std::sin(0.10f * t) + 0.3f * std::sin(0.37f * t)
                   : 0.6f * std::sin(0.21f * t + 0.7f) - 0.2f * std::cos(0.05f * t);
}

std::vector<float> make_block(uint32_t b) {
    std::vector<float> planar(static_cast<std::size_t>(kCh) * kBlock, 0.0f);
    for (uint32_t ch = 0; ch < kCh; ++ch)
        for (uint32_t i = 0; i < kBlock; ++i)
            planar[ch * kBlock + i] = sample_at(ch, b * kBlock + i);
    return planar;
}

// The shipped BLOCKING path, run over an explicit list of block indices. Passing
// a list (rather than 0..N) is what lets the expiry case build its reference: a
// block whose GPU result never lands must leave the overlap-add history exactly
// as if that block had never been submitted at all.
std::vector<std::vector<float>> reference_blocks(const std::vector<float>& ir,
                                                 const std::vector<uint32_t>& seqs) {
    GpuConvolver node(kCh, kBlock, kSr, ir);
    REQUIRE(node.prepare());
    if (!node.gpu_available()) return {};

    std::vector<std::vector<float>> out;
    std::vector<float> in_planar, out_planar(static_cast<std::size_t>(kCh) * kBlock, 0.0f);
    for (uint32_t b : seqs) {
        in_planar = make_block(b);
        const float* in_ptrs[kCh] = {in_planar.data(), in_planar.data() + kBlock};
        float* out_ptrs[kCh] = {out_planar.data(), out_planar.data() + kBlock};
        BufferView<const float> in(in_ptrs, kCh, kBlock);
        BufferView<float> ov(out_ptrs, kCh, kBlock);
        node.process_block(in, ov, kBlock);
        out.push_back(out_planar);
    }
    return out;
}

// A GpuCompute the WebGpuConvolver can drive, or nullptr when the machine has no
// compute device (headless CI) — every case below then skips cleanly.
std::unique_ptr<pulp::render::GpuCompute> make_device() {
    auto gpu = pulp::render::GpuCompute::create();
    if (!gpu || !gpu->initialize_standalone()) return nullptr;
    return gpu;
}

void require_close(const std::vector<float>& got, const std::vector<float>& want) {
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < got.size(); ++i)
        REQUIRE(std::abs(got[i] - want[i]) < 1e-5f);
}

constexpr auto kGenerous = std::chrono::microseconds(2'000'000);

}  // namespace

// The CPU-computed IR spectrum and the submit/collect overlap-add must produce
// bit-comparable audio to the shipped blocking GpuConvolver — the async path is
// a scheduling change, not a DSP change.
TEST_CASE("WebGpuConvolver matches the blocking GpuConvolver", "[gpu_audio][web][gpu]") {
    const auto ir = make_ir();
    constexpr uint32_t kBlocks = 16;

    std::vector<uint32_t> seqs;
    for (uint32_t b = 0; b < kBlocks; ++b) seqs.push_back(b);
    const auto want = reference_blocks(ir, seqs);
    if (want.empty()) SKIP(kNoGpu);

    auto gpu = make_device();
    if (!gpu) SKIP(kNoGpu);

    WebGpuConvolver conv;
    REQUIRE(conv.prepare(gpu.get(), static_cast<int>(kSr), static_cast<int>(kBlock),
                         static_cast<int>(kCh), ir.data(), kIrLen));

    std::vector<std::vector<float>> got;
    std::vector<uint32_t> got_seq;
    const auto sink = [&](uint32_t seq, const float* out, bool ok) {
        REQUIRE(ok);
        got_seq.push_back(seq);
        got.emplace_back(out, out + static_cast<std::size_t>(kCh) * kBlock);
    };

    for (uint32_t b = 0; b < kBlocks; ++b) {
        const auto in = make_block(b);
        // At the depth cap a submit is refused — that is the design (a browser
        // worker sheds blocks, it does not queue them). Here we are not the audio
        // thread, so we make room by harvesting instead of dropping the block.
        int guard = 0;
        while (!conv.submit(b, in.data(), static_cast<int>(kBlock), kGenerous)) {
            conv.collect(sink);
            REQUIRE(++guard < 100'000);  // never spin forever on a wedged device
        }
        conv.collect(sink);
    }
    // Drain whatever is still in flight. collect() never spins, so the harvest
    // loop lives here (in the browser it is the worker's event loop).
    while (conv.in_flight() > 0) conv.collect(sink);

    REQUIRE(got.size() == kBlocks);
    for (uint32_t b = 0; b < kBlocks; ++b) {
        REQUIRE(got_seq[b] == b);
        require_close(got[b], want[b]);
    }
    REQUIRE(conv.submits() == kBlocks);
    REQUIRE(conv.resolves() == kBlocks);
    REQUIRE(conv.expired() == 0);
    REQUIRE(conv.failed() == 0);
}

// Pipeline depth: several blocks in flight at once must come back in submission
// order with uncontaminated payloads — i.e. each block used its OWN scratch slot
// and the sequential overlap-add carry was applied on collection, not submission.
TEST_CASE("WebGpuConvolver delivers pipelined blocks in order", "[gpu_audio][web][gpu]") {
    const auto ir = make_ir();
    const auto want = reference_blocks(ir, {0, 1, 2});
    if (want.empty()) SKIP(kNoGpu);

    auto gpu = make_device();
    if (!gpu) SKIP(kNoGpu);

    WebGpuConvolver conv;
    REQUIRE(conv.prepare(gpu.get(), static_cast<int>(kSr), static_cast<int>(kBlock),
                         static_cast<int>(kCh), ir.data(), kIrLen));
    REQUIRE(conv.depth() >= 3);

    // Three blocks submitted BEFORE any collect.
    std::vector<std::vector<float>> ins;
    for (uint32_t b = 0; b < 3; ++b) {
        ins.push_back(make_block(b));
        REQUIRE(conv.submit(b, ins.back().data(), static_cast<int>(kBlock), kGenerous));
    }
    REQUIRE(conv.in_flight() == 3);

    std::vector<uint32_t> got_seq;
    std::vector<std::vector<float>> got;
    const auto sink = [&](uint32_t seq, const float* out, bool ok) {
        REQUIRE(ok);
        got_seq.push_back(seq);
        got.emplace_back(out, out + static_cast<std::size_t>(kCh) * kBlock);
    };
    while (conv.in_flight() > 0) conv.collect(sink);

    REQUIRE(got_seq == std::vector<uint32_t>{0, 1, 2});
    for (uint32_t b = 0; b < 3; ++b) require_close(got[b], want[b]);
}

// An unreachable deadline must expire the block — reported ok = false — WITHOUT
// touching the overlap-add carry. The next block therefore convolves exactly as
// if the expired one had never been submitted; a carry mutated by a phantom
// result would smear across the whole IR tail, not just one block.
TEST_CASE("WebGpuConvolver expiry does not corrupt the overlap-add history",
          "[gpu_audio][web][gpu]") {
    const auto ir = make_ir();
    // Reference: blocks 0 and 1 land, block 2 never does, block 3 lands.
    const auto want = reference_blocks(ir, {0, 1, 3});
    if (want.empty()) SKIP(kNoGpu);

    auto gpu = make_device();
    if (!gpu) SKIP(kNoGpu);

    WebGpuConvolver conv;
    REQUIRE(conv.prepare(gpu.get(), static_cast<int>(kSr), static_cast<int>(kBlock),
                         static_cast<int>(kCh), ir.data(), kIrLen));

    std::vector<uint32_t> got_seq;
    std::vector<bool> got_ok;
    std::vector<std::vector<float>> got;
    const auto sink = [&](uint32_t seq, const float* out, bool ok) {
        got_seq.push_back(seq);
        got_ok.push_back(ok);
        got.emplace_back(out, out + static_cast<std::size_t>(kCh) * kBlock);
    };

    std::vector<std::vector<float>> ins;
    for (uint32_t b = 0; b < 4; ++b) {
        ins.push_back(make_block(b));
        // Block 2 gets a deadline that has effectively already passed by the time
        // the first poll runs (a GPU map round-trip is ~0.5 ms).
        const auto deadline = (b == 2) ? std::chrono::microseconds(1) : kGenerous;
        REQUIRE(conv.submit(b, ins.back().data(), static_cast<int>(kBlock), deadline));
        while (conv.in_flight() > 0) conv.collect(sink);
    }

    REQUIRE(got_seq == std::vector<uint32_t>{0, 1, 2, 3});
    if (got_ok[2]) {
        // The map beat its 1 µs deadline (a fast, idle GPU). Nothing to assert
        // about expiry then — say so rather than pretend the case ran.
        WARN("block 2 resolved before its 1 us deadline; expiry path not exercised");
        return;
    }

    REQUIRE(conv.expired() == 1);
    for (float v : got[2]) REQUIRE(v == 0.0f);   // an expired block emits silence
    require_close(got[0], want[0]);
    require_close(got[1], want[1]);
    require_close(got[3], want[2]);              // block 3 vs the {0,1,3} reference
}

// At the depth cap the pipeline refuses new work — cheaply, and without touching
// the heap. A browser worker that fell behind must shed blocks, not queue them.
TEST_CASE("WebGpuConvolver rejects submits at the depth cap", "[gpu_audio][web][gpu]") {
    const auto ir = make_ir();
    auto gpu = make_device();
    if (!gpu) SKIP(kNoGpu);

    WebGpuConvolver conv;
    REQUIRE(conv.prepare(gpu.get(), static_cast<int>(kSr), static_cast<int>(kBlock),
                         static_cast<int>(kCh), ir.data(), kIrLen));

    const uint32_t depth = conv.depth();
    std::vector<std::vector<float>> ins;
    for (uint32_t b = 0; b < depth; ++b) {
        ins.push_back(make_block(b));
        REQUIRE(conv.submit(b, ins.back().data(), static_cast<int>(kBlock), kGenerous));
    }
    REQUIRE(conv.in_flight() == depth);

    const auto over = make_block(depth);
    bool admitted = true;
    bool allocated = true;
    {
        // The probe scope holds nothing but the call: a Catch2 assertion allocates
        // (expression decomposition), so asserting inside it would measure the
        // harness, not the convolver.
        pulp::test::RtAllocationProbe probe;
        admitted = conv.submit(depth, over.data(), static_cast<int>(kBlock), kGenerous);
        allocated = probe.saw_allocation();
    }
    REQUIRE_FALSE(admitted);
    REQUIRE_FALSE(allocated);
    REQUIRE(conv.in_flight() == depth);
    REQUIRE(conv.submits() == depth);

    // A wrong block size is rejected the same way — never silently reshaped.
    REQUIRE_FALSE(conv.submit(depth, over.data(), static_cast<int>(kBlock) / 2, kGenerous));
}

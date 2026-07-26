#pragma once

// The synthesis family's bake-layer catalog suite.
//
// Both members are exercised over the REAL production path — build the graph,
// `bake()` it, `claim_param_injection()`, push a `ParameterEventQueue` through
// `ParamInjector`, and render through the routed executor. A param that is
// declared but never reaches the DSP is the failure mode this file exists to
// catch, so every declared param is asserted to MOVE THE AUDIO, not merely to
// be accepted by the injector.
//
// ── Two fixtures, and why there are two ───────────────────────────────────
//
// The additive node is 1-in/1-out and uses the shared `BakedNodeFixture`.
//
// The vocoder is 2-in/1-out, and the shared fixture structurally cannot express
// that: it wires `in.port → node.port` AND `node.port → out.port` over the same
// `Channels` loop, so a node whose input and output counts differ fails its own
// `REQUIRE(connect(...))` on the first mismatched port. `VocoderGraph` below is
// the smallest thing that builds an asymmetric graph — it borrows the shared
// fixture's `immediate()` and reuses its lifecycle verbatim, and it is NOT a
// second copy of the fixture's conveniences. (Reported upward as a fixture gap
// rather than fixed here: `baked_node_fixture.hpp` is not this module's file.)

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_synthesis_catalog.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

using namespace pulp;
using namespace pulp::host;
using pulp::test::BakedNodeFixture;
using pulp::test::immediate;
using pulp::test::ReusableRenderer;

namespace additive = pulp::host::synthesis::additive;
namespace vocoder = pulp::host::synthesis::vocoder;
namespace cyclic = pulp::host::synthesis::cyclic;
namespace granular = pulp::host::synthesis::granular;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 512;
constexpr double kPi = 3.14159265358979323846;

/// The DFT bin spacing of one block. Every additive probe tone is an integer
/// multiple of this, so a rectangular coherent DFT over one block is
/// leakage-free — including at the organ's 0.5 ratio, which is why the
/// multiplier below is even.
constexpr double kBinHz = kSr / kFrames;              // 93.75 Hz
constexpr double kAdditiveF0 = 4.0 * kBinHz;          // 375 Hz; f0/2 lands on bin 2

/// Hann-windowed coherent magnitude at an arbitrary frequency. The vocoder's
/// band centres are geometric and land nowhere near a DFT bin, so its probes
/// cannot use the shared rectangular helper; windowing removes the leakage a
/// non-integer number of periods would otherwise smear across the spectrum.
///
/// Never a peak-sample measurement: at 8 kHz and 48 kHz there are six samples
/// per cycle and none of them lands on the crest, which under-reads by 1.25 dB
/// and looks exactly like a filter that is not flat.
double windowed_magnitude(const std::vector<float>& x, double hz) {
    const auto n = x.size();
    std::complex<double> acc{0.0, 0.0};
    double window_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        const double theta = -2.0 * kPi * hz * static_cast<double>(i) / kSr;
        acc += w * static_cast<double>(x[i]) *
               std::complex<double>(std::cos(theta), std::sin(theta));
        window_sum += w;
    }
    return 2.0 * std::abs(acc) / window_sum;
}

std::vector<float> dc_block_signal(int frames, float value) {
    return std::vector<float>(static_cast<std::size_t>(frames), value);
}

std::vector<float> noise_block(int frames, float amplitude, std::uint32_t seed) {
    std::vector<float> v(static_cast<std::size_t>(frames), 0.0f);
    std::uint32_t s = seed == 0u ? 1u : seed;
    for (auto& x : v) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        x = amplitude * (static_cast<float>(static_cast<double>(s) / 2147483648.0) - 1.0f);
    }
    return v;
}

double peak_of(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) p = std::max(p, static_cast<double>(std::abs(v)));
    return p;
}

// ── The additive node under the shared fixture ────────────────────────────

/// A gated fixture: the additive node's port 0 is a gate CV, so "render" here
/// means "hold the gate high and let the bank settle".
struct GatedBank {
    BakedNodeFixture<1> fixture;
    ParamInjector injector;

    explicit GatedBank(const CustomNodeType& type)
        : fixture(type, kSr, kFrames), injector(fixture.claim_injector()) {
        REQUIRE(injector.valid());
    }

    // ONE INJECTION PER RENDER. The injector's mailbox holds a single queue, so
    // a second `inject()` before the next render REPLACES the first rather than
    // merging with it — silently, with `InjectStatus::Ok` both times. The first
    // draft of this file set a batch and then set one more param before
    // rendering, and every value in the batch was lost; what it measured was the
    // node running on its defaults, which looks like a node that ignores its
    // params. Build one queue per render.
    void set(state::ParamID id, float value) {
        state::ParameterEventQueue q;
        REQUIRE(q.push(immediate(id, value)));
        REQUIRE(injector.inject(q) == InjectStatus::Ok);
    }

    void set_all(const std::vector<std::pair<state::ParamID, float>>& values) {
        state::ParameterEventQueue q;
        for (const auto& [id, v] : values) REQUIRE(q.push(immediate(id, v)));
        REQUIRE(injector.inject(q) == InjectStatus::Ok);
    }

    /// Holds the gate high and returns the settled block.
    std::vector<float> settled(int blocks = 24) {
        const auto gate = dc_block_signal(kFrames, 1.0f);
        return fixture.settle({gate}, blocks)[0];
    }
};

// ── The vocoder's asymmetric graph ────────────────────────────────────────

/// Bakes `in(2) → vocoder → out(1)` and prepares the processor. See the file
/// header for why this is not the shared fixture.
class VocoderGraph {
public:
    VocoderGraph() {
        const auto type = vocoder::make_vocoder_node();
        REQUIRE(graph_.register_custom_node_type(type));
        const auto in = graph_.add_input_node(2, "In");   // 0 = modulator, 1 = carrier
        node_ = graph_.add_custom_node(vocoder::kTypeId, 1, "Vocoder");
        REQUIRE(node_ != 0);
        const auto out = graph_.add_output_node(1, "Out");
        REQUIRE(graph_.connect(in, vocoder::kModulatorPort, node_, vocoder::kModulatorPort));
        REQUIRE(graph_.connect(in, vocoder::kCarrierPort, node_, vocoder::kCarrierPort));
        REQUIRE(graph_.connect(node_, 0, out, 0));
        graph_.set_canonical_executor_routing_enabled(true);
        REQUIRE(graph_.prepare(kSr, kFrames));

        result_ = bake(graph_);
        REQUIRE(result_.accepted);
        REQUIRE(result_.processor);
        REQUIRE(result_.reason == LowerRejectReason::None);

        format::PrepareContext pc;
        pc.sample_rate = kSr;
        pc.max_buffer_size = kFrames;
        pc.input_channels = 2;
        pc.output_channels = 1;
        result_.processor->prepare(pc);

        modulator_.assign(static_cast<std::size_t>(kFrames), 0.0f);
        carrier_.assign(static_cast<std::size_t>(kFrames), 0.0f);
        output_.assign(static_cast<std::size_t>(kFrames), 0.0f);
        in_ptrs_[0] = modulator_.data();
        in_ptrs_[1] = carrier_.data();
        out_ptrs_[0] = output_.data();
        ctx_.sample_rate = kSr;
        ctx_.num_samples = kFrames;

        injector_ = static_cast<BakedGraphProcessor*>(result_.processor.get())
                        ->claim_param_injection(node_);
        REQUIRE(injector_.valid());
    }

    void set(const std::vector<std::pair<state::ParamID, float>>& values) {
        state::ParameterEventQueue q;
        for (const auto& [id, v] : values) REQUIRE(q.push(immediate(id, v)));
        REQUIRE(injector_.inject(q) == InjectStatus::Ok);
    }

    /// Injects a pre-built queue. Separate from `set` so an allocation probe
    /// can wrap the injection without the queue's own construction inside it.
    void inject(state::ParameterEventQueue& q) { injector_.inject(q); }

    std::vector<float>& modulator() { return modulator_; }
    std::vector<float>& carrier() { return carrier_; }
    const std::vector<float>& output() const { return output_; }

    /// Renders one block from the current input buffers. Allocates nothing, so
    /// it is usable inside an allocation probe.
    void render() {
        audio::BufferView<const float> in_view(in_ptrs_.data(), 2u,
                                               static_cast<std::uint32_t>(kFrames));
        audio::BufferView<float> out_view(out_ptrs_.data(), 1u,
                                          static_cast<std::uint32_t>(kFrames));
        result_.processor->process(out_view, in_view, midi_in_, midi_out_, ctx_);
    }

    /// Fills both inputs from generators over an absolute sample index, renders
    /// `blocks`, and returns the concatenated tail of `keep` blocks.
    template <typename ModFn, typename CarFn>
    std::vector<float> run(int blocks, int keep, ModFn&& mod, CarFn&& car) {
        std::vector<float> tail;
        tail.reserve(static_cast<std::size_t>(keep * kFrames));
        for (int b = 0; b < blocks; ++b) {
            for (int k = 0; k < kFrames; ++k) {
                const long long i = static_cast<long long>(b) * kFrames + k;
                modulator_[static_cast<std::size_t>(k)] = mod(i);
                carrier_[static_cast<std::size_t>(k)] = car(i);
            }
            render();
            if (b >= blocks - keep) tail.insert(tail.end(), output_.begin(), output_.end());
        }
        return tail;
    }

private:
    SignalGraph graph_;
    LowerResult result_;
    NodeId node_ = 0;
    ParamInjector injector_{};
    std::vector<float> modulator_, carrier_, output_;
    std::array<const float*, 2> in_ptrs_{};
    std::array<float*, 1> out_ptrs_{};
    midi::MidiBuffer midi_in_{}, midi_out_{};
    format::ProcessContext ctx_{};
};

/// Bakes the granular node's asymmetric 1-in/2-out routing.
class GranularGraph {
  public:
    GranularGraph() {
        REQUIRE(graph_.register_custom_node_type(granular::make_granular_node()));
        const auto in = graph_.add_input_node(1, "In");
        node_ = graph_.add_custom_node(granular::kTypeId, 1, "Granular");
        REQUIRE(node_ != 0);
        const auto out = graph_.add_output_node(2, "Out");
        REQUIRE(graph_.connect(in, 0, node_, 0));
        REQUIRE(graph_.connect(node_, 0, out, 0));
        REQUIRE(graph_.connect(node_, 1, out, 1));
        graph_.set_canonical_executor_routing_enabled(true);
        REQUIRE(graph_.prepare(kSr, kFrames));
        result_ = bake(graph_);
        REQUIRE(result_.accepted);
        REQUIRE(result_.processor);

        format::PrepareContext pc;
        pc.sample_rate = kSr;
        pc.max_buffer_size = kFrames;
        pc.input_channels = 1;
        pc.output_channels = 2;
        result_.processor->prepare(pc);

        input_.assign(kFrames, 0.0f);
        left_.assign(kFrames, 0.0f);
        right_.assign(kFrames, 0.0f);
        in_ptrs_[0] = input_.data();
        out_ptrs_[0] = left_.data();
        out_ptrs_[1] = right_.data();
        ctx_.sample_rate = kSr;
        ctx_.num_samples = kFrames;
        injector_ = static_cast<BakedGraphProcessor*>(result_.processor.get())
                        ->claim_param_injection(node_);
        REQUIRE(injector_.valid());
    }

    void set(std::initializer_list<std::pair<state::ParamID, float>> values) {
        state::ParameterEventQueue q;
        for (const auto& [id, value] : values)
            REQUIRE(q.push(immediate(id, value)));
        REQUIRE(injector_.inject(q) == InjectStatus::Ok);
    }

    void render() {
        audio::BufferView<const float> in(in_ptrs_.data(), 1u, kFrames);
        audio::BufferView<float> out(out_ptrs_.data(), 2u, kFrames);
        result_.processor->process(out, in, midi_in_, midi_out_, ctx_);
    }

    std::vector<float>& input() {
        return input_;
    }
    const std::vector<float>& left() const {
        return left_;
    }
    const std::vector<float>& right() const {
        return right_;
    }

  private:
    SignalGraph graph_;
    LowerResult result_;
    NodeId node_ = 0;
    ParamInjector injector_{};
    std::vector<float> input_, left_, right_;
    std::array<const float*, 1> in_ptrs_{};
    std::array<float*, 2> out_ptrs_{};
    midi::MidiBuffer midi_in_{}, midi_out_{};
    format::ProcessContext ctx_{};
};

/// The vocoder's band centres, recomputed from the geometric law rather than
/// read back from the node — the same numbers the DSP's own suite checks.
double band_center_hz(int k, int bands = 16, double lo = 120.0, double hi = 7000.0) {
    const double r = std::pow(hi / lo, 1.0 / static_cast<double>(bands - 1));
    return lo * std::pow(r, static_cast<double>(k));
}

template <typename Fn> void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════
// Shape — the things a graph author sees before any audio happens
// ═════════════════════════════════════════════════════════════════════════





// ═════════════════════════════════════════════════════════════════════════
// Additive bank
// ═════════════════════════════════════════════════════════════════════════







// ═════════════════════════════════════════════════════════════════════════
// Vocoder
// ═════════════════════════════════════════════════════════════════════════

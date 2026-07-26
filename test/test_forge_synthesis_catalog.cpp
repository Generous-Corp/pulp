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

TEST_CASE("synthesis catalog nodes declare the shapes their families need",
          "[host][baked][param-injection][forge][synthesis]") {
    const auto organ = additive::make_additive_bank_node(additive::Voice::organ);
    const auto bell = additive::make_additive_bank_node(additive::Voice::bell);
    const auto voc = vocoder::make_vocoder_node();
    const auto cyc_short = cyclic::make_cyclic_stretch_node(cyclic::Regime::short_frame);
    const auto cyc_long = cyclic::make_cyclic_stretch_node(cyclic::Regime::long_frame);
    const auto grains = granular::make_granular_node();

    // The additive bank is a source with a gate: one CV in, one audio out.
    for (const auto& t : {organ, bell}) {
        CHECK(t.num_input_ports == 1);
        CHECK(t.num_output_ports == 1);
        CHECK(t.lowerable);
        CHECK(t.create);
        CHECK(t.process_instance_baked_param);
    }
    // The realization split really is two registered types, not one type with a
    // mode — which is the whole reason `inharmonicity` can be inert on one.
    CHECK(organ.type_id != bell.type_id);

    // The vocoder is the family's two-input member.
    CHECK(voc.num_input_ports == 2);
    CHECK(voc.num_output_ports == 1);
    CHECK(vocoder::kModulatorPort == 0);
    CHECK(vocoder::kCarrierPort == 1);

    CHECK(cyc_short.num_input_ports == 1);
    CHECK(cyc_short.num_output_ports == 1);
    CHECK(cyc_short.type_id != cyc_long.type_id);
    CHECK(grains.num_input_ports == 1);
    CHECK(grains.num_output_ports == 2);

    // Every declared param must have a unique id and a default inside its own
    // range — a default outside the range is silently clamped by the framework
    // and the node then starts somewhere its own table does not describe.
    for (const auto& t : {organ, bell, voc, cyc_short, cyc_long, grains}) {
        INFO("type " << t.type_id);
        std::vector<state::ParamID> ids;
        for (const auto& p : t.baked_params) {
            INFO("param " << p.id);
            CHECK(p.min_value < p.max_value);
            CHECK(p.default_value >= p.min_value);
            CHECK(p.default_value <= p.max_value);
            ids.push_back(p.id);
        }
        std::sort(ids.begin(), ids.end());
        CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    }
}

TEST_CASE("additive catalog publishes the DSP's honest AR minima",
          "[host][baked][forge][synthesis][range]") {
    const auto type = additive::make_additive_bank_node();
    auto range_for = [&](state::ParamID id) -> const CustomNodeBakedParam& {
        const auto it = std::find_if(type.baked_params.begin(), type.baked_params.end(),
                                     [=](const auto& p) { return p.id == id; });
        REQUIRE(it != type.baked_params.end());
        return *it;
    };
    CHECK(range_for(additive::kAttackMs).min_value ==
          static_cast<float>(signal::AdditiveBank::kAttackMinMs));
    CHECK(range_for(additive::kReleaseMs).min_value ==
          static_cast<float>(signal::AdditiveBank::kReleaseMinMs));
}

TEST_CASE("cyclic stretch catalog bakes both regimes and routes automation",
          "[host][baked][param-injection][forge][synthesis][cyclic]") {
    const auto input = pulp::test::sine_block(kFrames, 375.0, kSr, 0.4f);
    for (const auto regime : {cyclic::Regime::short_frame, cyclic::Regime::long_frame}) {
        BakedNodeFixture<1> fx(cyclic::make_cyclic_stretch_node(regime), kSr, kFrames);
        auto injector = fx.claim_injector();
        REQUIRE(injector.inject(immediate(cyclic::kMixPct, 0.0f)) == InjectStatus::Ok);
        const auto dry = fx.settle({input}, 8)[0];
        CHECK(peak_of(dry) > 0.3);

        REQUIRE(injector.inject(immediate(cyclic::kMixPct, 100.0f)) == InjectStatus::Ok);
        REQUIRE(injector.inject(immediate(cyclic::kStretch, 2.0f)) == InjectStatus::Ok);
        const auto wet = fx.settle({input}, 40)[0];
        CHECK(std::all_of(wet.begin(), wet.end(), [](float v) { return std::isfinite(v); }));
        CHECK(wet != dry);
    }
    CHECK(cyclic::cyclic_stretch_worst_case_gain() >= 1.0f);
}

TEST_CASE("granular catalog bakes its 1-to-2 live-ring path",
          "[host][baked][param-injection][forge][synthesis][granular]") {
    GranularGraph graph;
    graph.set({{granular::kDensityHz, 80.0f},
               {granular::kGrainMs, 50.0f},
               {granular::kPosition, 0.0f},
               {granular::kPanSpray, 1.0f},
               {granular::kLevelDb,
                static_cast<float>(signal::GranularEngine::kMaxLevelDb)},
               {granular::kMix, 1.0f}});
    std::uint32_t rng = 0x714ACu;
    double peak = 0.0;
    double stereo_difference = 0.0;
    for (int block = 0; block < 40; ++block) {
        for (auto& sample : graph.input()) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            sample = static_cast<float>(static_cast<double>(rng) / 2147483648.0 - 1.0);
        }
        graph.render();
        for (std::size_t i = 0; i < graph.left().size(); ++i) {
            REQUIRE(std::isfinite(graph.left()[i]));
            REQUIRE(std::isfinite(graph.right()[i]));
            peak = std::max(peak, static_cast<double>(std::abs(graph.left()[i])));
            stereo_difference =
                std::max(stereo_difference,
                         static_cast<double>(std::abs(graph.left()[i] - graph.right()[i])));
        }
    }
    CHECK(peak > 1e-4);
    CHECK(stereo_difference > 1e-5);
    CHECK(peak <= granular::granular_worst_case_gain());
    CHECK(granular::granular_worst_case_gain() ==
          static_cast<float>(signal::GranularEngine::kMaxGrainBudget) * 1.25f *
              static_cast<float>(signal::units::db_to_linear(
                  signal::GranularEngine::kMaxLevelDb)));
}

// ═════════════════════════════════════════════════════════════════════════
// Additive bank
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("additive node speaks only when its gate CV opens",
          "[host][baked][param-injection][forge][synthesis]") {
    // The gate is the reason port 0 exists. A bank whose onset never opens is
    // silent forever, so this is the difference between a working node and one
    // that bakes, injects, renders — and outputs nothing.
    GatedBank bank{additive::make_additive_bank_node()};
    bank.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                  {additive::kAttackMs, 1.0f},
                  {additive::kReleaseMs, 20.0f}});

    const auto gate_low = dc_block_signal(kFrames, 0.0f);
    const auto closed = bank.fixture.settle({gate_low}, 8)[0];
    INFO("gate low peak " << peak_of(closed));
    CHECK(peak_of(closed) == 0.0);

    const auto open = bank.settled(24);
    INFO("gate high peak " << peak_of(open));
    CHECK(peak_of(open) > 0.05);

    // Dropping the gate releases rather than cutting: the tail decays but the
    // block immediately after the drop is not silent.
    const auto releasing = bank.fixture.render({gate_low})[0];
    CHECK(peak_of(releasing) > 0.0);
    CHECK(peak_of(releasing) < peak_of(open));
    const auto decayed = bank.fixture.settle({gate_low}, 24)[0];
    INFO("after release peak " << peak_of(decayed));
    CHECK(peak_of(decayed) < 0.02 * peak_of(open));
}

TEST_CASE("additive partial_count moves the baked node's spectrum",
          "[host][baked][param-injection][forge][synthesis]") {
    // One of the two load-bearing params. The organ voice's drawbar ratios run
    // 0.5, 1, 1.5, 2, 3, 4, 5, 6 over its first eight partials, so a bank
    // truncated to four cannot produce anything at six times the fundamental
    // and a bank of eight must.
    auto ratio_at_six = [&](int count) {
        GatedBank bank{additive::make_additive_bank_node()};
        bank.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                      {additive::kSpectralTilt, 0.0f},  // flat, so the comparison is the table's
                      {additive::kMasterGainDb, 0.0f},
                      {additive::kAttackMs, 1.0f},
                      {additive::kPartialCount, static_cast<float>(count)}});
        const auto y = bank.settled(24);
        const double fundamental =
            pulp::test::harmonic_magnitude(y, 1, kAdditiveF0, kSr);
        const double sixth = pulp::test::harmonic_magnitude(y, 6, kAdditiveF0, kSr);
        REQUIRE(fundamental > 1e-4);
        return sixth / fundamental;
    };

    const double narrow = ratio_at_six(4);
    const double wide = ratio_at_six(8);
    INFO("6·f0 relative to f0: " << narrow << " with 4 partials, " << wide
                                 << " with 8 partials");
    CHECK(narrow < 1e-3);   // nothing in the table reaches ratio 6
    CHECK(wide > 0.02);     // the drawbar's 1⅓′ row does
    CHECK(wide > 20.0 * narrow);
}

TEST_CASE("additive inharmonicity moves the baked node's spectrum, and only where it should",
          "[host][baked][param-injection][forge][synthesis]") {
    // The other load-bearing param — and the test that justifies splitting the
    // voice into two registered types. Stretching a stiff string moves every
    // partial off its exact harmonic, so the coherent energy AT the harmonic
    // collapses. On the modal voice the same param is inert by design
    // (`VoiceTable::harmonic` is false: a bell's ratios are measurements of a
    // casting, not a string), so the same injection must change nothing.
    const float max_b = static_cast<float>(signal::AdditiveBank::kInharmonicityMax);

    auto sixth_harmonic = [&](additive::Voice voice, float b) {
        GatedBank bank{additive::make_additive_bank_node(voice)};
        bank.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                      {additive::kPartialCount, 8.0f},
                      {additive::kSpectralTilt, 0.0f},
                      {additive::kMasterGainDb, 0.0f},
                      {additive::kAttackMs, 1.0f},
                      {additive::kInharmonicity, b}});
        const auto y = bank.settled(24);
        return pulp::test::harmonic_magnitude(y, 6, kAdditiveF0, kSr);
    };

    const double organ_flat = sixth_harmonic(additive::Voice::organ, 0.0f);
    const double organ_stretched = sixth_harmonic(additive::Voice::organ, max_b);
    INFO("organ 6·f0: " << organ_flat << " at B = 0, " << organ_stretched << " at B = " << max_b);
    REQUIRE(organ_flat > 1e-4);
    CHECK(organ_stretched < 0.5 * organ_flat);

    // The bell's partial 5 sits at ratio 2.5, not 6, so the absolute level here
    // is small — what matters is that it does not MOVE.
    const double bell_flat = sixth_harmonic(additive::Voice::bell, 0.0f);
    const double bell_stretched = sixth_harmonic(additive::Voice::bell, max_b);
    INFO("bell 6·f0: " << bell_flat << " at B = 0, " << bell_stretched << " at B = " << max_b);
    CHECK(bell_stretched == Catch::Approx(bell_flat).margin(1e-9));
}

TEST_CASE("additive remaining params each move the baked node's audio",
          "[host][baked][param-injection][forge][synthesis]") {
    // The bar: a declared param that never reaches the DSP is the failure this
    // file exists to catch, so each one is injected and the audio compared.
    auto baseline_and_moved = [](state::ParamID id, float from, float to,
                                 const std::vector<std::pair<state::ParamID, float>>& setup) {
        GatedBank bank{additive::make_additive_bank_node()};
        auto base = setup;
        base.emplace_back(id, from);
        bank.set_all(base);
        const auto a = bank.settled(24);
        bank.set(id, to);
        const auto b = bank.settled(24);
        double diff = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i)
            diff = std::max(diff, static_cast<double>(std::abs(a[i] - b[i])));
        return diff;
    };

    const std::vector<std::pair<state::ParamID, float>> gated{
        {additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
        {additive::kPartialCount, 16.0f},
        {additive::kAttackMs, 1.0f}};

    struct Case {
        const char* name;
        state::ParamID id;
        float from;
        float to;
    };
    const Case cases[] = {
        {"fundamental_hz", additive::kFundamentalHz, 250.0f, 500.0f},
        {"spectral_tilt", additive::kSpectralTilt, 0.0f, -18.0f},
        {"master_gain_db", additive::kMasterGainDb, -6.0f, 0.0f},
        {"detune_cents", additive::kDetuneCents, 0.0f, 40.0f},
    };
    for (const auto& c : cases) {
        const double moved = baseline_and_moved(c.id, c.from, c.to, gated);
        INFO(c.name << ": largest sample difference " << moved);
        CHECK(moved > 1e-5);
    }

    // `attack_ms` and `release_ms` shape the ONSET, so re-injecting them into a
    // note whose attack finished twenty blocks ago moves nothing — correctly.
    // They are compared across a fresh gate instead.
    auto onset_with = [](state::ParamID id, float value) {
        GatedBank bank{additive::make_additive_bank_node()};
        bank.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                      {additive::kPartialCount, 16.0f},
                      {id, value}});
        return bank.settled(2);   // still inside a long attack
    };
    for (auto [name, id, from, to] :
         std::vector<std::tuple<const char*, state::ParamID, float, float>>{
             {"attack_ms", additive::kAttackMs, 1.0f, 900.0f},
             {"release_ms", additive::kReleaseMs, 50.0f, 5000.0f}}) {
        const auto a = onset_with(id, from);
        const auto b = onset_with(id, to);
        double diff = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i)
            diff = std::max(diff, static_cast<double>(std::abs(a[i] - b[i])));
        INFO(name << " across a fresh gate: largest sample difference " << diff);
        if (id == additive::kAttackMs) CHECK(diff > 1e-5);
        else CHECK(diff >= 0.0);   // release only bites after the gate drops; see below
    }

    // `release_ms` needs the gate to DROP before it can do anything.
    auto tail_with = [](float release_ms) {
        GatedBank bank{additive::make_additive_bank_node()};
        bank.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                      {additive::kPartialCount, 16.0f},
                      {additive::kReleaseMs, release_ms}});
        bank.settled(12);
        return bank.fixture.settle({dc_block_signal(kFrames, 0.0f)}, 8)[0];
    };
    INFO("release tails: " << peak_of(tail_with(50.0f)) << " vs " << peak_of(tail_with(5000.0f)));
    CHECK(peak_of(tail_with(5000.0f)) > 2.0 * peak_of(tail_with(50.0f)));

    // `envelope_mode` is provably inert on the ORGAN — every drawbar row is
    // sustained (`decay_ms = 0`), and `shared_ar` also yields a decay
    // coefficient of 1, so the two modes are the same arithmetic. It is the
    // BELL, whose rows carry real decay times, that the switch acts on.
    auto bell_with_mode = [](float mode) {
        GatedBank bank{additive::make_additive_bank_node(additive::Voice::bell)};
        bank.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                      {additive::kPartialCount, 16.0f},
                      {additive::kEnvelopeMode, mode}});
        return bank.settled(24);
    };
    const auto shared = bell_with_mode(0.0f);
    const auto per_partial = bell_with_mode(1.0f);
    double mode_diff = 0.0;
    for (std::size_t i = 0; i < shared.size(); ++i)
        mode_diff = std::max(mode_diff, static_cast<double>(std::abs(shared[i] - per_partial[i])));
    INFO("envelope_mode on the bell: largest sample difference " << mode_diff);
    CHECK(mode_diff > 1e-5);

    // `retrig_phase` is read only inside `retrigger()`, so it cannot be shown
    // by re-injecting into a sounding note — it needs a fresh gate edge. That
    // is exactly why it is safe as a param, and the test says so by construction.
    GatedBank coherent{additive::make_additive_bank_node()};
    coherent.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                      {additive::kPartialCount, 16.0f},
                      {additive::kAttackMs,
                       static_cast<float>(pulp::signal::AdditiveBank::kAttackMinMs)},
                      {additive::kRetrigPhase, 0.0f}});
    const auto stored = coherent.settled(4);
    GatedBank scattered{additive::make_additive_bank_node()};
    scattered.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                       {additive::kPartialCount, 16.0f},
                       {additive::kAttackMs,
                        static_cast<float>(pulp::signal::AdditiveBank::kAttackMinMs)},
                       {additive::kRetrigPhase, 2.0f}});  // seeded_random
    const auto random_phase = scattered.settled(4);
    double phase_diff = 0.0;
    for (std::size_t i = 0; i < stored.size(); ++i)
        phase_diff = std::max(phase_diff, static_cast<double>(
                                              std::abs(stored[i] - random_phase[i])));
    INFO("retrig_phase changed the attack by " << phase_diff);
    CHECK(phase_diff > 1e-5);
}

TEST_CASE("additive node never exceeds the bound its DSP suite asserts",
          "[host][baked][param-injection][forge][synthesis]") {
    // Series law 8: the registry number cites the DSP's own asserted invariant
    // rather than an estimate. The bank's crest normaliser bounds the partial
    // sum to 1 by construction, so the whole worst case is the master trim's
    // ceiling — and that is what is checked here, at the ceiling, because a
    // baked param can be automated anywhere in its declared range.
    const float bound = additive::additive_bank_worst_case_gain();
    CHECK(bound == Catch::Approx(static_cast<float>(
                                    signal::AdditiveBank::worst_case_gain()))
                       .epsilon(1e-6));

    for (auto voice : {additive::Voice::organ, additive::Voice::bell}) {
        GatedBank bank{additive::make_additive_bank_node(voice)};
        // Everything that could add level, at its ceiling: the widest bank, no
        // downward tilt, the loudest trim, and a coherent-phase retrigger,
        // which is the state that makes the crest bound an equality.
        bank.set_all({{additive::kFundamentalHz, 110.0f},
                      {additive::kPartialCount,
                       static_cast<float>(additive::kNodeMaxPartials)},
                      {additive::kSpectralTilt,
                       static_cast<float>(signal::AdditiveBank::kSpectralTiltMaxDbOct)},
                      {additive::kMasterGainDb,
                       static_cast<float>(signal::AdditiveBank::kMasterGainMaxDb)},
                      {additive::kRetrigPhase, 0.0f},
                      {additive::kAttackMs,
                       static_cast<float>(pulp::signal::AdditiveBank::kAttackMinMs)}});
        double worst = 0.0;
        const auto gate = dc_block_signal(kFrames, 1.0f);
        for (int b = 0; b < 32; ++b)
            worst = std::max(worst, peak_of(bank.fixture.render({gate})[0]));
        INFO("voice " << static_cast<int>(voice) << ": peak " << worst << " against bound "
                      << bound);
        CHECK(worst <= static_cast<double>(bound) * (1.0 + 1e-6));
    }
}

TEST_CASE("additive node renders deterministically and allocates nothing",
          "[host][baked][param-injection][forge][synthesis]") {
    // Two independent bakes rather than a reset: `BakedGraphProcessor` exposes
    // no public `reset()`, and a fresh bake is the stronger statement — it
    // covers the seeded RNG, the phase accumulators and the gate latch all
    // starting from the same place.
    auto render_once = [] {
        GatedBank bank{additive::make_additive_bank_node()};
        bank.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                      {additive::kPartialCount, 32.0f},
                      {additive::kRetrigPhase, 2.0f}});  // seeded_random — the path with an RNG
        return bank.settled(16);
    };
    CHECK(render_once() == render_once());

    GatedBank bank{additive::make_additive_bank_node()};
    bank.set_all({{additive::kFundamentalHz, static_cast<float>(kAdditiveF0)},
                  {additive::kPartialCount, 32.0f}});

    // The probe's silence only means something if the probe can speak, so a
    // known-allocating control runs first. A synthetic container control does
    // NOT work at -O3 (clang stack-promotes it under the C++14 allocation
    // elision rule), which is why this is an explicit `operator new`.
    {
        pulp::test::RtAllocationProbe control;
        void* block = ::operator new(static_cast<std::size_t>(kFrames) * sizeof(float));
        const auto seen = control.allocation_count();
        ::operator delete(block);
        REQUIRE(seen > 0);
    }

    ReusableRenderer<1> renderer(bank.fixture, {dc_block_signal(kFrames, 1.0f)});
    renderer.render();  // warm any lazy state
    state::ParameterEventQueue queue;
    REQUIRE(queue.push(immediate(additive::kFundamentalHz, 440.0f)));
    REQUIRE(queue.push(immediate(additive::kPartialCount, 64.0f)));
    REQUIRE(queue.push(immediate(additive::kMasterGainDb, -3.0f)));
    require_allocates_no_memory([&] {
        REQUIRE(bank.injector.inject(queue) == InjectStatus::Ok);
        for (int b = 0; b < 4; ++b) renderer.render();
    });
}

// ═════════════════════════════════════════════════════════════════════════
// Vocoder
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("vocoder node imposes the modulator's spectrum on the carrier",
          "[host][baked][param-injection][forge][synthesis]") {
    // THE bar for this node. A flat-spectrum carrier carries no timbre of its
    // own, so wherever the output's energy sits, the modulator put it there.
    // Moving the modulator between two band centres must move the output's
    // energy with it — a test that only checked "the output changed" would pass
    // an implementation that ignored the modulator and merely filtered.
    const double low_hz = band_center_hz(4);    // ≈ 355 Hz
    const double high_hz = band_center_hz(12);  // ≈ 3104 Hz

    auto energy_at = [&](double modulator_hz) {
        VocoderGraph g;
        g.set({{vocoder::kCarrierSource, 0.0f},   // external carrier
               {vocoder::kSibilanceMix, 0.0f},    // isolate the bank
               {vocoder::kDryWet, 1.0f},
               {vocoder::kBandCount, 16.0f},
               {vocoder::kFreqLoHz, 120.0f},
               {vocoder::kFreqHiHz, 7000.0f}});
        std::uint32_t rng = 0x2545F491u;
        const auto tail = g.run(48, 16,
                                [&](long long i) {
                                    return static_cast<float>(std::sin(
                                        2.0 * kPi * modulator_hz * static_cast<double>(i) / kSr));
                                },
                                [&](long long) {
                                    rng ^= rng << 13;
                                    rng ^= rng >> 17;
                                    rng ^= rng << 5;
                                    return 0.5f * (static_cast<float>(static_cast<double>(rng) /
                                                                     2147483648.0) -
                                                   1.0f);
                                });
        return std::pair{windowed_magnitude(tail, low_hz), windowed_magnitude(tail, high_hz)};
    };

    const auto [low_low, low_high] = energy_at(low_hz);
    const auto [high_low, high_high] = energy_at(high_hz);
    INFO("modulator at " << low_hz << " Hz → output " << low_low << " / " << low_high
                         << " ; modulator at " << high_hz << " Hz → output " << high_low << " / "
                         << high_high);

    // With the modulator low, the low band dominates; with it high, the high
    // band does. Asserted as a RATIO REVERSAL, which no fixed filter can fake.
    CHECK(low_low > low_high);
    CHECK(high_high > high_low);
    CHECK((low_low / (low_high + 1e-12)) > 4.0 * (high_low / (high_high + 1e-12)));
}

TEST_CASE("vocoder node's two inputs are not interchangeable",
          "[host][baked][param-injection][forge][synthesis]") {
    // A transposed node passes every measurement that feeds both ports the same
    // signal, which is exactly how one ships unusable with green tests. Two
    // independent proofs that port 0 is the modulator.
    auto run = [](bool swapped) {
        VocoderGraph g;
        g.set({{vocoder::kCarrierSource, 0.0f},
               {vocoder::kSibilanceMix, 0.0f},
               {vocoder::kDryWet, 1.0f}});
        std::uint32_t rng = 0x9E3779B9u;
        auto tone = [](long long i) {
            return static_cast<float>(
                std::sin(2.0 * kPi * band_center_hz(6) * static_cast<double>(i) / kSr));
        };
        auto noise = [&](long long) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return 0.5f * (static_cast<float>(static_cast<double>(rng) / 2147483648.0) - 1.0f);
        };
        return swapped ? g.run(24, 8, noise, tone) : g.run(24, 8, tone, noise);
    };
    const auto normal = run(false);
    const auto swapped = run(true);
    double difference = 0.0;
    for (std::size_t i = 0; i < normal.size(); ++i)
        difference = std::max(difference,
                              static_cast<double>(std::abs(normal[i] - swapped[i])));
    INFO("swapping the ports changed the output by " << difference);
    CHECK(difference > 1e-3);

    // The direct proof of WHICH port is which: at dry_wet = 0 the DSP passes
    // its DC-blocked MODULATOR, so the output must track port 0 and ignore
    // port 1.
    VocoderGraph g;
    g.set({{vocoder::kDryWet, 0.0f}, {vocoder::kCarrierSource, 0.0f}});
    const auto dry = g.run(8, 1,
                           [](long long i) {
                               return static_cast<float>(std::sin(
                                   2.0 * kPi * 1000.0 * static_cast<double>(i) / kSr));
                           },
                           [](long long i) {
                               return static_cast<float>(std::sin(
                                   2.0 * kPi * 3000.0 * static_cast<double>(i) / kSr));
                           });
    const double at_modulator = windowed_magnitude(dry, 1000.0);
    const double at_carrier = windowed_magnitude(dry, 3000.0);
    INFO("dry path: " << at_modulator << " at the modulator's tone, " << at_carrier
                      << " at the carrier's");
    CHECK(at_modulator > 0.9);
    CHECK(at_carrier < 0.01 * at_modulator);
}

TEST_CASE("vocoder params each move the baked node's audio",
          "[host][baked][param-injection][forge][synthesis]") {
    // Including the four applied at BLOCK rate behind a change guard — a guard
    // that never releases would make its param inert, and that failure looks
    // identical to a param that was never wired.
    auto moved_by = [](const std::vector<std::pair<state::ParamID, float>>& setup,
                       state::ParamID id, float from, float to) {
        auto render_with = [&](float value) {
            VocoderGraph g;
            auto all = setup;
            all.emplace_back(id, value);
            g.set(all);
            std::uint32_t rng = 0x51F0C0DEu;
            return g.run(24, 8,
                         [](long long i) {
                             return 0.8f * static_cast<float>(std::sin(
                                               2.0 * kPi * 220.0 * static_cast<double>(i) / kSr)) +
                                    0.4f * static_cast<float>(std::sin(
                                               2.0 * kPi * 2600.0 * static_cast<double>(i) / kSr));
                         },
                         [&](long long) {
                             rng ^= rng << 13;
                             rng ^= rng >> 17;
                             rng ^= rng << 5;
                             return 0.5f * (static_cast<float>(static_cast<double>(rng) /
                                                               2147483648.0) -
                                            1.0f);
                         });
        };
        const auto a = render_with(from);
        const auto b = render_with(to);
        double diff = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i)
            diff = std::max(diff, static_cast<double>(std::abs(a[i] - b[i])));
        return diff;
    };

    const std::vector<std::pair<state::ParamID, float>> external{
        {vocoder::kCarrierSource, 0.0f}, {vocoder::kDryWet, 1.0f}};
    const std::vector<std::pair<state::ParamID, float>> internal{
        {vocoder::kCarrierSource, 1.0f}, {vocoder::kDryWet, 1.0f}};
    // Pulse width does nothing to a SAW carrier — correctly. It needs the shape
    // it belongs to.
    const std::vector<std::pair<state::ParamID, float>> pulse{
        {vocoder::kCarrierSource, 1.0f}, {vocoder::kDryWet, 1.0f},
        {vocoder::kInternalWave, 1.0f}};

    struct Case {
        const char* name;
        const std::vector<std::pair<state::ParamID, float>>* setup;
        state::ParamID id;
        float from;
        float to;
    };
    const Case cases[] = {
        // Block-rate, change-guarded.
        {"band_count", &external, vocoder::kBandCount, 10.0f, 20.0f},
        {"freq_lo_hz", &external, vocoder::kFreqLoHz, 120.0f, 300.0f},
        {"freq_hi_hz", &external, vocoder::kFreqHiHz, 7000.0f, 4000.0f},
        {"attack_ms", &external, vocoder::kAttackMs, 0.5f, 40.0f},
        {"release_ms", &external, vocoder::kReleaseMs, 5.0f, 180.0f},
        // Per sample.
        {"carrier_source", &external, vocoder::kCarrierSource, 0.0f, 1.0f},
        {"internal_wave", &internal, vocoder::kInternalWave, 0.0f, 1.0f},
        {"internal_pw", &pulse, vocoder::kInternalPw, 0.5f, 0.1f},
        {"carrier_pitch_hz", &internal, vocoder::kCarrierPitchHz, 110.0f, 220.0f},
        {"noise_mix", &internal, vocoder::kNoiseMix, 0.0f, 1.0f},

        {"formant_shift_st", &external, vocoder::kFormantShiftSt, 0.0f, 12.0f},
        {"formant_freeze", &external, vocoder::kFormantFreeze, 0.0f, 1.0f},
        {"output_trim_db", &external, vocoder::kOutputTrimDb, 0.0f, 6.0f},
        {"dry_wet", &external, vocoder::kDryWet, 1.0f, 0.0f},
    };
    for (const auto& c : cases) {
        const double diff = moved_by(*c.setup, c.id, c.from, c.to);
        INFO(c.name << ": largest sample difference " << diff);
        CHECK(diff > 1e-5);
    }

    // Two params are gated by the VOICING DECISION, and the two-tone modulator
    // above is firmly voiced — so neither can show anything against it. Both
    // need material chosen for where it sits relative to the Schmitt trigger,
    // and the choice was measured rather than guessed:
    //
    //   * `unvoiced_sens` biases the raw decision by ±kSensSpan/2. Pure noise
    //     is already so far into the unvoiced region that the bias changes
    //     nothing (measured: u = 1 at every sensitivity). A 65/35 mix of tone
    //     and noise straddles the entry threshold — u = 0 at sensitivity 0 and
    //     u = 1 at sensitivity 1 — which is the only place the control has
    //     authority.
    //   * `sibilance_mix` is multiplied by `u`, so it needs u > 0 at all; pure
    //     noise is the reliable choice there for exactly the reason it is the
    //     wrong one above.
    {
        auto render_voicing = [](float sens, float sibilance, double noise_amount) {
            VocoderGraph g;
            g.set({{vocoder::kCarrierSource, 0.0f},
                   {vocoder::kDryWet, 1.0f},
                   {vocoder::kSibilanceMix, sibilance},
                   {vocoder::kUnvoicedSens, sens}});
            std::uint32_t m = 0x0BADu, c = 0xF00Du;
            return g.run(32, 8,
                         [&](long long i) {
                             m ^= m << 13; m ^= m >> 17; m ^= m << 5;
                             const double noise =
                                 static_cast<double>(m) / 2147483648.0 - 1.0;
                             const double tone =
                                 std::sin(2.0 * kPi * 180.0 * static_cast<double>(i) / kSr);
                             return static_cast<float>((1.0 - noise_amount) * tone +
                                                       noise_amount * noise);
                         },
                         [&](long long) {
                             c ^= c << 13; c ^= c >> 17; c ^= c << 5;
                             return 0.5f * (static_cast<float>(static_cast<double>(c) /
                                                               2147483648.0) - 1.0f);
                         });
        };
        auto largest_difference = [](const std::vector<float>& a, const std::vector<float>& b) {
            double d = 0.0;
            for (std::size_t i = 0; i < a.size(); ++i)
                d = std::max(d, static_cast<double>(std::abs(a[i] - b[i])));
            return d;
        };

        const double sens_diff = largest_difference(render_voicing(0.0f, 0.0f, 0.35),
                                                    render_voicing(1.0f, 0.0f, 0.35));
        INFO("unvoiced_sens across the decision boundary: " << sens_diff);
        CHECK(sens_diff > 1e-5);

        const double sibilance_diff = largest_difference(render_voicing(0.5f, 0.0f, 1.0),
                                                         render_voicing(0.5f, 1.0f, 1.0));
        INFO("sibilance_mix on unvoiced material: " << sibilance_diff);
        CHECK(sibilance_diff > 1e-5);
    }

    // `internal_wave` and `internal_pw` only exist when the internal carrier is
    // selected, so the negative half is worth pinning too: with an EXTERNAL
    // carrier they must do nothing at all.
    const double inert = moved_by(external, vocoder::kInternalWave, 0.0f, 1.0f);
    INFO("internal_wave under an external carrier moved the output by " << inert);
    CHECK(inert == 0.0);
}

TEST_CASE("vocoder node never exceeds its parameter-ceiling gain bound",
          "[host][baked][param-injection][forge][synthesis]") {
    // The DSP's own T-GAIN parks the sibilance path and the trim; the NODE
    // exposes both, so its bound has to include them. See the header comment on
    // `vocoder_worst_case_gain` for the assembly.
    const float bound = vocoder::vocoder_worst_case_gain(kSr);
    INFO("node bound " << bound << " against the DSP's pre-trim " << signal::Vocoder::kWorstCaseGain);
    CHECK(bound > signal::Vocoder::kWorstCaseGain);

    VocoderGraph g;
    g.set({{vocoder::kCarrierSource, 0.0f},
           {vocoder::kBandCount, 10.0f},   // the widest bands, which pass the most carrier
           {vocoder::kSibilanceMix, 1.0f},
           {vocoder::kUnvoicedSens, 1.0f},  // force the unvoiced decision, opening the sibilance path
           {vocoder::kOutputTrimDb,
            static_cast<float>(signal::Vocoder::kOutputTrimMaxDb)},
           {vocoder::kDryWet, 1.0f}});

    // BOTH inputs at full scale and no further — a worst-case GAIN is a ratio,
    // so it is only meaningful for |input| ≤ 1.
    //
    // The first draft drove the modulator at 1e4 to saturate every band's
    // `VcaT` control (which is what the DSP's own T-GAIN case does, and is
    // correct there), and measured a peak of 54467 against a bound of 10.4.
    // That is not a violated bound, it is a violated premise: the bank path is
    // amplitude-INSENSITIVE because the clamp absorbs the modulator's level,
    // while the SIBILANCE path is amplitude-PROPORTIONAL and passes the
    // modulator straight through. Feeding 1e4 measures 1e4 times the gain.
    // Worth knowing in its own right — a hot modulator makes the sibilance path
    // dominate the output — and noted on the node's bound.
    std::uint32_t mod_rng = 0x1234u;
    std::uint32_t car_rng = 0xABCDu;
    double worst = 0.0;
    for (int b = 0; b < 48; ++b) {
        for (int k = 0; k < kFrames; ++k) {
            mod_rng ^= mod_rng << 13; mod_rng ^= mod_rng >> 17; mod_rng ^= mod_rng << 5;
            car_rng ^= car_rng << 13; car_rng ^= car_rng >> 17; car_rng ^= car_rng << 5;
            g.modulator()[static_cast<std::size_t>(k)] =
                static_cast<float>(static_cast<double>(mod_rng) / 2147483648.0) - 1.0f;
            g.carrier()[static_cast<std::size_t>(k)] =
                static_cast<float>(static_cast<double>(car_rng) / 2147483648.0) - 1.0f;
        }
        g.render();
        if (b > 8) worst = std::max(worst, peak_of(g.output()));
    }
    INFO("measured peak " << worst << " against the node bound " << bound);
    CHECK(worst <= static_cast<double>(bound));
}

TEST_CASE("vocoder node renders deterministically and allocates nothing",
          "[host][baked][param-injection][forge][synthesis]") {
    auto render_twice = [] {
        VocoderGraph g;
        g.set({{vocoder::kCarrierSource, 1.0f},  // internal — the path with the seeded RNG
               {vocoder::kNoiseMix, 0.5f}});
        std::uint32_t rng = 0x7A7Au;
        auto mod = [&](long long) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return 0.4f * (static_cast<float>(static_cast<double>(rng) / 2147483648.0) - 1.0f);
        };
        return g.run(12, 4, mod, [](long long) { return 0.0f; });
    };
    CHECK(render_twice() == render_twice());

    VocoderGraph g;
    g.set({{vocoder::kCarrierSource, 1.0f}});
    for (std::size_t i = 0; i < g.modulator().size(); ++i) {
        g.modulator()[i] = 0.25f;
        g.carrier()[i] = -0.25f;
    }
    g.render();  // warm any lazy state, and let the change guards latch

    require_allocates_no_memory([&] {
        for (int b = 0; b < 4; ++b) g.render();
    });

    // ... and again across a structural change, which is the one that would
    // reallocate if `set_band_count` were not the loop-bound move it claims to
    // be. The injection is inside the probe deliberately.
    state::ParameterEventQueue queue;
    REQUIRE(queue.push(immediate(vocoder::kBandCount, 20.0f)));
    REQUIRE(queue.push(immediate(vocoder::kFreqLoHz, 90.0f)));
    REQUIRE(queue.push(immediate(vocoder::kFreqHiHz, 9000.0f)));
    require_allocates_no_memory([&] {
        g.inject(queue);
        for (int b = 0; b < 4; ++b) g.render();
    });
}

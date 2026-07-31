#pragma once

// Shared fixture for bake-layer catalog-node tests.
//
// Every catalog node's suite needs the same scaffolding: build a graph
// `in → node → out`, bake it, prepare the processor, push blocks through it,
// and inject parameter events over the real production path. Written per suite
// that is ~80 lines of boilerplate each, and the copies drift — by the third
// catalog node in the DSP series they already differed in channel count, in
// settle length, and in whether the render helper allocated (which quietly
// broke an RT-allocation probe by reporting the harness's allocations as the
// node's).
//
// So it lives here. A suite states its channel count and its analysis tone; the
// rest is shared.
//
// The one thing a suite must NOT take from here is the RT-probe render. Buffers
// and views have to be constructed OUTSIDE the probe, and a helper returning a
// `std::vector` by value cannot be — so `ReusableRenderer` exists for that path
// and the convenience `render()` is documented as unsuitable for it.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace pulp::test {

/// Bakes `in(N) → node → out(N)` and prepares the processor.
///
/// `Channels` is a template parameter rather than a constructor argument
/// because it decides the shape of every render helper below, and a suite that
/// mixed the two would be asserting something other than what it reads.
template <int Channels = 1>
class BakedNodeFixture {
public:
    static constexpr int channels = Channels;

    BakedNodeFixture(const host::CustomNodeType& type, double sample_rate, int frames)
        : sample_rate_(sample_rate), frames_(frames) {
        REQUIRE(graph_.register_custom_node_type(type));
        const auto in = graph_.add_input_node(Channels, "In");
        node_ = graph_.add_custom_node(type.type_id, 1, "Node");
        const auto out = graph_.add_output_node(Channels, "Out");
        for (int port = 0; port < Channels; ++port) {
            REQUIRE(graph_.connect(in, static_cast<host::PortIndex>(port), node_,
                                   static_cast<host::PortIndex>(port)));
            REQUIRE(graph_.connect(node_, static_cast<host::PortIndex>(port), out,
                                   static_cast<host::PortIndex>(port)));
        }
        graph_.set_canonical_executor_routing_enabled(true);
        REQUIRE(graph_.prepare(sample_rate_, frames_));

        result_ = host::bake(graph_);
        REQUIRE(result_.accepted);
        REQUIRE(result_.processor);
        REQUIRE(result_.reason == host::LowerRejectReason::None);

        format::PrepareContext pc;
        pc.sample_rate = sample_rate_;
        pc.max_buffer_size = frames_;
        pc.input_channels = Channels;
        pc.output_channels = Channels;
        result_.processor->prepare(pc);
    }

    host::BakedGraphProcessor& baked() {
        return *static_cast<host::BakedGraphProcessor*>(result_.processor.get());
    }

    host::NodeId node() const { return node_; }
    double sample_rate() const { return sample_rate_; }
    int frames() const { return frames_; }

    host::ParamInjector claim_injector() { return baked().claim_param_injection(node_); }

    /// Renders one block. Convenient, and deliberately NOT for use inside an
    /// allocation probe — it allocates its own output vectors, which a probe
    /// would attribute to the node under test.
    std::vector<std::vector<float>> render(const std::vector<std::vector<float>>& input) {
        std::vector<const float*> in_ptrs(static_cast<std::size_t>(Channels));
        for (int c = 0; c < Channels; ++c)
            in_ptrs[static_cast<std::size_t>(c)] =
                input[static_cast<std::size_t>(c % static_cast<int>(input.size()))].data();

        std::vector<std::vector<float>> output(
            static_cast<std::size_t>(Channels),
            std::vector<float>(static_cast<std::size_t>(frames_), 0.0f));
        std::vector<float*> out_ptrs(static_cast<std::size_t>(Channels));
        for (int c = 0; c < Channels; ++c)
            out_ptrs[static_cast<std::size_t>(c)] = output[static_cast<std::size_t>(c)].data();

        audio::BufferView<const float> in_view(in_ptrs.data(),
                                               static_cast<std::uint32_t>(Channels),
                                               static_cast<std::uint32_t>(frames_));
        audio::BufferView<float> out_view(out_ptrs.data(), static_cast<std::uint32_t>(Channels),
                                          static_cast<std::uint32_t>(frames_));
        midi::MidiBuffer midi_in, midi_out;
        format::ProcessContext ctx;
        ctx.sample_rate = sample_rate_;
        ctx.num_samples = frames_;
        result_.processor->process(out_view, in_view, midi_in, midi_out, ctx);
        return output;
    }

    /// Renders `blocks` copies of the same input and returns the last — the
    /// steady state of a stateful node.
    std::vector<std::vector<float>> settle(const std::vector<std::vector<float>>& input,
                                           int blocks = 16) {
        std::vector<std::vector<float>> out;
        for (int b = 0; b < blocks; ++b) out = render(input);
        return out;
    }

private:
    host::SignalGraph graph_;
    host::LowerResult result_;
    host::NodeId node_ = 0;
    double sample_rate_ = 48000.0;
    int frames_ = 128;
};

/// A renderer whose buffers are built once, so it can be driven from inside an
/// allocation probe. This is the counterpart to `BakedNodeFixture::render()`,
/// which cannot be.
template <int Channels = 1>
class ReusableRenderer {
public:
    ReusableRenderer(BakedNodeFixture<Channels>& fixture,
                     const std::vector<std::vector<float>>& input)
        : processor_(&fixture.baked()) {
        const auto frames = static_cast<std::size_t>(fixture.frames());
        for (int c = 0; c < Channels; ++c) {
            inputs_[static_cast<std::size_t>(c)] =
                input[static_cast<std::size_t>(c % static_cast<int>(input.size()))];
            outputs_[static_cast<std::size_t>(c)].assign(frames, 0.0f);
            in_ptrs_[static_cast<std::size_t>(c)] = inputs_[static_cast<std::size_t>(c)].data();
            out_ptrs_[static_cast<std::size_t>(c)] = outputs_[static_cast<std::size_t>(c)].data();
        }
        ctx_.sample_rate = fixture.sample_rate();
        ctx_.num_samples = fixture.frames();
        frames_ = fixture.frames();
    }

    /// Renders one block in place. Allocates nothing.
    void render() {
        audio::BufferView<const float> in_view(in_ptrs_.data(),
                                               static_cast<std::uint32_t>(Channels),
                                               static_cast<std::uint32_t>(frames_));
        audio::BufferView<float> out_view(out_ptrs_.data(), static_cast<std::uint32_t>(Channels),
                                          static_cast<std::uint32_t>(frames_));
        processor_->process(out_view, in_view, midi_in_, midi_out_, ctx_);
    }

    const std::vector<float>& output(int channel = 0) const {
        return outputs_[static_cast<std::size_t>(channel)];
    }

private:
    format::Processor* processor_ = nullptr;
    std::array<std::vector<float>, static_cast<std::size_t>(Channels)> inputs_{};
    std::array<std::vector<float>, static_cast<std::size_t>(Channels)> outputs_{};
    std::array<const float*, static_cast<std::size_t>(Channels)> in_ptrs_{};
    std::array<float*, static_cast<std::size_t>(Channels)> out_ptrs_{};
    midi::MidiBuffer midi_in_{}, midi_out_{};
    format::ProcessContext ctx_{};
    int frames_ = 128;
};

/// A parameter event applied immediately, with no ramp. Every catalog suite
/// wants this and every one of them had written it out.
inline state::ParameterEvent immediate(state::ParamID id, float value,
                                       std::int32_t offset = 0) {
    return {id, offset, value, /*ramp_duration_sample_frames=*/0};
}

/// A sine block whose period divides `frames` exactly, so a coherent DFT over
/// one block is leakage-free. A suite that picks its own tone should check that
/// property — several measurement bugs in this series came from a tone whose
/// crest is never sampled.
inline std::vector<float> sine_block(int frames, double tone_hz, double sample_rate,
                                     float amplitude) {
    std::vector<float> v(static_cast<std::size_t>(frames), 0.0f);
    for (int k = 0; k < frames; ++k)
        v[static_cast<std::size_t>(k)] =
            amplitude *
            static_cast<float>(std::sin(2.0 * std::numbers::pi * tone_hz * k / sample_rate));
    return v;
}

/// Coherent DFT magnitude at harmonic `k` of `tone_hz`. Exact when the analysis
/// window holds a whole number of periods.
inline double harmonic_magnitude(const std::vector<float>& x, int harmonic, double tone_hz,
                                 double sample_rate) {
    const double w = 2.0 * std::numbers::pi * harmonic * tone_hz / sample_rate;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

}  // namespace pulp::test

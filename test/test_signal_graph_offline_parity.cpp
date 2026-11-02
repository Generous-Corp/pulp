// Offline-equals-online equivalence harness for pulp::host::SignalGraph.
//
// SignalGraph nodes do not receive ProcessMode or transport, so the only thing
// that separates a live block-by-block ("online") render from an offline one is
// the block partitioning. For DETERMINISTIC nodes — output depends only on the
// current block's samples — rendering the same input at any block size must
// produce the same result. This harness proves that block-partitioning
// invariance:
//
//   A. ONLINE: a hand-written block loop at block size B drives graph.process()
//      directly (the independent reference).
//   B. OFFLINE same size: OfflineSignalGraphHost at block_frames == B must be
//      BIT-EXACT to A (the host reproduces a live loop exactly).
//   C. OFFLINE coarse: a DIFFERENT partition (B2 != B) must match A within a
//      small tolerance. For pure gain/sum graphs this is also bit-exact, but the
//      assertion is partition-robust by design.
//   D. OFFLINE one big block: block_frames == frame_count must match A.
//
// An EXEMPT control fixture (a node whose output legitimately depends on the
// block's sample count) demonstrates the declared-exemption path: the harness
// FLAGS it and EXCLUDES it from the equivalence requirement instead of failing.
//
// Everything that could perturb the comparison is pinned equal across runs:
// identical input, identical node gains, identical sample rate, and all executor
// routing opt-ins left OFF (the default legacy walk; anticipation in particular
// is intentionally NOT block-size invariant and is never enabled here).

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace audio = pulp::audio;

using pulp::host::OfflineSignalGraphConfig;
using pulp::host::OfflineSignalGraphHost;
using pulp::host::OfflineSignalGraphOptions;
using pulp::host::SignalGraph;

constexpr double kSr = 48000.0;
constexpr std::uint64_t kFrames = 1000; // not a multiple of any block: short tails

// Deterministic pseudo-ramp, identical across every run.
std::vector<std::vector<float>> make_input(std::size_t channels, std::size_t frames) {
    std::vector<std::vector<float>> in(channels, std::vector<float>(frames, 0.0f));
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t i = 0; i < frames; ++i) {
            const auto h = static_cast<std::uint32_t>((i * 2654435761u) ^ (c * 40503u));
            in[c][i] = (static_cast<float>(h & 0xFFFF) / 32768.0f - 1.0f) * (0.7f + 0.1f * c);
        }
    }
    return in;
}

audio::Buffer<float> to_buffer(const std::vector<std::vector<float>>& in) {
    audio::Buffer<float> buf(in.size(), in.empty() ? 0 : in[0].size());
    for (std::size_t c = 0; c < in.size(); ++c) {
        std::copy(in[c].begin(), in[c].end(), buf.channel(c).data());
    }
    return buf;
}

std::vector<std::vector<float>> to_channels(const pulp::audio::Buffer<float>& buf) {
    std::vector<std::vector<float>> out(buf.num_channels());
    for (std::size_t c = 0; c < buf.num_channels(); ++c) {
        const auto ch = buf.channel(c);
        out[c].assign(ch.begin(), ch.end());
    }
    return out;
}

// Independent ONLINE reference: drive a prepared graph block-by-block at B.
std::vector<std::vector<float>> run_online(SignalGraph& g, int block, std::size_t out_ch,
                                           const std::vector<std::vector<float>>& input) {
    const std::size_t in_ch = input.size();
    const std::size_t frames = input.empty() ? 0 : input[0].size();
    std::vector<std::vector<float>> out(out_ch, std::vector<float>(frames, 0.0f));
    const auto B = static_cast<std::size_t>(block);
    for (std::size_t pos = 0; pos < frames; pos += B) {
        const std::size_t n = std::min(B, frames - pos);
        std::vector<std::vector<float>> bi(in_ch, std::vector<float>(n, 0.0f));
        std::vector<std::vector<float>> bo(out_ch, std::vector<float>(n, 0.0f));
        for (std::size_t c = 0; c < in_ch; ++c) {
            std::copy_n(input[c].data() + pos, n, bi[c].data());
        }
        std::vector<const float*> ip;
        std::vector<float*> op;
        for (auto& c : bi)
            ip.push_back(c.data());
        for (auto& c : bo)
            op.push_back(c.data());
        pulp::audio::BufferView<const float> iv(ip.data(), in_ch, n);
        pulp::audio::BufferView<float> ov(op.data(), out_ch, n);
        g.process(ov, iv, static_cast<int>(n));
        for (std::size_t c = 0; c < out_ch; ++c) {
            std::copy_n(bo[c].data(), n, out[c].data() + pos);
        }
    }
    return out;
}

// Render the same graph offline at `block` via OfflineSignalGraphHost.
std::vector<std::vector<float>> run_offline(const std::function<void(SignalGraph&)>& build,
                                            int block, int in_ch, int out_ch,
                                            const audio::Buffer<float>& input) {
    SignalGraph g;
    build(g);
    OfflineSignalGraphHost host(g);
    OfflineSignalGraphConfig cfg;
    cfg.sample_rate = kSr;
    cfg.block_frames = block;
    cfg.input_channels = in_ch;
    cfg.output_channels = out_ch;
    REQUIRE(host.prepare(cfg));
    OfflineSignalGraphOptions opt;
    opt.frame_count = kFrames;
    opt.input = input.view();
    const auto result = host.render(opt);
    REQUIRE(result.ok);
    REQUIRE(result.audio.num_samples() == kFrames);
    return to_channels(result.audio);
}

void require_bit_exact(const std::vector<std::vector<float>>& a,
                       const std::vector<std::vector<float>>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t c = 0; c < a.size(); ++c) {
        REQUIRE(a[c].size() == b[c].size());
        for (std::size_t i = 0; i < a[c].size(); ++i) {
            REQUIRE(a[c][i] == b[c][i]);
        }
    }
}

void require_within(const std::vector<std::vector<float>>& a,
                    const std::vector<std::vector<float>>& b, float tol) {
    REQUIRE(a.size() == b.size());
    for (std::size_t c = 0; c < a.size(); ++c) {
        REQUIRE(a[c].size() == b[c].size());
        for (std::size_t i = 0; i < a[c].size(); ++i) {
            REQUIRE(std::fabs(a[c][i] - b[c][i]) <= tol);
        }
    }
}

bool max_abs_diff_exceeds(const std::vector<std::vector<float>>& a,
                          const std::vector<std::vector<float>>& b, float tol) {
    if (a.size() != b.size())
        return true;
    for (std::size_t c = 0; c < a.size(); ++c) {
        if (a[c].size() != b[c].size())
            return true;
        for (std::size_t i = 0; i < a[c].size(); ++i) {
            if (std::fabs(a[c][i] - b[c][i]) > tol)
                return true;
        }
    }
    return false;
}

// Harness-side fixture metadata. `block_size_dependent` is the DECLARED
// exemption: a fixture whose output legitimately varies with the block size is
// flagged and excluded from the equivalence requirement, never failed.
struct Fixture {
    std::string name;
    int in_channels = 0;
    int out_channels = 0;
    bool block_size_dependent = false;
    std::function<void(SignalGraph&)> build;
};

// Stereo chain + parallel arm summed at the output: pure gain/sum, fully
// deterministic and block-size invariant. Distinct gains make a fan-in
// summation-order or scaling bug observable.
Fixture make_gain_sum_fixture() {
    Fixture f;
    f.name = "stereo gain chain + fan-in sum";
    f.in_channels = 2;
    f.out_channels = 2;
    f.block_size_dependent = false;
    f.build = [](SignalGraph& g) {
        const auto in = g.add_input_node(2, "In");
        const auto g1 = g.add_gain_node("G1");
        const auto g2 = g.add_gain_node("G2");
        const auto g3 = g.add_gain_node("G3");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, g1, c));
            REQUIRE(g.connect(g1, c, g2, c));
            REQUIRE(g.connect(g2, c, out, c));
            REQUIRE(g.connect(in, c, g3, c));
            REQUIRE(g.connect(g3, c, out, c)); // fan-in sum at the output
        }
        REQUIRE(g.set_node_gain(g1, 0.5f));
        REQUIRE(g.set_node_gain(g2, 0.75f));
        REQUIRE(g.set_node_gain(g3, 0.3f));
    };
    return f;
}

// A custom node that scales by a per-block constant derived from the sample
// count: out = in * scale + num_samples. Its output legitimately depends on the
// block size, so re-partitioning changes the result — this is the EXEMPT
// control. Registered with an explicit harness-declared exemption.
Fixture make_block_size_dependent_fixture() {
    Fixture f;
    f.name = "block-size-dependent custom node (exempt)";
    f.in_channels = 1;
    f.out_channels = 1;
    f.block_size_dependent = true;
    f.build = [](SignalGraph& g) {
        pulp::host::CustomNodeType type;
        type.type_id = "pulp.test.block-size-dependent";
        type.version = 1;
        type.num_input_ports = 1;
        type.num_output_ports = 1;
        type.default_name = "BlockDep";
        type.process = [](pulp::audio::BufferView<float>& out,
                          const pulp::audio::BufferView<const float>& in, int num_samples) {
            float* o = out.channel_ptr(0);
            const float* i = in.channel_ptr(0);
            for (int s = 0; s < num_samples; ++s) {
                o[static_cast<std::size_t>(s)] =
                    i[static_cast<std::size_t>(s)] * 0.5f + static_cast<float>(num_samples);
            }
        };
        REQUIRE(g.register_custom_node_type(std::move(type)));
        const auto in = g.add_input_node(1, "In");
        const auto node = g.add_custom_node("pulp.test.block-size-dependent", "BlockDep");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, node, 0));
        REQUIRE(g.connect(node, 0, out, 0));
    };
    return f;
}

// Small, stateless plugin pair for the offline sidechain/PDC fixture. The
// first instance reports latency but forwards its main inputs unchanged; the
// second instance mixes its main and sidechain buses. Keeping the processing
// deterministic makes any mismatch attributable to graph routing or the
// offline block driver rather than plugin state.
class OfflineSidechainPlugin final : public pulp::host::PluginSlot {
  public:
    enum class Mode { LatencyPassthrough, MainSidechainMix };

    OfflineSidechainPlugin(Mode mode, int input_channels, int output_channels,
                           int latency_samples = 0)
        : mode_(mode), latency_samples_(latency_samples),
          info_(make_info(input_channels, output_channels)) {}

    const pulp::host::PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return true;
    }
    bool prepare(double, int) override {
        return true;
    }
    void release() override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::host::ParameterEventQueue&,
                 int num_samples) override {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
            float* dst = output.channel_ptr(channel);
            const float* main =
                channel < input.num_channels() ? input.channel_ptr(channel) : nullptr;
            const std::size_t sidechain_channel = channel + output.num_channels();
            const float* sidechain =
                mode_ == Mode::MainSidechainMix && sidechain_channel < input.num_channels()
                    ? input.channel_ptr(sidechain_channel)
                    : nullptr;
            for (int frame = 0; frame < num_samples; ++frame) {
                const auto index = static_cast<std::size_t>(frame);
                const float main_value = main ? main[index] : 0.0f;
                dst[index] = main_value + (sidechain ? sidechain[index] : 0.0f);
            }
        }
    }

    std::vector<pulp::host::HostParamInfo> parameters() const override {
        return {};
    }
    float get_parameter(std::uint32_t) const override {
        return 0.0f;
    }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override {
        return false;
    }
    std::vector<std::uint8_t> save_state() const override {
        return {};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override {
        return true;
    }
    int latency_samples() const override {
        return latency_samples_;
    }
    int tail_samples() const override {
        return 0;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}

  private:
    static pulp::host::PluginInfo make_info(int input_channels, int output_channels) {
        pulp::host::PluginInfo info{};
        info.name = "OfflineSidechainFixture";
        info.format = pulp::host::PluginFormat::BuiltIn;
        info.num_inputs = input_channels;
        info.num_outputs = output_channels;
        info.category = "Fx";
        return info;
    }

    Mode mode_;
    int latency_samples_;
    pulp::host::PluginInfo info_;
};

Fixture make_sidechain_pdc_fixture() {
    Fixture f;
    f.name = "offline sidechain PDC";
    f.in_channels = 2;
    f.out_channels = 2;
    f.block_size_dependent = false;
    f.build = [](SignalGraph& g) {
        const auto input = g.add_input_node(2, "Input");
        const auto delayed =
            g.add_plugin_node(std::make_unique<OfflineSidechainPlugin>(
                                  OfflineSidechainPlugin::Mode::LatencyPassthrough, 2, 2, 64),
                              2, 2, "Delayed");
        const auto sidechain = g.add_gain_node("Sidechain");
        const auto mixer =
            g.add_plugin_node(std::make_unique<OfflineSidechainPlugin>(
                                  OfflineSidechainPlugin::Mode::MainSidechainMix, 4, 2),
                              4, 2, "Mixer");
        const auto output = g.add_output_node(2, "Output");
        for (int channel = 0; channel < 2; ++channel) {
            REQUIRE(g.connect(input, channel, delayed, channel));
            REQUIRE(g.connect(delayed, channel, mixer, channel));
            REQUIRE(g.connect(input, channel, sidechain, channel));
            REQUIRE(g.connect_sidechain(sidechain, channel, mixer, channel + 2));
            REQUIRE(g.connect(mixer, channel, output, channel));
        }
        REQUIRE(g.set_node_gain(sidechain, 0.75f));
    };
    return f;
}

} // namespace

TEST_CASE("Offline SignalGraph render equals online render across partitionings",
          "[host][graph][offline][parity][i13]") {
    constexpr int kBlockOnline = 128;                // reference partition
    constexpr int kBlockCoarse = 64;                 // different, finer partition
    const int kBlockBig = static_cast<int>(kFrames); // one block for the whole render

    const Fixture f = make_gain_sum_fixture();
    const auto in_channels = make_input(static_cast<std::size_t>(f.in_channels), kFrames);
    const auto host_input = to_buffer(in_channels);

    // A. ONLINE reference: hand-written block loop on a freshly prepared graph.
    SignalGraph online_graph;
    f.build(online_graph);
    REQUIRE(online_graph.prepare(kSr, kBlockOnline));
    const auto online = run_online(online_graph, kBlockOnline,
                                   static_cast<std::size_t>(f.out_channels), in_channels);

    // B. OFFLINE, same block size as online -> bit-exact.
    const auto offline_same =
        run_offline(f.build, kBlockOnline, f.in_channels, f.out_channels, host_input);
    require_bit_exact(online, offline_same);

    // C. OFFLINE, coarser/different partition -> matches within tolerance.
    const auto offline_coarse =
        run_offline(f.build, kBlockCoarse, f.in_channels, f.out_channels, host_input);
    require_within(online, offline_coarse, 1e-6f);

    // D. OFFLINE, one big block -> matches within tolerance.
    const auto offline_big =
        run_offline(f.build, kBlockBig, f.in_channels, f.out_channels, host_input);
    require_within(online, offline_big, 1e-6f);

    // Non-vacuity guard: the gain/sum fixture must actually produce signal, so a
    // broad "every path silent" regression in SignalGraph::process() cannot pass
    // the equal-output comparisons trivially.
    float peak = 0.0f;
    for (const auto& ch : online)
        for (float s : ch)
            peak = std::max(peak, std::fabs(s));
    CHECK(peak > 0.1f);
}

TEST_CASE("Offline sidechain PDC render matches online routing across partitions",
          "[host][graph][offline][parity][sidechain][pdc]") {
    constexpr int kBlockOnline = 128;
    constexpr int kBlockCoarse = 64;
    const int kBlockBig = static_cast<int>(kFrames);

    const Fixture f = make_sidechain_pdc_fixture();
    std::vector<std::vector<float>> input(static_cast<std::size_t>(f.in_channels),
                                          std::vector<float>(kFrames, 0.0f));
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        input[1][frame] = 0.1f + static_cast<float>(frame) * 0.001f;
    }
    const auto host_input = to_buffer(input);

    SignalGraph online_graph;
    f.build(online_graph);
    REQUIRE(online_graph.prepare(kSr, kBlockOnline));
    const auto online =
        run_online(online_graph, kBlockOnline, static_cast<std::size_t>(f.out_channels), input);

    const auto offline_same =
        run_offline(f.build, kBlockOnline, f.in_channels, f.out_channels, host_input);
    const auto offline_coarse =
        run_offline(f.build, kBlockCoarse, f.in_channels, f.out_channels, host_input);
    const auto offline_big =
        run_offline(f.build, kBlockBig, f.in_channels, f.out_channels, host_input);
    require_bit_exact(online, offline_same);
    require_bit_exact(online, offline_coarse);
    require_bit_exact(online, offline_big);

    // The latent main path forces the sidechain path through a 64-sample PDC
    // ring. The input node feeds both buses, so the expected output is the
    // immediate main signal plus the delayed, gain-scaled sidechain. This is a
    // positive oracle for both the sidechain edge and the delay ring,
    // independent of the online comparison.
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const float delayed_sidechain = frame < 64 ? 0.0f : input[channel][frame - 64] * 0.75f;
            CHECK(online[channel][frame] == input[channel][frame] + delayed_sidechain);
        }
    }

    // Planted negative: a one-sample-wrong PDC oracle must fail, proving that
    // the positive result is sensitive to the negotiated delay rather than
    // merely checking that both paths produce finite audio.
    std::size_t wrong_delay_mismatches = 0;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const float wrong_delayed_sidechain =
                frame < 63 ? 0.0f : input[channel][frame - 63] * 0.75f;
            if (online[channel][frame] != input[channel][frame] + wrong_delayed_sidechain)
                ++wrong_delay_mismatches;
        }
    }
    CHECK(wrong_delay_mismatches > 0);
}

TEST_CASE("Offline sidechain PDC delays an impulse by the negotiated latency",
          "[host][graph][offline][parity][sidechain][pdc]") {
    constexpr int kPdcLatency = 64;
    constexpr std::size_t kImpulseFrame = 0;

    const Fixture f = make_sidechain_pdc_fixture();
    std::vector<std::vector<float>> input(static_cast<std::size_t>(f.in_channels),
                                          std::vector<float>(kFrames, 0.0f));
    input[0][kImpulseFrame] = 1.0f;

    const auto output =
        run_offline(f.build, kPdcLatency, f.in_channels, f.out_channels, to_buffer(input));

    // The main path is immediate, while the sidechain impulse appears only
    // after the first plugin's reported latency and is scaled by the sidechain
    // gain. This is an independent impulse-response oracle for the PDC ring.
    CHECK(output[0][kImpulseFrame] == 1.0f);
    CHECK(output[0][static_cast<std::size_t>(kPdcLatency - 1)] == 0.0f);
    CHECK(output[0][static_cast<std::size_t>(kPdcLatency)] == 0.75f);
}

TEST_CASE("Offline render zero-pads a source shorter than the frame count",
          "[host][graph][offline][parity][i13]") {
    // Exercises the host's source-tail padding: a source buffer shorter than
    // frame_count must render as if the missing tail were silence. The reference
    // is an online render of the SAME input explicitly zero-padded to frame_count,
    // so the two must be bit-exact — proving the host pads rather than reading past
    // the source or repeating stale samples.
    constexpr int kBlock = 128;
    constexpr std::size_t kShort = 500; // shorter than kFrames (1000)

    const Fixture f = make_gain_sum_fixture();
    const auto short_in = make_input(static_cast<std::size_t>(f.in_channels), kShort);

    // Zero-pad the short source to the full frame count for the online reference.
    std::vector<std::vector<float>> padded(short_in.size(), std::vector<float>(kFrames, 0.0f));
    for (std::size_t c = 0; c < short_in.size(); ++c) {
        std::copy(short_in[c].begin(), short_in[c].end(), padded[c].begin());
    }

    SignalGraph online_graph;
    f.build(online_graph);
    REQUIRE(online_graph.prepare(kSr, kBlock));
    const auto online =
        run_online(online_graph, kBlock, static_cast<std::size_t>(f.out_channels), padded);

    // Offline host fed the SHORT source but asked to render the full frame count.
    const auto short_buffer = to_buffer(short_in);
    const auto offline = run_offline(f.build, kBlock, f.in_channels, f.out_channels, short_buffer);

    require_bit_exact(online, offline);
}

TEST_CASE("Offline parity harness flags and excludes a block-size-dependent node",
          "[host][graph][offline][parity][i13][exempt]") {
    constexpr int kBlockOnline = 128;
    constexpr int kBlockCoarse = 64;

    const Fixture f = make_block_size_dependent_fixture();
    REQUIRE(f.block_size_dependent); // declared exemption metadata

    const auto in_channels = make_input(static_cast<std::size_t>(f.in_channels), kFrames);
    const auto host_input = to_buffer(in_channels);

    SignalGraph online_graph;
    f.build(online_graph);
    REQUIRE(online_graph.prepare(kSr, kBlockOnline));
    const auto online = run_online(online_graph, kBlockOnline,
                                   static_cast<std::size_t>(f.out_channels), in_channels);
    const auto offline_coarse =
        run_offline(f.build, kBlockCoarse, f.in_channels, f.out_channels, host_input);

    // The harness does NOT require equivalence for a declared-exempt fixture; it
    // flags it instead. Render both ways to demonstrate the exemption is real:
    // the two partitionings genuinely diverge, and that divergence is reported,
    // not asserted as a failure.
    INFO("fixture '" << f.name << "' is block-size dependent; excluded from equivalence");
    const bool diverged = max_abs_diff_exceeds(online, offline_coarse, 1e-6f);
    WARN("declared-exempt fixture '" << f.name << "' diverges across partitionings as expected: "
                                     << std::boolalpha << diverged);
    REQUIRE(diverged); // proves this fixture is genuinely block-size dependent
}

// GpuNeuralRealtimeNode continuity at the plug-in boundary, through the real
// CLAP adapter, for more than one neural family.
//
// Fake provider, in-process; not DAW and not render-loss evidence.
//
// Two processors render the same stimulus through the same host calls:
//   * GpuNeuralEffect stages host callbacks into fixed quanta and runs them
//     through GpuAudioTransport -> GpuNeuralRealtimeNode, whose CPU shadow is
//     built in. The WaveNet family also has a fake provider computing the
//     same function as its shadow; the LSTM and SSM families are CPU-only.
//   * CpuNeuralEffect runs the same family kernel directly and delays it by
//     the same reported latency. It shares no transport, node, or staging code.
// The claim: whatever the provider does (healthy, failed, lost, starved, or
// absent), and across reactivation and ragged host buffers, the plug-in
// emits exactly the CPU reference.

#include "gpu_neural_test_kernels.hpp"

#include "detail/gpu_neural_realtime_test_access.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/clap_entry.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::gpu_audio;
using namespace pulp::gpu_audio::neural_test;
namespace analysis = pulp::test::audio;

namespace {

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kLeadBlocks = 2;

std::uint32_t quantum_for(int max_buffer_size) {
    std::uint32_t q = 16;
    while (q < static_cast<std::uint32_t>(std::max(max_buffer_size, 1)))
        q <<= 1;
    return q;
}

/// ProcessorFactory is a plain function pointer, so the factories read the
/// family under test from here.
struct FixtureOptions {
    NeuralFamily family = NeuralFamily::WaveNet;
    bool gpu_backend = true;
};
FixtureOptions g_options;

struct OptionsScope {
    explicit OptionsScope(FixtureOptions options) {
        g_options = options;
    }
    ~OptionsScope() {
        g_options = {};
    }
};

format::PluginDescriptor continuity_descriptor(const char* name) {
    format::PluginDescriptor d;
    d.name = name;
    d.manufacturer = "PulpTest";
    d.bundle_id = "com.pulp.test.gpu-neural-plugin-boundary-continuity";
    d.version = "1.0.0";
    d.category = format::PluginCategory::Effect;
    d.input_buses = {{"In", 2}};
    d.output_buses = {{"Out", 2}};
    return d;
}

/// Effect rendering through the generic neural node. Host callbacks of any
/// size are staged into one fixed quantum (one quantum of extra latency).
class GpuNeuralEffect final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return continuity_descriptor("GpuNeuralContinuity");
    }
    void define_parameters(state::StateStore&) override {}
    bool has_editor() const override {
        return false;
    }

    void prepare(const format::PrepareContext& ctx) override {
        release_engine();
        plan_ = {};
        quantum_ = quantum_for(ctx.max_buffer_size);
        fixture_ = std::make_unique<FamilyFixture>(g_options.family, kChannels, quantum_);
        fixture_->sample_rate = static_cast<std::uint32_t>(ctx.sample_rate);
        auto request = fixture_->request();
        if (g_options.gpu_backend)
            request.backend = make_fake_wavenet_backend(plan_, shape_, kChannels, quantum_, 1,
                                                        false, fixture_->sample_rate);
        auto build = detail::make_neural_model(std::move(request));
        engine_ready_ = static_cast<bool>(build);
        if (engine_ready_) {
            node_ = std::make_unique<GpuNeuralRealtimeNode>(
                GpuNeuralRealtimeNode::Config{.family = g_options.family,
                                              .model = build.model,
                                              .channels = kChannels,
                                              .lead_blocks = kLeadBlocks,
                                              .capacity = kLeadBlocks + 3});
            engine_ready_ = node_->prepare() && transport_.prepare(node_.get(), {});
        }
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            staged_in_[ch].assign(quantum_, 0.0f);
            wet_[ch].assign(quantum_, 0.0f);
            out_fifo_[ch].assign(2u * quantum_, 0.0f);
        }
        fill_ = 0;
        fifo_read_ = 0;
        fifo_write_ = quantum_;
    }

    void release() override {
        release_engine();
    }

    int latency_samples() const override {
        return static_cast<int>(transport_.latency_samples() + quantum_);
    }

    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        if (!engine_ready_) {
            out.clear();
            return;
        }
        const auto fifo_size = 2u * quantum_;
        for (std::size_t i = 0; i < out.num_samples(); ++i) {
            for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
                staged_in_[ch][fill_] = ch < in.num_channels() ? in.channel(ch)[i] : 0.0f;
                out.channel(ch)[i] = out_fifo_[ch][fifo_read_];
            }
            fifo_read_ = (fifo_read_ + 1) % fifo_size;
            if (++fill_ == quantum_) {
                run_quantum();
                fill_ = 0;
            }
        }
    }

    GpuAudioTransport& transport() noexcept {
        return transport_;
    }
    GpuNeuralRealtimeNode* node() noexcept {
        return node_.get();
    }
    ChannelPlan& plan() noexcept {
        return plan_;
    }
    bool engine_ready() const noexcept {
        return engine_ready_;
    }

  private:
    void run_quantum() noexcept {
        std::array<const float*, kChannels> in_ptrs{};
        std::array<float*, kChannels> wet_ptrs{};
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in_ptrs[ch] = staged_in_[ch].data();
            wet_ptrs[ch] = wet_[ch].data();
        }
        audio::BufferView<const float> in(in_ptrs.data(), kChannels, quantum_);
        audio::BufferView<float> wet(wet_ptrs.data(), kChannels, quantum_);
        transport_.process(in, wet, quantum_);
        const auto fifo_size = 2u * quantum_;
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            for (std::uint32_t i = 0; i < quantum_; ++i)
                out_fifo_[ch][(fifo_write_ + i) % fifo_size] = wet_[ch][i];
        fifo_write_ = (fifo_write_ + quantum_) % fifo_size;
    }

    void release_engine() noexcept {
        transport_.release();
        if (node_)
            (void)node_->release();
        node_.reset();
        fixture_.reset();
        engine_ready_ = false;
    }

    // Declaration order keeps the provider plan and kernel states alive until
    // the node (and the model it owns) has released.
    ChannelPlan plan_;
    WaveNetShape shape_;
    std::unique_ptr<FamilyFixture> fixture_;
    std::unique_ptr<GpuNeuralRealtimeNode> node_;
    GpuAudioTransport transport_;
    bool engine_ready_ = false;
    std::uint32_t quantum_ = 0;
    std::array<std::vector<float>, kChannels> staged_in_, wet_, out_fifo_;
    std::uint32_t fill_ = 0, fifo_read_ = 0, fifo_write_ = 0;
};

/// CPU reference: the family kernel run sample by sample, delayed by the same
/// reported latency. Same seeds as FamilyFixture, independent state.
class CpuNeuralEffect final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return continuity_descriptor("CpuNeuralContinuity");
    }
    void define_parameters(state::StateStore&) override {}
    bool has_editor() const override {
        return false;
    }
    void prepare(const format::PrepareContext& ctx) override {
        const auto quantum = quantum_for(ctx.max_buffer_size);
        latency_ = (kLeadBlocks + 1) * quantum;
        fixture_ = std::make_unique<FamilyFixture>(g_options.family, kChannels, quantum);
        fixture_->build_kernels();
        for (auto& line : delay_)
            line.assign(latency_, 0.0f);
        cursor_ = 0;
    }
    int latency_samples() const override {
        return static_cast<int>(latency_);
    }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        for (std::size_t i = 0; i < out.num_samples(); ++i) {
            for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
                const float x = ch < in.num_channels() ? in.channel(ch)[i] : 0.0f;
                float y = 0.0f;
                const auto& kernel = fixture_->kernels[ch];
                kernel.process(kernel.state, &x, &y, 1);
                out.channel(ch)[i] = delay_[ch][cursor_];
                delay_[ch][cursor_] = y;
            }
            cursor_ = (cursor_ + 1) % latency_;
        }
    }

  private:
    std::unique_ptr<FamilyFixture> fixture_;
    std::uint32_t latency_ = 0;
    std::array<std::vector<float>, kChannels> delay_;
    std::uint32_t cursor_ = 0;
};

std::unique_ptr<format::Processor> make_gpu_effect() {
    return std::make_unique<GpuNeuralEffect>();
}
std::unique_ptr<format::Processor> make_cpu_effect() {
    return std::make_unique<CpuNeuralEffect>();
}

enum class Fault { StarveWorker, FeedWorker, FailCompletions, ProviderLost };

/// Drives one plug-in through the real CLAP adapter the way a host would.
class ClapSession {
  public:
    explicit ClapSession(format::ProcessorFactory factory) {
        plugin_->factory = factory;
        plugin_->plugin.plugin_data = plugin_.get();
        REQUIRE(format::clap_adapter::clap_init(&plugin_->plugin));
    }
    ~ClapSession() {
        if (active_)
            format::clap_adapter::clap_deactivate(&plugin_->plugin);
    }
    ClapSession(const ClapSession&) = delete;
    ClapSession& operator=(const ClapSession&) = delete;

    void activate(double sample_rate, std::uint32_t max_frames) {
        if (active_)
            format::clap_adapter::clap_deactivate(&plugin_->plugin);
        REQUIRE(format::clap_adapter::clap_activate(&plugin_->plugin, sample_rate, 1, max_frames));
        active_ = true;
        max_frames_ = max_frames;
        starved_ = false;
        if (auto* fx = gpu())
            REQUIRE(fx->engine_ready());
    }

    void render(std::uint64_t frames, std::initializer_list<std::uint32_t> pattern) {
        std::vector<std::uint32_t> sizes(pattern);
        std::size_t next = 0;
        while (frames > 0) {
            const auto n = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(std::min(sizes[next], max_frames_), frames));
            next = (next + 1) % sizes.size();
            callback(n);
            frames -= n;
        }
    }

    void callback(std::uint32_t n) {
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in_[ch].resize(n);
            out_[ch].assign(n, 0.0f);
            for (std::uint32_t i = 0; i < n; ++i)
                in_[ch][i] = stimulus(ch, frame_ + i);
        }
        float* in_ptrs[2] = {in_[0].data(), in_[1].data()};
        float* out_ptrs[2] = {out_[0].data(), out_[1].data()};
        clap_audio_buffer_t in_bus{};
        in_bus.channel_count = kChannels;
        in_bus.data32 = in_ptrs;
        clap_audio_buffer_t out_bus{};
        out_bus.channel_count = kChannels;
        out_bus.data32 = out_ptrs;
        clap_input_events_t in_events{};
        in_events.size = [](const clap_input_events_t*) -> std::uint32_t { return 0; };
        in_events.get = [](const clap_input_events_t*,
                           std::uint32_t) -> const clap_event_header_t* { return nullptr; };
        clap_output_events_t out_events{};
        out_events.try_push = [](const clap_output_events_t*, const clap_event_header_t*) {
            return true;
        };
        clap_process_t proc{};
        proc.frames_count = n;
        proc.audio_inputs = &in_bus;
        proc.audio_inputs_count = 1;
        proc.audio_outputs = &out_bus;
        proc.audio_outputs_count = 1;
        proc.in_events = &in_events;
        proc.out_events = &out_events;

        // No Catch assertion inside the probe: a reporter may allocate.
        std::size_t allocations = 0;
        clap_process_status status = CLAP_PROCESS_ERROR;
        {
            pulp::test::RtAllocationProbe probe;
            status = format::clap_adapter::clap_process(&plugin_->plugin, &proc);
            allocations = probe.allocation_count();
        }
        REQUIRE(status == CLAP_PROCESS_CONTINUE);
        callback_allocations_ += allocations;
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            output_[ch].insert(output_[ch].end(), out_[ch].begin(), out_[ch].end());
        frame_ += n;
        if (auto* fx = gpu(); fx != nullptr && !starved_)
            for (int i = 0; i < 4; ++i)
                fx->transport().pump();
    }

    void inject(Fault fault) {
        auto* fx = gpu();
        if (fx == nullptr)
            return;
        switch (fault) {
        case Fault::StarveWorker:
            starved_ = true;
            break;
        case Fault::FeedWorker:
            starved_ = false;
            break;
        case Fault::FailCompletions:
            fx->plan().fail_completions = true;
            break;
        case Fault::ProviderLost:
            detail::NeuralRealtimeTestAccess::request_recovery(
                *fx->node(), detail::SharedIoRecoveryReason::ProviderLost);
            break;
        }
    }

    GpuNeuralEffect* gpu() {
        return dynamic_cast<GpuNeuralEffect*>(plugin_->processor.get());
    }
    std::uint32_t latency() const {
        return static_cast<std::uint32_t>(plugin_->processor->latency_samples());
    }
    audio::Buffer<float> output() const {
        audio::Buffer<float> buffer(kChannels, output_[0].size());
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            std::copy(output_[ch].begin(), output_[ch].end(), buffer.channel(ch).begin());
        return buffer;
    }
    std::size_t callback_allocations() const noexcept {
        return callback_allocations_;
    }

  private:
    struct ClapDestroy {
        void operator()(format::clap_adapter::PulpClapPlugin* p) const {
            format::clap_adapter::clap_destroy(&p->plugin);
        }
    };
    std::unique_ptr<format::clap_adapter::PulpClapPlugin, ClapDestroy> plugin_{
        new format::clap_adapter::PulpClapPlugin()};
    bool active_ = false;
    bool starved_ = false;
    std::uint32_t max_frames_ = 0;
    std::uint64_t frame_ = 0;
    std::array<std::vector<float>, kChannels> in_, out_, output_;
    std::size_t callback_allocations_ = 0;
};

void require_null(const ClapSession& gpu, const ClapSession& cpu) {
    const auto actual = gpu.output();
    const auto expected = cpu.output();
    REQUIRE(actual.num_samples() == expected.num_samples());
    const auto metrics = analysis::analyze(actual, 48000.0);
    CHECK(analysis::assert_no_nan_inf(metrics).passed);
    CHECK(analysis::assert_not_silent(metrics).passed);
    const auto null = analysis::assert_null_near(actual, expected);
    INFO(null.message);
    CHECK(null.passed);
}

GpuAudioTransport::DeliverySnapshot delivery(ClapSession& session) {
    return session.gpu()->transport().delivery_snapshot();
}

struct FamilyCase {
    const char* name;
    NeuralFamily family;
    bool gpu_backend;
};

} // namespace

TEST_CASE("neural plug-in nulls against the CPU reference for every hosted family",
          "[gpu_audio][neural][plugin-boundary-continuity][clap]") {
    const auto c = GENERATE(FamilyCase{"wavenet-gpu", NeuralFamily::WaveNet, true},
                            FamilyCase{"lstm", NeuralFamily::Lstm, false},
                            FamilyCase{"ssm", NeuralFamily::Ssm, false});
    CAPTURE(c.name);
    OptionsScope scope({.family = c.family, .gpu_backend = c.gpu_backend});
    ClapSession gpu(&make_gpu_effect), cpu(&make_cpu_effect);
    for (auto* s : {&gpu, &cpu}) {
        s->activate(48000.0, 64);
        s->render(48 * 64, {64, 37, 1, 64, 13, 50});
        // Host reactivation with a new block size rebuilds the node.
        s->activate(44100.0, 128);
        s->render(24 * 128, {128, 7, 100, 128});
    }
    CHECK(gpu.latency() == cpu.latency());
    CHECK(gpu.latency() == (kLeadBlocks + 1) * 128);
    const auto d = delivery(gpu);
    if (c.gpu_backend) {
        CHECK(d.gpu_blocks > 0);
        CHECK(d.cpu_fallback_blocks == 0);
        CHECK_FALSE(gpu.gpu()->plan().sequence_gap_seen);
    } else {
        CHECK(d.gpu_blocks == 0);
        CHECK(d.cpu_fallback_blocks > 0);
        CHECK(gpu.gpu()->node()->epoch() == 0);
    }
    CHECK(d.silence_blocks == 0);
    CHECK(gpu.callback_allocations() == 0);
    require_null(gpu, cpu);
}

TEST_CASE("neural plug-in output stays continuous across simulated provider faults",
          "[gpu_audio][neural][plugin-boundary-continuity][clap]") {
    const auto fault = GENERATE(Fault::FailCompletions, Fault::ProviderLost, Fault::StarveWorker);
    CAPTURE(static_cast<int>(fault));
    OptionsScope scope({.family = NeuralFamily::WaveNet, .gpu_backend = true});
    ClapSession gpu(&make_gpu_effect), cpu(&make_cpu_effect);
    for (auto* s : {&gpu, &cpu}) {
        s->activate(48000.0, 64);
        s->render(16 * 64, {64, 21, 43});
    }
    REQUIRE(delivery(gpu).gpu_blocks > 0);
    REQUIRE(delivery(gpu).cpu_fallback_blocks == 0);
    for (auto* s : {&gpu, &cpu}) {
        s->inject(fault);
        s->render(12 * 64, {64, 21, 43, 1});
        if (fault == Fault::StarveWorker)
            s->inject(Fault::FeedWorker);
        s->render(20 * 64, {64, 30, 34});
    }
    const auto after = delivery(gpu);
    CHECK(after.cpu_fallback_blocks > 0);
    CHECK(after.silence_blocks == 0);
    CHECK(after.invalid_blocks == 0);
    CHECK_FALSE(gpu.gpu()->plan().sequence_gap_seen);
    CHECK(gpu.callback_allocations() == 0);
    require_null(gpu, cpu);
}

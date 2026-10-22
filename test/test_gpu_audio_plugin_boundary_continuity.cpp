// GPU-audio plug-in-boundary continuity under simulated provider faults,
// through the real CLAP adapter and HeadlessHost.
//
// Fake provider, in-process; not DAW and not render-loss evidence.
//
// The transport and execution-controller suites prove the fallback contract one
// callback at a time. This file checks it at the plug-in boundary, under
// host-style call sequences: activate/deactivate with a new sample rate or
// block size, ragged host callbacks, adapter-owned bypass, and state save/load
// while the simulated GPU provider has failed. Bypass and state spans null
// partly by construction (both plug-ins share the adapter's dry delay and
// parameter state); what they add is that neither disturbs the fallback
// timeline on either side of the transition.
//
// Two processors render the same stimulus through the same host call sequence:
//   * GpuFirEffect stages host callbacks into fixed quanta and runs them through
//     GpuAudioTransport -> GpuWaveNetRealtimeNode, whose provider seam is a
//     deterministic FIR "GPU". A latency-aligned CPU shadow is the declared
//     CpuFallback.
//   * CpuFirEffect computes the same FIR directly and delays it by the same
//     reported latency. It shares no transport, node, bridge, or staging code.
// Both use one FIR definition, so the null measures delivery (alignment,
// continuity, fallback selection), not the DSP itself. The claim is that
// whatever the GPU path does (healthy, starved, failed, lost), the plug-in emits
// exactly the CPU reference.
//
// Scope. Faults enter at Pulp-owned seams: a provider completion reporting
// failure, a refused submission, a starved worker, and a ProviderLost recovery
// request on the bridge (the reason the Dawn convolution session raises on
// device loss). None of this observes a real graphics device, a render owner,
// or a DAW. Real render-owner loss, authenticated device callbacks, resource
// replay, teardown callback suppression, and end-to-end DAW continuity remain
// gated on the Vellum render-owner seam.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "detail/shared_io_stamped_bridge.hpp"
#include "detail/wavenet_realtime_channel.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/clap_entry.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#define PULP_CONTINUITY_NOINLINE __declspec(noinline)
#else
#define PULP_CONTINUITY_NOINLINE __attribute__((noinline))
#endif

using namespace pulp;
using namespace pulp::gpu_audio;
namespace analysis = pulp::test::audio;

namespace {

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kLeadBlocks = 2;
constexpr state::ParamID kBypassParam = 1;
constexpr state::ParamID kGainParam = 2;

// ---------------------------------------------------------------------------
// The DSP both processors and the fake provider compute.
// ---------------------------------------------------------------------------

constexpr std::array<float, 5> kTaps{0.5f, -0.3f, 0.2f, -0.1f, 0.05f};

/// Stateful FIR whose history spans block boundaries, so a dropped, repeated,
/// or misaligned block leaves a trace in every later sample. One out-of-line
/// body keeps floating-point contraction identical at every call site.
struct Fir {
    std::array<float, kTaps.size() - 1> z{};
    PULP_CONTINUITY_NOINLINE float step(float x) noexcept {
        float y = kTaps[0] * x;
        for (std::size_t i = 0; i < z.size(); ++i)
            y += kTaps[i + 1] * z[i];
        for (std::size_t i = z.size() - 1; i > 0; --i)
            z[i] = z[i - 1];
        z[0] = x;
        return y;
    }
};

std::uint32_t quantum_for(int max_buffer_size) {
    std::uint32_t q = 16;
    while (q < static_cast<std::uint32_t>(std::max(max_buffer_size, 1)))
        q <<= 1;
    return q;
}

// ---------------------------------------------------------------------------
// Fault seams.
// ---------------------------------------------------------------------------

/// Test-owned provider health. Reset on every activation, which models a host
/// reactivation that rebuilds the provider session on a recovered device.
struct FaultPlan {
    bool fail_completions = false; // receive() reports ProviderFailed
    bool reject_submissions = false;
    bool sequence_gap_seen = false;
    std::uint64_t submissions = 0;
};

/// Deterministic provider: computes the FIR at submission in stream order and
/// returns it on the next service. It also records whether the node ever
/// handed it a non-contiguous sequence, which would mean a skipped input.
class FirChannel final : public detail::WaveNetRealtimeChannel {
  public:
    FirChannel(FaultPlan& plan, std::uint32_t quantum) : plan_(plan) {
        for (auto& slot : slots_)
            slot.data.assign(quantum, 0.0f);
    }
    bool submit(std::span<const float> input, std::uint64_t sequence) noexcept override {
        if (plan_.reject_submissions)
            return false;
        auto slot =
            std::find_if(slots_.begin(), slots_.end(), [](const Slot& s) { return !s.active; });
        if (slot == slots_.end())
            return false;
        if (has_expected_ && sequence != expected_)
            plan_.sequence_gap_seen = true;
        has_expected_ = true;
        expected_ = sequence + 1;
        for (std::size_t i = 0; i < input.size(); ++i)
            slot->data[i] = fir_.step(input[i]);
        slot->sequence = sequence;
        slot->active = true;
        ++plan_.submissions;
        return true;
    }
    void service(std::uint64_t) noexcept override {}
    void service_until(std::uint64_t, std::uint64_t) noexcept override {}
    std::optional<GpuWaveNetBlockResult> receive(std::span<float> output) noexcept override {
        auto slot =
            std::find_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.active; });
        if (slot == slots_.end())
            return {};
        slot->active = false;
        if (plan_.fail_completions)
            return GpuWaveNetBlockResult{slot->sequence, GpuWaveNetBlockStatus::ProviderFailed,
                                         false};
        std::copy_n(slot->data.begin(), output.size(), output.begin());
        return GpuWaveNetBlockResult{slot->sequence, GpuWaveNetBlockStatus::GpuDelivered, false};
    }
    bool release() noexcept override {
        return true;
    }

  private:
    struct Slot {
        std::vector<float> data;
        std::uint64_t sequence = 0;
        bool active = false;
    };
    FaultPlan& plan_;
    Fir fir_;
    std::array<Slot, 4> slots_{};
    std::uint64_t expected_ = 0;
    bool has_expected_ = false;
};

/// Realtime node with a continuously primed CPU shadow. The shadow runs the
/// same FIR on every callback and emits it `delay_blocks` callbacks later. The
/// correct delay is the node's lead; any other value is a planted misalignment.
/// The alignment is this fixture's choice: shipping nodes own their own shadow,
/// so this proves the transport's use of a correct one, not any node's shadow.
class ShadowFirNode final : public GpuWaveNetRealtimeNode {
  public:
    ShadowFirNode(const Config& config, std::uint32_t quantum, std::uint32_t delay_blocks)
        : GpuWaveNetRealtimeNode(config), quantum_(quantum), delay_blocks_(delay_blocks),
          history_(static_cast<std::size_t>(std::max(delay_blocks, 1u)) * kChannels * quantum,
                   0.0f),
          due_(static_cast<std::size_t>(kChannels) * quantum, 0.0f) {}
    bool cpu_fallback_prepared() const noexcept override {
        return true;
    }
    void prime_fallback(const audio::BufferView<const float>& in,
                        std::uint32_t n) noexcept override {
        const auto slot = delay_blocks_ == 0 ? 0u : cursor_ % delay_blocks_;
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            const float* x = in.channel_ptr(ch);
            float* due = due_.data() + static_cast<std::size_t>(ch) * quantum_;
            float* held =
                history_.data() + (static_cast<std::size_t>(slot) * kChannels + ch) * quantum_;
            for (std::uint32_t i = 0; i < n; ++i) {
                const float y = fir_[ch].step(x[i]);
                if (delay_blocks_ == 0) {
                    due[i] = y;
                } else {
                    due[i] = held[i];
                    held[i] = y;
                }
            }
        }
        ++cursor_;
    }
    void process_cpu_fallback(const audio::BufferView<const float>&, audio::BufferView<float>& out,
                              std::uint32_t n) noexcept override {
        ++fallback_blocks;
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            std::copy_n(due_.data() + static_cast<std::size_t>(ch) * quantum_, n,
                        out.channel_ptr(ch));
    }
    std::uint64_t fallback_blocks = 0;

  private:
    std::uint32_t quantum_;
    std::uint32_t delay_blocks_;
    std::array<Fir, kChannels> fir_{};
    std::vector<float> history_;
    std::vector<float> due_;
    std::uint64_t cursor_ = 0;
};

/// Per-test fixture knobs. ProcessorFactory is a plain function pointer, so the
/// factory reads these when the adapter or HeadlessHost constructs the plug-in.
struct FixtureOptions {
    MissPolicy miss_policy = MissPolicy::CpuFallback;
    std::uint32_t shadow_delay_blocks = kLeadBlocks;
};
FixtureOptions g_options;

// ---------------------------------------------------------------------------
// The two plug-ins.
// ---------------------------------------------------------------------------

format::PluginDescriptor continuity_descriptor(const char* name) {
    format::PluginDescriptor d;
    d.name = name;
    d.manufacturer = "PulpTest";
    d.bundle_id = "com.pulp.test.gpu-audio-plugin-boundary-continuity";
    d.version = "1.0.0";
    d.category = format::PluginCategory::Effect;
    d.input_buses = {{"In", 2}};
    d.output_buses = {{"Out", 2}};
    return d;
}

void define_continuity_parameters(state::StateStore& store) {
    store.add_parameter({.id = kBypassParam,
                         .name = "Bypass",
                         .range = {0.0f, 1.0f, 0.0f, 1.0f},
                         .designation = state::ParamDesignation::Bypass});
    store.add_parameter({.id = kGainParam, .name = "Gain", .range = {0.0f, 2.0f, 1.0f, 0.0f}});
}

/// Effect that renders through a GPU-audio node. Host callbacks of any size up
/// to the prepared maximum are staged into one fixed quantum, which costs one
/// quantum of latency on top of the transport's lead.
class GpuFirEffect final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return continuity_descriptor("GpuAudioContinuity");
    }
    void define_parameters(state::StateStore& store) override {
        define_continuity_parameters(store);
    }
    bool has_editor() const override {
        return false;
    }

    void prepare(const format::PrepareContext& ctx) override {
        release_engine();
        plan_ = {};
        quantum_ = quantum_for(ctx.max_buffer_size);
        GpuWaveNetRealtimeNode::Config config{
            .session = {.descriptor = {.block_size = quantum_,
                                       .sample_rate = static_cast<std::uint32_t>(ctx.sample_rate),
                                       .stream_instances = 1,
                                       .layers = {&layer_, 1},
                                       .weight_count = weights_.size()},
                        .weights = weights_,
                        .slots = 2},
            .channels = kChannels,
            .lead_blocks = kLeadBlocks,
            .capacity = kLeadBlocks + 3,
            .max_inflight = 1,
            .miss_policy = g_options.miss_policy,
            .supports_cpu_fallback = g_options.miss_policy == MissPolicy::CpuFallback};
        node_ = std::make_unique<ShadowFirNode>(config, quantum_, g_options.shadow_delay_blocks);
        std::vector<std::unique_ptr<detail::WaveNetRealtimeChannel>> providers;
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            providers.push_back(std::make_unique<FirChannel>(plan_, quantum_));
        engine_ready_ = detail::WaveNetRealtimeTestAccess::prepare(*node_, std::move(providers)) &&
                        transport_.prepare(node_.get(), {});

        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            staged_in_[ch].assign(quantum_, 0.0f);
            wet_[ch].assign(quantum_, 0.0f);
            // Primed with one quantum of silence: each sample pops one value
            // before its quantum can complete and push a full quantum back.
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
        // Transport lead plus the one staging quantum.
        return static_cast<int>(transport_.latency_samples() + quantum_);
    }

    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        if (!engine_ready_) {
            out.clear();
            return;
        }
        const float gain = state().get_value(kGainParam);
        const auto fifo_size = 2u * quantum_;
        for (std::size_t i = 0; i < out.num_samples(); ++i) {
            for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
                const float x = ch < in.num_channels() ? in.channel(ch)[i] : 0.0f;
                staged_in_[ch][fill_] = x;
                out.channel(ch)[i] = out_fifo_[ch][fifo_read_] * gain;
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
    ShadowFirNode* node() noexcept {
        return node_.get();
    }
    FaultPlan& plan() noexcept {
        return plan_;
    }
    bool engine_ready() const noexcept {
        return engine_ready_;
    }
    std::uint32_t quantum() const noexcept {
        return quantum_;
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
        engine_ready_ = false;
    }

    GpuWaveNetLayerDescriptor layer_{.input_size = 1,
                                     .condition_size = 1,
                                     .channels = 1,
                                     .kernel = 2,
                                     .head_size = 1,
                                     .dilation = 1,
                                     .tanh_activation = true};
    std::array<float, 9> weights_{};
    FaultPlan plan_;
    std::unique_ptr<ShadowFirNode> node_;
    GpuAudioTransport transport_;
    bool engine_ready_ = false;
    std::uint32_t quantum_ = 0;
    std::array<std::vector<float>, kChannels> staged_in_;
    std::array<std::vector<float>, kChannels> wet_;
    std::array<std::vector<float>, kChannels> out_fifo_;
    std::uint32_t fill_ = 0;
    std::uint32_t fifo_read_ = 0;
    std::uint32_t fifo_write_ = 0;
};

/// The CPU reference: the same FIR delayed by the same reported latency.
class CpuFirEffect final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return continuity_descriptor("CpuAudioContinuity");
    }
    void define_parameters(state::StateStore& store) override {
        define_continuity_parameters(store);
    }
    bool has_editor() const override {
        return false;
    }
    void prepare(const format::PrepareContext& ctx) override {
        latency_ = (kLeadBlocks + 1) * quantum_for(ctx.max_buffer_size);
        for (auto& line : delay_)
            line.assign(latency_, 0.0f);
        fir_ = {};
        cursor_ = 0;
    }
    int latency_samples() const override {
        return static_cast<int>(latency_);
    }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        const float gain = state().get_value(kGainParam);
        for (std::size_t i = 0; i < out.num_samples(); ++i) {
            for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
                const float x = ch < in.num_channels() ? in.channel(ch)[i] : 0.0f;
                const float y = fir_[ch].step(x);
                out.channel(ch)[i] = delay_[ch][cursor_] * gain;
                delay_[ch][cursor_] = y;
            }
            cursor_ = (cursor_ + 1) % latency_;
        }
    }

  private:
    std::uint32_t latency_ = 0;
    std::array<std::vector<float>, kChannels> delay_;
    std::array<Fir, kChannels> fir_{};
    std::uint32_t cursor_ = 0;
};

std::unique_ptr<format::Processor> make_gpu_effect() {
    return std::make_unique<GpuFirEffect>();
}
std::unique_ptr<format::Processor> make_cpu_effect() {
    return std::make_unique<CpuFirEffect>();
}

// ---------------------------------------------------------------------------
// Host simulation.
// ---------------------------------------------------------------------------

/// Continuous broadband stimulus: seeded noise plus a tone, never silent, so a
/// dropout anywhere shows up as a null failure.
float stimulus(std::uint32_t ch, std::uint64_t frame) {
    std::uint32_t seed =
        0x9e3779b9u ^ static_cast<std::uint32_t>(frame * 2654435761u) ^ (ch * 0x85ebca6bu);
    seed ^= seed >> 15;
    seed *= 0x2c1b3c6du;
    seed ^= seed >> 12;
    const float noise = static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
    const float tone =
        static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * (220.0 + 110.0 * ch) *
                                    static_cast<double>(frame) / 48000.0));
    return 0.25f * noise + 0.5f * tone;
}

enum class Fault {
    StarveWorker,    // the worker misses its deadline (e.g. a contended GPU queue)
    FeedWorker,      // the worker is serviced again
    FailCompletions, // the provider reports failed completions
    RejectSubmissions,
    ProviderLost, // the bridge recovery request the Dawn session uses on loss
};

/// Drives one plug-in through the real CLAP adapter the way a host would.
/// The GPU worker is modelled deterministically: after each callback it is
/// pumped unless a test has starved it.
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

    /// Render `frames` of stimulus in callbacks cycling through `pattern`.
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

    /// One host callback. Queued parameter changes land at offset 0.
    void callback(std::uint32_t n) {
        in_[0].resize(n);
        in_[1].resize(n);
        out_[0].assign(n, 0.0f);
        out_[1].assign(n, 0.0f);
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            for (std::uint32_t i = 0; i < n; ++i)
                in_[ch][i] = stimulus(ch, frame_ + i);
        float* in_ptrs[2] = {in_[0].data(), in_[1].data()};
        float* out_ptrs[2] = {out_[0].data(), out_[1].data()};

        clap_audio_buffer_t in_bus{};
        in_bus.channel_count = kChannels;
        in_bus.data32 = in_ptrs;
        clap_audio_buffer_t out_bus{};
        out_bus.channel_count = kChannels;
        out_bus.data32 = out_ptrs;

        events_.clear();
        for (const auto& [id, value] : pending_params_) {
            clap_event_param_value_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.param_id = id;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = value;
            events_.push_back(ev);
        }
        pending_params_.clear();
        clap_input_events_t in_events{};
        in_events.ctx = &events_;
        in_events.size = [](const clap_input_events_t* l) -> std::uint32_t {
            return static_cast<std::uint32_t>(
                static_cast<const std::vector<clap_event_param_value_t>*>(l->ctx)->size());
        };
        in_events.get = [](const clap_input_events_t* l,
                           std::uint32_t i) -> const clap_event_header_t* {
            return &(*static_cast<const std::vector<clap_event_param_value_t>*>(l->ctx))[i].header;
        };
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

        // No Catch assertion inside the probe: a reporter that stringifies
        // passing assertions would allocate and be counted.
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

        // The pump (and the fake provider's FIR) runs outside the adapter's
        // denormal-flush scope. The stimulus never produces subnormals, so
        // both sides compute identical bits regardless of FTZ/DAZ.
        if (auto* fx = gpu(); fx != nullptr && !starved_)
            for (int i = 0; i < 4; ++i)
                fx->transport().pump();
    }

    void set_param(state::ParamID id, double value) {
        pending_params_.push_back({id, value});
    }

    /// Applies a fault to the GPU plug-in; a no-op for the CPU reference.
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
        case Fault::RejectSubmissions:
            fx->plan().reject_submissions = true;
            break;
        case Fault::ProviderLost:
            detail::WaveNetRealtimeTestAccess::request_recovery(
                *fx->node(), detail::SharedIoRecoveryReason::ProviderLost);
            break;
        }
    }

    std::vector<std::uint8_t> save_state() {
        auto* ext = static_cast<const clap_plugin_state_t*>(
            format::clap_generic::get_extension(&plugin_->plugin, CLAP_EXT_STATE));
        REQUIRE(ext != nullptr);
        std::vector<std::uint8_t> bytes;
        clap_ostream_t stream{};
        stream.ctx = &bytes;
        stream.write = [](const clap_ostream_t* s, const void* data,
                          std::uint64_t size) -> std::int64_t {
            auto* sink = static_cast<std::vector<std::uint8_t>*>(s->ctx);
            const auto* p = static_cast<const std::uint8_t*>(data);
            sink->insert(sink->end(), p, p + size);
            return static_cast<std::int64_t>(size);
        };
        REQUIRE(ext->save(&plugin_->plugin, &stream));
        return bytes;
    }

    void load_state(const std::vector<std::uint8_t>& bytes) {
        auto* ext = static_cast<const clap_plugin_state_t*>(
            format::clap_generic::get_extension(&plugin_->plugin, CLAP_EXT_STATE));
        REQUIRE(ext != nullptr);
        struct Source {
            const std::vector<std::uint8_t>* bytes;
            std::size_t offset = 0;
        } source{&bytes};
        clap_istream_t stream{};
        stream.ctx = &source;
        stream.read = [](const clap_istream_t* s, void* data, std::uint64_t size) -> std::int64_t {
            auto* src = static_cast<Source*>(s->ctx);
            const auto left = src->bytes->size() - src->offset;
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(size, left));
            std::memcpy(data, src->bytes->data() + src->offset, n);
            src->offset += n;
            return static_cast<std::int64_t>(n);
        };
        REQUIRE(ext->load(&plugin_->plugin, &stream));
    }

    GpuFirEffect* gpu() {
        return dynamic_cast<GpuFirEffect*>(plugin_->processor.get());
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
    // clap_destroy() deletes the adapter state, as a host's destroy would; the
    // deleter also runs when a failed clap_init() unwinds the constructor.
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
    std::vector<clap_event_param_value_t> events_;
    std::vector<std::pair<state::ParamID, double>> pending_params_;
    std::size_t callback_allocations_ = 0;
};

/// The rendered plug-in output must be a continuous, finite, non-silent stream
/// that nulls against the CPU reference rendered through the same host calls.
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

struct OptionsScope {
    explicit OptionsScope(FixtureOptions options) {
        g_options = options;
    }
    ~OptionsScope() {
        g_options = {};
    }
};

} // namespace

TEST_CASE("GPU-audio plug-in nulls against the CPU reference while the GPU path is healthy",
          "[gpu_audio][plugin-boundary-continuity][clap]") {
    ClapSession gpu(&make_gpu_effect), cpu(&make_cpu_effect);
    for (auto* s : {&gpu, &cpu}) {
        s->activate(48000.0, 64);
        s->render(48 * 64, {64, 37, 1, 64, 13, 50});
    }
    CHECK(gpu.latency() == cpu.latency());
    CHECK(gpu.latency() == (kLeadBlocks + 1) * 64);
    const auto d = delivery(gpu);
    CHECK(d.gpu_blocks > 0);
    CHECK(d.priming_blocks == kLeadBlocks);
    CHECK(d.cpu_fallback_blocks == 0);
    CHECK(d.silence_blocks == 0);
    CHECK_FALSE(gpu.gpu()->node()->fenced());
    CHECK_FALSE(gpu.gpu()->plan().sequence_gap_seen);
    CHECK(gpu.callback_allocations() == 0);
    require_null(gpu, cpu);
}

TEST_CASE("GPU-audio plug-in output stays continuous across simulated provider faults",
          "[gpu_audio][plugin-boundary-continuity][clap]") {
    const auto fault =
        GENERATE(Fault::FailCompletions, Fault::RejectSubmissions, Fault::ProviderLost);
    CAPTURE(static_cast<int>(fault));
    ClapSession gpu(&make_gpu_effect), cpu(&make_cpu_effect);
    for (auto* s : {&gpu, &cpu}) {
        s->activate(48000.0, 64);
        s->render(16 * 64, {64, 21, 43});
    }
    const auto before = delivery(gpu);
    REQUIRE(before.gpu_blocks > 0);
    REQUIRE(before.cpu_fallback_blocks == 0);

    for (auto* s : {&gpu, &cpu}) {
        s->inject(fault);
        s->render(32 * 64, {64, 21, 43, 1});
    }
    const auto after = delivery(gpu);
    CHECK(gpu.gpu()->node()->fenced());
    CHECK(after.cpu_fallback_blocks > 0);
    CHECK(after.silence_blocks == 0);
    CHECK(after.invalid_blocks == 0);
    // Fenced: no later block is delivered from the provider.
    const auto fenced_gpu_blocks = after.gpu_blocks;
    for (auto* s : {&gpu, &cpu})
        s->render(8 * 64, {64});
    CHECK(delivery(gpu).gpu_blocks == fenced_gpu_blocks);
    CHECK(gpu.gpu()->node()->recovery_reason() == (fault == Fault::ProviderLost
                                                       ? GpuAudioRecoveryReason::ProviderLost
                                                       : GpuAudioRecoveryReason::ProviderFailure));
    CHECK_FALSE(gpu.gpu()->plan().sequence_gap_seen);
    CHECK(gpu.callback_allocations() == 0);
    require_null(gpu, cpu);
}

TEST_CASE("GPU-audio plug-in covers a starved worker with the CPU fallback and recovers",
          "[gpu_audio][plugin-boundary-continuity][clap]") {
    ClapSession gpu(&make_gpu_effect), cpu(&make_cpu_effect);
    for (auto* s : {&gpu, &cpu}) {
        s->activate(48000.0, 64);
        s->render(16 * 64, {64});
    }
    // A short stall. With a lead of two quanta the first two starved callbacks
    // still consume results produced before the stall; the third misses.
    for (auto* s : {&gpu, &cpu}) {
        s->inject(Fault::StarveWorker);
        s->render(3 * 64, {64});
        s->inject(Fault::FeedWorker);
    }
    const auto stalled = delivery(gpu);
    CHECK(stalled.cpu_fallback_blocks > 0);
    for (auto* s : {&gpu, &cpu})
        s->render(24 * 64, {64, 30, 34});
    const auto recovered = delivery(gpu);
    CHECK(recovered.gpu_blocks > stalled.gpu_blocks);
    CHECK(recovered.silence_blocks == 0);
    CHECK_FALSE(gpu.gpu()->node()->fenced());

    // A long stall overruns the bridge and fences; audio must not notice.
    for (auto* s : {&gpu, &cpu}) {
        s->inject(Fault::StarveWorker);
        s->render(12 * 64, {64, 7, 57});
        s->inject(Fault::FeedWorker);
        s->render(12 * 64, {64});
    }
    CHECK(gpu.gpu()->node()->fenced());
    CHECK(delivery(gpu).silence_blocks == 0);
    CHECK_FALSE(gpu.gpu()->plan().sequence_gap_seen);
    CHECK(gpu.callback_allocations() == 0);
    require_null(gpu, cpu);
}

TEST_CASE("GPU-audio plug-in stays continuous across reactivation with new rate and block size",
          "[gpu_audio][plugin-boundary-continuity][clap]") {
    ClapSession gpu(&make_gpu_effect), cpu(&make_cpu_effect);
    for (auto* s : {&gpu, &cpu}) {
        s->activate(48000.0, 64);
        s->render(12 * 64, {64, 19});
        s->inject(Fault::ProviderLost);
        s->render(12 * 64, {64, 19});
    }
    REQUIRE(gpu.gpu()->node()->fenced());

    // The host changes sample rate and buffer size. The fixture builds a fresh
    // node and provider session on activation (a host reactivating on a
    // recovered device), so this checks that the adapter's deactivate/activate
    // path hands the fallback-to-GPU switch a clean timeline and the new
    // latency. It does not exercise epoch hygiene on a re-prepared node.
    for (auto* s : {&gpu, &cpu}) {
        s->activate(96000.0, 192);
        s->render(16 * 192, {192, 100, 5});
    }
    CHECK(gpu.gpu()->quantum() == 256);
    CHECK(gpu.latency() == cpu.latency());
    CHECK(gpu.latency() == (kLeadBlocks + 1) * 256);
    CHECK(gpu.gpu()->transport().status_snapshot().sample_rate == 96000);
    CHECK_FALSE(gpu.gpu()->node()->fenced());
    CHECK(delivery(gpu).gpu_blocks > 0);
    CHECK(delivery(gpu).cpu_fallback_blocks == 0);

    // Loss again in the new activation, then a third activation at a small,
    // non-power-of-two buffer that keeps the provider down from the start.
    for (auto* s : {&gpu, &cpu}) {
        s->inject(Fault::FailCompletions);
        s->render(10 * 192, {192, 64});
        s->activate(44100.0, 37);
        s->inject(Fault::RejectSubmissions);
        s->render(40 * 37, {37, 1, 36});
    }
    CHECK(gpu.gpu()->quantum() == 64);
    CHECK(gpu.latency() == cpu.latency());
    CHECK(delivery(gpu).gpu_blocks == 0);
    CHECK(delivery(gpu).cpu_fallback_blocks > 0);
    CHECK(delivery(gpu).silence_blocks == 0);
    CHECK(gpu.callback_allocations() == 0);
    require_null(gpu, cpu);
}

TEST_CASE("GPU-audio plug-in keeps adapter bypass and state restore aligned through a fallback",
          "[gpu_audio][plugin-boundary-continuity][clap]") {
    ClapSession gpu(&make_gpu_effect), cpu(&make_cpu_effect);
    std::vector<std::uint8_t> gpu_state, cpu_state;
    for (auto* s : {&gpu, &cpu}) {
        s->activate(48000.0, 128);
        s->set_param(kGainParam, 0.75);
        s->render(10 * 128, {128, 77});
        // Bypass engaged while healthy, released after the provider is lost:
        // the transition back into the plug-in path lands on the fallback.
        s->set_param(kBypassParam, 1.0);
        s->render(6 * 128, {128, 77});
        s->inject(Fault::ProviderLost);
        s->render(4 * 128, {128});
        s->set_param(kBypassParam, 0.0);
        s->render(10 * 128, {128, 77});
    }
    REQUIRE(gpu.gpu()->node()->fenced());
    for (auto* s : {&gpu, &cpu}) {
        auto& saved = s == &gpu ? gpu_state : cpu_state;
        saved = s->save_state();
        s->set_param(kGainParam, 1.5);
        s->render(6 * 128, {128, 3});
        s->load_state(saved);
        s->render(10 * 128, {128, 77});
        // Bypass while on the fallback and back again.
        s->set_param(kBypassParam, 1.0);
        s->render(5 * 128, {128});
        s->set_param(kBypassParam, 0.0);
        s->render(10 * 128, {128, 61});
    }
    CHECK(delivery(gpu).cpu_fallback_blocks > 0);
    CHECK(delivery(gpu).silence_blocks == 0);
    CHECK(gpu.callback_allocations() == 0);
    require_null(gpu, cpu);
}

TEST_CASE("GPU-audio continuity null detects a missing or misaligned fallback",
          "[gpu_audio][plugin-boundary-continuity][clap][negative-control]") {
    // Both planted faults pass the healthy null, so the failure below is caused
    // by the fallback transition and nothing else.
    const auto planted = GENERATE(0, 1);
    CAPTURE(planted);
    FixtureOptions options;
    if (planted == 0)
        options.miss_policy = MissPolicy::Silence;
    else
        options.shadow_delay_blocks = kLeadBlocks - 1;
    OptionsScope scope(options);

    ClapSession healthy_gpu(&make_gpu_effect), healthy_cpu(&make_cpu_effect);
    for (auto* s : {&healthy_gpu, &healthy_cpu}) {
        s->activate(48000.0, 64);
        s->render(24 * 64, {64, 37});
    }
    require_null(healthy_gpu, healthy_cpu);

    ClapSession gpu(&make_gpu_effect), cpu(&make_cpu_effect);
    for (auto* s : {&gpu, &cpu}) {
        s->activate(48000.0, 64);
        s->render(12 * 64, {64, 37});
        s->inject(Fault::ProviderLost);
        s->render(12 * 64, {64, 37});
    }
    const auto null = analysis::assert_null_near(gpu.output(), cpu.output());
    INFO(null.message);
    CHECK_FALSE(null.passed);
    if (planted == 0)
        CHECK(delivery(gpu).silence_blocks > 0);
    else
        CHECK(delivery(gpu).cpu_fallback_blocks > 0);
}

TEST_CASE("GPU-audio plug-in nulls through HeadlessHost across loss and re-prepare",
          "[gpu_audio][plugin-boundary-continuity][headless]") {
    format::HeadlessHost gpu(&make_gpu_effect), cpu(&make_cpu_effect);
    std::array<std::vector<float>, kChannels> gpu_out, cpu_out;
    std::uint64_t frame = 0;
    auto render = [&](std::uint64_t frames, std::uint32_t block) {
        while (frames > 0) {
            const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(block, frames));
            audio::Buffer<float> in_n(kChannels, n), a_n(kChannels, n), b_n(kChannels, n);
            for (std::uint32_t ch = 0; ch < kChannels; ++ch)
                for (std::uint32_t i = 0; i < n; ++i)
                    in_n.channel(ch)[i] = stimulus(ch, frame + i);
            const auto in_view = std::as_const(in_n).view();
            auto a_view = a_n.view();
            auto b_view = b_n.view();
            gpu.process(a_view, in_view);
            cpu.process(b_view, in_view);
            for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
                gpu_out[ch].insert(gpu_out[ch].end(), a_n.channel(ch).begin(),
                                   a_n.channel(ch).end());
                cpu_out[ch].insert(cpu_out[ch].end(), b_n.channel(ch).begin(),
                                   b_n.channel(ch).end());
            }
            if (auto* fx = gpu.processor_as<GpuFirEffect>())
                for (int i = 0; i < 4; ++i)
                    fx->transport().pump();
            frame += n;
            frames -= n;
        }
    };
    gpu.prepare(48000.0, 100);
    cpu.prepare(48000.0, 100);
    auto* fx = gpu.processor_as<GpuFirEffect>();
    REQUIRE(fx != nullptr);
    REQUIRE(fx->engine_ready());
    render(20 * 100, 100);
    detail::WaveNetRealtimeTestAccess::request_recovery(
        *fx->node(), detail::SharedIoRecoveryReason::ProviderLost);
    render(20 * 100, 77);
    CHECK(fx->node()->fenced());
    CHECK(fx->transport().delivery_snapshot().cpu_fallback_blocks > 0);

    const auto saved = gpu.save_state();
    REQUIRE(gpu.load_state(saved));
    gpu.prepare(44100.0, 32);
    cpu.prepare(44100.0, 32);
    REQUIRE(fx->engine_ready());
    render(40 * 32, 32);
    CHECK_FALSE(fx->node()->fenced());
    CHECK(fx->transport().delivery_snapshot().gpu_blocks > 0);

    audio::Buffer<float> actual(kChannels, gpu_out[0].size()),
        expected(kChannels, cpu_out[0].size());
    for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
        std::copy(gpu_out[ch].begin(), gpu_out[ch].end(), actual.channel(ch).begin());
        std::copy(cpu_out[ch].begin(), cpu_out[ch].end(), expected.channel(ch).begin());
    }
    CHECK(analysis::assert_no_nan_inf(analysis::analyze(actual, 48000.0)).passed);
    CHECK(analysis::assert_not_silent(analysis::analyze(actual, 48000.0)).passed);
    const auto null = analysis::assert_null_near(actual, expected);
    INFO(null.message);
    CHECK(null.passed);
}

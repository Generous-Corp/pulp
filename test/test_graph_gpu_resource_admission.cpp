// Two independent GPU-audio consumers inside one SignalGraph, admitted through
// one shared, test-local resource admission manifest.
//
// Consumers (serial amp-and-cabinet chain, each a format::Processor node):
//   * neural: GpuNeuralRealtimeNode, WaveNet family, built through the public
//     make_gpu_neural_model factory with a real Dawn WaveNet session (the
//     GpuWaveNetSession forward that GPU NAM uses) and an exact inline CPU
//     shadow.
//   * convolution: GpuConvolver on the staged Dawn GpuCompute path with its
//     continuously primed CPU fallback.
// Both run through GpuAudioTransport behind a one-quantum staging FIFO.
//
// The manifest names, per consumer: resource identity (SHA-256 of the bytes
// handed to the node), provider claim, residency, a declared payload budget,
// a latency budget, the expected device-loss transition, the CPU fallback
// requirement and an output tolerance. Admission compares it against what the
// prepared graph reports; geometry and latency are measured per fixture by
// lag search against an independent zero-latency CPU oracle.
//
// Device loss is fault-injected, never a hardware loss: the convolver's real
// Dawn device is put in the lost state through GpuCompute::notify_device_lost
// (the adopted-device loss notification), and the neural node latches
// ProviderLost through its private recovery seam, which is the reason the Dawn
// shared-I/O bridge raises on loss. The manifest is test-local evidence for
// the admission gate, not an SDK API.

#include "detail/gpu_convolver_test_access.hpp"
#include "detail/gpu_neural_realtime_test_access.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_neural_model.hpp>
#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>
#include <pulp/gpu_audio/gpu_wavenet.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::gpu_audio;

namespace {

// ---------------------------------------------------------------------------
// Resources. One definition each, shared by the GPU consumer and the oracle.
// ---------------------------------------------------------------------------

/// One-layer WaveNet: rechannel, two-tap causal conv, tanh, head. Weight
/// order is the session's flat blob; with these values the forward is
/// y[t] = tanh(kPrevTap * x[t-1]).
constexpr float kPrevTap = 0.5f;
struct NeuralResource {
    GpuWaveNetLayerDescriptor layer{.input_size = 1,
                                    .condition_size = 1,
                                    .channels = 1,
                                    .kernel = 2,
                                    .head_size = 1,
                                    .dilation = 1,
                                    .gated = false,
                                    .head_bias = false,
                                    .tanh_activation = true};
    std::array<float, 9> weights{1.0f, kPrevTap, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
};

/// Deterministic decaying cabinet-style IR.
std::vector<float> make_ir() {
    std::vector<float> ir(384);
    std::uint32_t seed = 0x1234567u;
    for (std::size_t i = 0; i < ir.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float noise = static_cast<float>((seed >> 8) & 0xffff) / 32768.0f - 1.0f;
        ir[i] = 0.35f * noise * std::exp(-static_cast<float>(i) / 60.0f);
    }
    ir[0] = 0.6f;
    return ir;
}

std::string sha256_of(const float* data, std::size_t count) {
    return runtime::sha256_hex(reinterpret_cast<const std::uint8_t*>(data), count * sizeof(float));
}

float stimulus(std::uint64_t frame) {
    std::uint32_t seed = 0x9e3779b9u ^ static_cast<std::uint32_t>(frame * 2654435761u);
    seed ^= seed >> 15;
    seed *= 0x2c1b3c6du;
    seed ^= seed >> 12;
    const float noise = static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
    const float tone = static_cast<float>(
        std::sin(2.0 * 3.14159265358979323846 * 220.0 * static_cast<double>(frame) / 48000.0));
    return 0.3f * noise + 0.4f * tone;
}

/// Exact CPU shadow for NeuralResource, used by the node as its fallback.
struct TinyWaveNetCpu {
    float prev = 0.0f;
    static bool prepare(void* s) noexcept {
        static_cast<TinyWaveNetCpu*>(s)->prev = 0.0f;
        return true;
    }
    static void process(void* s, const float* in, float* out, std::uint32_t n) noexcept {
        auto& self = *static_cast<TinyWaveNetCpu*>(s);
        for (std::uint32_t i = 0; i < n; ++i) {
            out[i] = std::tanh(kPrevTap * self.prev);
            self.prev = in[i];
        }
    }
    static void reset(void* s) noexcept {
        static_cast<TinyWaveNetCpu*>(s)->prev = 0.0f;
    }
    GpuNeuralCpuKernel kernel() {
        return {.state = this, .prepare = &prepare, .process = &process, .reset = &reset};
    }
};

/// Independent zero-latency oracle for the whole chain: the WaveNet function
/// followed by a direct-form FIR. Shares no node, transport or staging code.
std::vector<float> oracle(const std::vector<float>& input, const std::vector<float>& ir) {
    std::vector<float> amp(input.size());
    float prev = 0.0f;
    for (std::size_t t = 0; t < input.size(); ++t) {
        amp[t] = std::tanh(kPrevTap * prev);
        prev = input[t];
    }
    std::vector<float> out(input.size(), 0.0f);
    for (std::size_t t = 0; t < input.size(); ++t) {
        double acc = 0.0;
        const std::size_t taps = std::min(ir.size(), t + 1);
        for (std::size_t k = 0; k < taps; ++k)
            acc += static_cast<double>(ir[k]) * amp[t - k];
        out[t] = static_cast<float>(acc);
    }
    return out;
}

std::uint32_t quantum_for(int max_block) {
    std::uint32_t q = 16;
    while (q < static_cast<std::uint32_t>(std::max(max_block, 1)))
        q <<= 1;
    return q;
}

// ---------------------------------------------------------------------------
// Consumers.
// ---------------------------------------------------------------------------

format::PluginDescriptor mono_descriptor(const char* name) {
    format::PluginDescriptor d;
    d.name = name;
    d.manufacturer = "PulpTest";
    d.bundle_id = "com.pulp.test.graph-gpu-resource-admission";
    d.version = "1.0.0";
    d.category = format::PluginCategory::Effect;
    d.input_buses = {{"In", 1}};
    d.output_buses = {{"Out", 1}};
    return d;
}

/// Stages host blocks of any size into one fixed quantum and runs it through
/// a GpuAudioTransport. The quantum adds exactly one quantum of latency.
class StagedGpuConsumer : public format::Processor {
  public:
    void define_parameters(state::StateStore&) override {}
    bool has_editor() const override {
        return false;
    }
    void prepare(const format::PrepareContext& ctx) override {
        release_engine();
        quantum_ = quantum_for(ctx.max_buffer_size);
        sample_rate_ = static_cast<std::uint32_t>(ctx.sample_rate);
        ready_ = build(quantum_, sample_rate_) && transport_.prepare(node(), {});
        staged_.assign(quantum_, 0.0f);
        wet_.assign(quantum_, 0.0f);
        fifo_.assign(2u * quantum_, 0.0f);
        fill_ = 0;
        read_ = 0;
        write_ = quantum_;
    }
    void release() override {
        release_engine();
    }
    int latency_samples() const override {
        return static_cast<int>(transport_.latency_samples() + quantum_);
    }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        if (!ready_) {
            out.clear();
            return;
        }
        const auto size = 2u * quantum_;
        for (std::size_t i = 0; i < out.num_samples(); ++i) {
            staged_[fill_] = in.num_channels() > 0 ? in.channel(0)[i] : 0.0f;
            out.channel(0)[i] = fifo_[read_];
            read_ = (read_ + 1) % size;
            if (++fill_ == quantum_) {
                const float* ip = staged_.data();
                float* op = wet_.data();
                audio::BufferView<const float> qi(&ip, 1, quantum_);
                audio::BufferView<float> qo(&op, 1, quantum_);
                transport_.process(qi, qo, quantum_);
                for (std::uint32_t k = 0; k < quantum_; ++k)
                    fifo_[(write_ + k) % size] = wet_[k];
                write_ = (write_ + quantum_) % size;
                fill_ = 0;
            }
        }
    }

    /// Non-realtime service context: drive the transport worker.
    void service() noexcept {
        transport_.pump();
    }
    GpuAudioTransport& transport() noexcept {
        return transport_;
    }
    bool ready() const noexcept {
        return ready_;
    }
    std::uint32_t quantum() const noexcept {
        return quantum_;
    }
    virtual GpuAudioNode* node() noexcept = 0;
    virtual std::string identity_sha256() const = 0;
    virtual std::uint64_t payload_bytes() const = 0;
    virtual void inject_device_loss() = 0;

  protected:
    virtual bool build(std::uint32_t quantum, std::uint32_t sample_rate) = 0;
    virtual void release_node() noexcept = 0;
    void release_engine() noexcept {
        transport_.release();
        release_node();
        ready_ = false;
    }

    GpuAudioTransport transport_;

  private:
    bool ready_ = false;
    std::uint32_t quantum_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::vector<float> staged_, wet_, fifo_;
    std::uint32_t fill_ = 0, read_ = 0, write_ = 0;
};

struct NeuralOptions {
    /// False is a planted negative: no CPU shadow, so a miss is silent.
    bool cpu_shadow = true;
};

class NeuralConsumer final : public StagedGpuConsumer {
  public:
    explicit NeuralConsumer(NeuralOptions options) : options_(options) {}
    ~NeuralConsumer() override {
        release_engine();
    }
    format::PluginDescriptor descriptor() const override {
        return mono_descriptor("P4NeuralConsumer");
    }
    GpuAudioNode* node() noexcept override {
        return node_.get();
    }
    GpuNeuralRealtimeNode* neural() noexcept {
        return node_.get();
    }
    std::string identity_sha256() const override {
        return sha256_of(resource_.weights.data(), resource_.weights.size());
    }
    std::uint64_t payload_bytes() const override {
        return resource_.weights.size() * sizeof(float);
    }
    void inject_device_loss() override {
        detail::NeuralRealtimeTestAccess::request_recovery(
            *node_, detail::SharedIoRecoveryReason::ProviderLost);
    }
    /// Planted negative: alter one weight after the manifest digest was taken.
    void plant_weight_drift() {
        resource_.weights[1] += 0.25f;
    }

  protected:
    bool build(std::uint32_t quantum, std::uint32_t sample_rate) override {
        session_ = {.descriptor = {.block_size = quantum,
                                   .sample_rate = sample_rate,
                                   .stream_instances = 1,
                                   .head_scale = 1.0f,
                                   .layers = {&resource_.layer, 1},
                                   .weight_count = resource_.weights.size()},
                    .weights = resource_.weights,
                    .slots = 4};
        sha_ = identity_sha256();
        kernels_[0] = shadow_.kernel();
        GpuNeuralModelRequest request;
        request.family = NeuralFamily::WaveNet;
        request.manifest = {.model_id = "pulp.test.p4.wavenet-one-layer",
                            .architecture = "nam.wavenet",
                            .model_version = "1",
                            .artifact_id = "pulp.test.p4.wavenet-one-layer.weights",
                            .artifact_sha256 = sha_,
                            .license = "MIT",
                            .runtime = "pulp.gpu-wavenet.v1",
                            .artifact_size_bytes = payload_bytes(),
                            .sample_rate_hz = sample_rate,
                            .redistributable = true};
        request.block_size = quantum;
        request.channel_kernels = kernels_;
        request.wavenet_gpu = &session_;
        auto model = make_gpu_neural_model(request);
        if (!model)
            return false;
        node_ = std::make_unique<GpuNeuralRealtimeNode>(GpuNeuralRealtimeNode::Config{
            .family = NeuralFamily::WaveNet,
            .model = model.model,
            .channels = 1,
            .lead_blocks = 2,
            .capacity = 6,
            .miss_policy = MissPolicy::CpuFallback,
            .shadow_policy = options_.cpu_shadow ? GpuNeuralRealtimeNode::ShadowPolicy::Inline
                                                 : GpuNeuralRealtimeNode::ShadowPolicy::None});
        return node_->prepare();
    }
    void release_node() noexcept override {
        if (node_)
            (void)node_->release();
        node_.reset();
    }

  private:
    NeuralOptions options_;
    NeuralResource resource_;
    GpuWaveNetSession::Config session_{};
    std::string sha_;
    TinyWaveNetCpu shadow_;
    std::array<GpuNeuralCpuKernel, 1> kernels_{};
    std::unique_ptr<GpuNeuralRealtimeNode> node_;
};

class ConvolutionConsumer final : public StagedGpuConsumer {
  public:
    ConvolutionConsumer() : ir_(make_ir()) {}
    ~ConvolutionConsumer() override {
        release_engine();
    }
    format::PluginDescriptor descriptor() const override {
        return mono_descriptor("P4ConvolutionConsumer");
    }
    GpuAudioNode* node() noexcept override {
        return node_.get();
    }
    GpuConvolver* convolver() noexcept {
        return node_.get();
    }
    std::string identity_sha256() const override {
        return sha256_of(ir_.data(), ir_.size());
    }
    std::uint64_t payload_bytes() const override {
        return ir_.size() * sizeof(float);
    }
    void inject_device_loss() override {
        if (auto* device = detail::GpuConvolverTestAccess::staged_compute(*node_))
            device->notify_device_lost();
    }
    bool has_device() noexcept {
        return node_ && detail::GpuConvolverTestAccess::staged_compute(*node_) != nullptr;
    }

  protected:
    bool build(std::uint32_t quantum, std::uint32_t sample_rate) override {
        node_ = std::make_unique<GpuConvolver>(1, quantum, sample_rate, ir_);
        return node_->set_provider_policy(GpuConvolver::ProviderPolicy::StagedOnly) &&
               node_->prepare();
    }
    void release_node() noexcept override {
        node_.reset();
    }

  private:
    std::vector<float> ir_;
    std::unique_ptr<GpuConvolver> node_;
};

// ---------------------------------------------------------------------------
// Shared admission manifest.
// ---------------------------------------------------------------------------

enum class ProviderClaim : std::uint8_t { AttestedDawn, Unattested };
enum class Residency : std::uint8_t { ProviderShared, HostStaged };

struct AdmissionEntry {
    std::string consumer;
    std::string resource_kind; // "model" or "kernel"
    std::string identity_sha256;
    ProviderClaim provider = ProviderClaim::Unattested;
    Residency residency = Residency::HostStaged;
    std::uint64_t payload_budget_bytes = 0;
    double latency_budget_ms = 0.0;
    GpuAudioRecoveryReason device_loss_transition = GpuAudioRecoveryReason::ProviderLost;
    bool cpu_fallback_required = true;
    double output_tolerance_abs = 0.0;
};

struct AdmissionManifest {
    static constexpr const char* kSchema = "pulp.test.graph-resource-admission.v0";
    std::vector<AdmissionEntry> entries;
};

struct ConsumerObservation {
    std::string consumer;
    std::string identity_sha256;
    std::uint64_t payload_bytes = 0;
    GpuAudioCapabilityReport capability{};
    GpuAudioStatus status{};
    std::int64_t graph_latency_samples = 0;
    double sample_rate = 0.0;
};

enum class AdmissionError : std::uint8_t {
    None,
    MissingConsumer,
    UnknownConsumer,
    IdentityMismatch,
    CapabilityInvalid,
    ProviderMismatch,
    ResidencyMismatch,
    FallbackMissing,
    PayloadOverBudget,
    LatencyOverBudget,
};

const char* to_string(AdmissionError e) {
    switch (e) {
    case AdmissionError::None:
        return "none";
    case AdmissionError::MissingConsumer:
        return "missing-consumer";
    case AdmissionError::UnknownConsumer:
        return "unknown-consumer";
    case AdmissionError::IdentityMismatch:
        return "identity-mismatch";
    case AdmissionError::CapabilityInvalid:
        return "capability-invalid";
    case AdmissionError::ProviderMismatch:
        return "provider-mismatch";
    case AdmissionError::ResidencyMismatch:
        return "residency-mismatch";
    case AdmissionError::FallbackMissing:
        return "fallback-missing";
    case AdmissionError::PayloadOverBudget:
        return "payload-over-budget";
    case AdmissionError::LatencyOverBudget:
        return "latency-over-budget";
    }
    return "?";
}

ProviderClaim observed_provider(const ConsumerObservation& o) {
    // Only a provider that names itself in the status counts as attested.
    return o.status.provider == GpuAudioProvider::Dawn ? ProviderClaim::AttestedDawn
                                                       : ProviderClaim::Unattested;
}

std::optional<Residency> observed_residency(const ConsumerObservation& o) {
    switch (o.capability.path) {
    case GpuAudioExecutionPath::SharedMemory:
        return Residency::ProviderShared;
    case GpuAudioExecutionPath::Staged:
        return Residency::HostStaged;
    default:
        return std::nullopt;
    }
}

/// Pre-render admission: every manifest entry must match exactly one prepared
/// consumer, and every consumer must be named.
struct AdmissionVerdict {
    AdmissionError error = AdmissionError::None;
    std::string consumer;
};

AdmissionVerdict admit(const AdmissionManifest& manifest,
                       const std::vector<ConsumerObservation>& observed) {
    for (const auto& o : observed) {
        if (std::none_of(manifest.entries.begin(), manifest.entries.end(),
                         [&](const AdmissionEntry& e) { return e.consumer == o.consumer; }))
            return {AdmissionError::UnknownConsumer, o.consumer};
    }
    for (const auto& e : manifest.entries) {
        const auto it =
            std::find_if(observed.begin(), observed.end(),
                         [&](const ConsumerObservation& o) { return o.consumer == e.consumer; });
        if (it == observed.end())
            return {AdmissionError::MissingConsumer, e.consumer};
        const auto& o = *it;
        if (o.identity_sha256 != e.identity_sha256)
            return {AdmissionError::IdentityMismatch, e.consumer};
        if (!validate_gpu_audio_capability(o.capability).accepted() || !o.capability.prepared ||
            !validate_gpu_audio_status(o.status).accepted())
            return {AdmissionError::CapabilityInvalid, e.consumer};
        if (observed_provider(o) != e.provider)
            return {AdmissionError::ProviderMismatch, e.consumer};
        if (observed_residency(o) != e.residency)
            return {AdmissionError::ResidencyMismatch, e.consumer};
        if (e.cpu_fallback_required && (o.capability.fallback_policy != MissPolicy::CpuFallback ||
                                        !o.capability.fallback_available))
            return {AdmissionError::FallbackMissing, e.consumer};
        if (o.payload_bytes > e.payload_budget_bytes)
            return {AdmissionError::PayloadOverBudget, e.consumer};
        const double latency_ms =
            1000.0 * static_cast<double>(o.graph_latency_samples) / o.sample_rate;
        if (latency_ms > e.latency_budget_ms)
            return {AdmissionError::LatencyOverBudget, e.consumer};
    }
    return {};
}

AdmissionManifest reference_manifest(const NeuralConsumer& neural,
                                     const ConvolutionConsumer& convolution) {
    AdmissionManifest m;
    m.entries.push_back({.consumer = "neural.wavenet",
                         .resource_kind = "model",
                         .identity_sha256 = neural.identity_sha256(),
                         .provider = ProviderClaim::AttestedDawn,
                         .residency = Residency::ProviderShared,
                         .payload_budget_bytes = 4096,
                         .latency_budget_ms = 20.0,
                         .output_tolerance_abs = 1.0e-4});
    // The staged convolver does not name its provider, so the honest claim is
    // Unattested; claiming Dawn is a planted negative below.
    m.entries.push_back({.consumer = "convolution.fir",
                         .resource_kind = "kernel",
                         .identity_sha256 = convolution.identity_sha256(),
                         .provider = ProviderClaim::Unattested,
                         .residency = Residency::HostStaged,
                         .payload_budget_bytes = 4096,
                         .latency_budget_ms = 20.0,
                         .output_tolerance_abs = 1.0e-4});
    return m;
}

// ---------------------------------------------------------------------------
// Graph fixture.
// ---------------------------------------------------------------------------

struct GeometryFixture {
    const char* name;
    double sample_rate;
    int max_block;
    std::vector<int> pattern; // host block sizes, cycled
};

struct GraphRig {
    host::SignalGraph graph;
    NeuralConsumer* neural = nullptr;
    ConvolutionConsumer* convolution = nullptr;
    host::NodeId neural_id = 0;
    host::NodeId convolution_id = 0;
    std::vector<float> input, output;
    std::size_t callback_allocations = 0;
    std::uint64_t frame = 0;

    explicit GraphRig(NeuralOptions neural_options = {}) {
        auto n = std::make_unique<NeuralConsumer>(neural_options);
        auto c = std::make_unique<ConvolutionConsumer>();
        neural = n.get();
        convolution = c.get();
        const auto in = graph.add_input_node(1);
        neural_id = graph.add_processor_node(std::move(n), "neural.wavenet");
        convolution_id = graph.add_processor_node(std::move(c), "convolution.fir");
        const auto out = graph.add_output_node(1);
        REQUIRE(graph.connect(in, 0, neural_id, 0));
        REQUIRE(graph.connect(neural_id, 0, convolution_id, 0));
        REQUIRE(graph.connect(convolution_id, 0, out, 0));
    }

    bool prepare(const GeometryFixture& f) {
        return graph.prepare(f.sample_rate, f.max_block) && neural->ready() && convolution->ready();
    }

    std::vector<ConsumerObservation> observe(double sample_rate) {
        const auto snapshot = graph.prepared_latency_snapshot();
        REQUIRE(snapshot.has_value());
        const auto artifact = [&](host::NodeId id) {
            const auto node = snapshot->node(id);
            REQUIRE(node.has_value());
            return node->artifact_latency_samples;
        };
        return {{.consumer = "neural.wavenet",
                 .identity_sha256 = neural->identity_sha256(),
                 .payload_bytes = neural->payload_bytes(),
                 .capability = neural->transport().capability_report(),
                 .status = neural->transport().status_snapshot(),
                 .graph_latency_samples = artifact(neural_id),
                 .sample_rate = sample_rate},
                {.consumer = "convolution.fir",
                 .identity_sha256 = convolution->identity_sha256(),
                 .payload_bytes = convolution->payload_bytes(),
                 .capability = convolution->transport().capability_report(),
                 .status = convolution->transport().status_snapshot(),
                 .graph_latency_samples = artifact(convolution_id),
                 .sample_rate = sample_rate}};
    }

    /// One host callback inside the allocation probe, then non-realtime
    /// service with a short wait so real GPU completions can land.
    void callback(int n) {
        std::vector<float> in(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            in[static_cast<std::size_t>(i)] = stimulus(frame + static_cast<std::uint64_t>(i));
        const float* ip = in.data();
        float* op = out.data();
        audio::BufferView<const float> iv(&ip, 1, static_cast<std::size_t>(n));
        audio::BufferView<float> ov(&op, 1, static_cast<std::size_t>(n));
        std::size_t allocations = 0;
        {
            pulp::test::RtAllocationProbe probe;
            graph.process(ov, iv, n);
            allocations = probe.allocation_count();
        }
        callback_allocations += allocations;
        input.insert(input.end(), in.begin(), in.end());
        output.insert(output.end(), out.begin(), out.end());
        frame += static_cast<std::uint64_t>(n);
        for (int k = 0; k < 3; ++k) {
            neural->service();
            convolution->service();
            std::this_thread::sleep_for(std::chrono::microseconds(150));
        }
    }

    void render(std::uint64_t frames, const std::vector<int>& pattern) {
        std::size_t next = 0;
        while (frames > 0) {
            const auto n = static_cast<int>(
                std::min<std::uint64_t>(static_cast<std::uint64_t>(pattern[next]), frames));
            next = (next + 1) % pattern.size();
            callback(n);
            frames -= static_cast<std::uint64_t>(n);
        }
    }
};

/// Lag (samples) that best aligns the graph output with the zero-latency
/// oracle, and the worst absolute error at that lag.
struct Alignment {
    std::int64_t lag = -1;
    double max_abs = 1.0e9;
};

Alignment measure_lag(const std::vector<float>& out, const std::vector<float>& ref,
                      std::int64_t max_lag) {
    Alignment best;
    const auto n = static_cast<std::int64_t>(out.size());
    for (std::int64_t lag = 0; lag <= max_lag && lag < n; ++lag) {
        double worst = 0.0;
        for (std::int64_t t = lag; t < n; ++t)
            worst = std::max(worst, std::abs(static_cast<double>(out[static_cast<std::size_t>(t)]) -
                                             ref[static_cast<std::size_t>(t - lag)]));
        if (worst < best.max_abs)
            best = {lag, worst};
    }
    return best;
}

double max_error_at(const std::vector<float>& out, const std::vector<float>& ref,
                    std::int64_t lag) {
    double worst = 0.0;
    for (std::size_t t = static_cast<std::size_t>(lag); t < out.size(); ++t)
        worst = std::max(worst, std::abs(static_cast<double>(out[t]) - ref[t - lag]));
    return worst;
}

/// Escapes the planted allocation so the compiler cannot elide it.
std::unique_ptr<std::vector<float>> g_allocation_sink;

std::size_t silent_quanta(const std::vector<float>& out, std::size_t from, std::uint32_t q) {
    std::size_t silent = 0;
    for (std::size_t start = from; start + q <= out.size(); start += q) {
        bool all_zero = true;
        for (std::size_t i = 0; i < q && all_zero; ++i)
            all_zero = out[start + i] == 0.0f;
        silent += all_zero ? 1u : 0u;
    }
    return silent;
}

const char* provider_name(GpuAudioProvider p) {
    switch (p) {
    case GpuAudioProvider::Dawn:
        return "dawn";
    case GpuAudioProvider::Metal:
        return "metal";
    case GpuAudioProvider::Cpu:
        return "cpu";
    default:
        return "unknown";
    }
}

const char* path_name(GpuAudioExecutionPath p) {
    switch (p) {
    case GpuAudioExecutionPath::Staged:
        return "staged";
    case GpuAudioExecutionPath::SharedMemory:
        return "shared_memory";
    case GpuAudioExecutionPath::Cpu:
        return "cpu";
    default:
        return "unavailable";
    }
}

/// Skips when this host cannot prepare both real GPU consumers.
void require_gpu_host(GraphRig& rig, const GeometryFixture& f) {
    if (!rig.prepare(f))
        SKIP("graph consumers could not be prepared on this host");
    const auto status = rig.neural->transport().status_snapshot();
    if (status.provider != GpuAudioProvider::Dawn)
        SKIP("authenticated Dawn WaveNet provider unavailable on this host");
    if (!rig.convolution->has_device())
        SKIP("no staged Dawn device for the convolver on this host");
}

} // namespace

TEST_CASE("two GPU consumers in one graph pass shared admission with measured geometry",
          "[gpu_audio][graph][resource-admission][p4]") {
    const auto fixture =
        GENERATE(GeometryFixture{"sr48k-b64", 48000.0, 64, {64}},
                 GeometryFixture{"sr44k1-b256-ragged", 44100.0, 256, {256, 37, 1, 200, 256, 18}});
    CAPTURE(fixture.name);
    GraphRig rig;
    require_gpu_host(rig, fixture);

    const auto observed = rig.observe(fixture.sample_rate);
    const auto manifest = reference_manifest(*rig.neural, *rig.convolution);
    const auto verdict = admit(manifest, observed);
    INFO("admission: " << to_string(verdict.error) << " " << verdict.consumer);
    REQUIRE(verdict.error == AdmissionError::None);

    rig.render(static_cast<std::uint64_t>(fixture.max_block) * 160, fixture.pattern);

    // Geometry and latency are measured, not assumed.
    const auto q_neural = rig.neural->quantum();
    const auto q_conv = rig.convolution->quantum();
    const auto declared = rig.graph.latency_samples();
    const auto ref = oracle(rig.input, make_ir());
    const auto aligned = measure_lag(rig.output, ref, 4 * declared + 64);
    INFO("measured lag " << aligned.lag << " declared " << declared << " max_abs "
                         << aligned.max_abs);
    CHECK(aligned.lag == declared);
    CHECK(declared == observed[0].graph_latency_samples + observed[1].graph_latency_samples);
    for (const auto& e : manifest.entries)
        CHECK(aligned.max_abs <= e.output_tolerance_abs);

    const auto neural_delivery = rig.neural->transport().delivery_snapshot();
    const auto conv_delivery = rig.convolution->transport().delivery_snapshot();
    CHECK(neural_delivery.gpu_blocks > 0);
    CHECK(neural_delivery.silence_blocks == 0);
    CHECK(conv_delivery.silence_blocks == 0);
    CHECK(neural_delivery.invalid_blocks == 0);
    CHECK(conv_delivery.invalid_blocks == 0);
    CHECK(rig.callback_allocations == 0);

    const auto& nc = observed[0].capability;
    const auto& cc = observed[1].capability;
    std::cout << "{\"schema\":\"pulp.test.graph-resource-admission.fixture.v0\",\"fixture\":\""
              << fixture.name << "\",\"sample_rate\":" << fixture.sample_rate
              << ",\"max_block\":" << fixture.max_block << ",\"neural\":{\"quantum\":" << q_neural
              << ",\"transport_latency_samples\":" << rig.neural->transport().latency_samples()
              << ",\"graph_artifact_latency_samples\":" << observed[0].graph_latency_samples
              << ",\"path\":\"" << path_name(nc.path) << "\",\"provider\":\""
              << provider_name(observed[0].status.provider)
              << "\",\"prepared_lead_blocks\":" << nc.prepared_lead_blocks
              << ",\"gpu_blocks\":" << neural_delivery.gpu_blocks
              << ",\"cpu_fallback_blocks\":" << neural_delivery.cpu_fallback_blocks
              << ",\"priming_blocks\":" << neural_delivery.priming_blocks
              << ",\"identity_sha256\":\"" << observed[0].identity_sha256
              << "\"},\"convolution\":{\"quantum\":" << q_conv
              << ",\"fft_size\":" << rig.convolution->convolver()->fft_size()
              << ",\"transport_latency_samples\":" << rig.convolution->transport().latency_samples()
              << ",\"graph_artifact_latency_samples\":" << observed[1].graph_latency_samples
              << ",\"path\":\"" << path_name(cc.path) << "\",\"provider\":\""
              << provider_name(observed[1].status.provider)
              << "\",\"gpu_kernel_available\":" << (cc.gpu_kernel_available ? "true" : "false")
              << ",\"worker_output_blocks\":" << conv_delivery.worker_output_blocks
              << ",\"cpu_fallback_blocks\":" << conv_delivery.cpu_fallback_blocks
              << ",\"identity_sha256\":\"" << observed[1].identity_sha256
              << "\"},\"graph_latency_samples\":" << declared
              << ",\"measured_lag_samples\":" << aligned.lag
              << ",\"max_abs_error\":" << aligned.max_abs
              << ",\"callback_allocations\":" << rig.callback_allocations << "}\n";
}

TEST_CASE("fault-injected device loss moves both graph consumers to their CPU fallback",
          "[gpu_audio][graph][resource-admission][p4]") {
    const GeometryFixture fixture{"sr48k-b64-loss", 48000.0, 64, {64, 21, 43}};
    GraphRig rig;
    require_gpu_host(rig, fixture);
    const auto manifest = reference_manifest(*rig.neural, *rig.convolution);
    REQUIRE(admit(manifest, rig.observe(fixture.sample_rate)).error == AdmissionError::None);

    rig.render(64 * 60, fixture.pattern);
    const auto before_neural = rig.neural->transport().delivery_snapshot();
    REQUIRE(before_neural.gpu_blocks > 0);
    const auto loss_frame = rig.frame;
    rig.neural->inject_device_loss();
    rig.convolution->inject_device_loss();
    rig.render(64 * 80, fixture.pattern);

    // The manifest's declared transition is what each consumer reports.
    for (auto* consumer : std::initializer_list<StagedGpuConsumer*>{rig.neural, rig.convolution}) {
        const auto status = consumer->transport().status_snapshot();
        CHECK(validate_gpu_audio_status(status).accepted());
        CHECK(status.recovery_reason == manifest.entries[0].device_loss_transition);
        CHECK(status.provider_state == GpuAudioProviderState::Unavailable);
        CHECK(consumer->transport().delivery_snapshot().silence_blocks == 0);
        CHECK(consumer->transport().delivery_snapshot().invalid_blocks == 0);
    }
    const auto after_neural = rig.neural->transport().delivery_snapshot();
    CHECK(after_neural.cpu_fallback_blocks > before_neural.cpu_fallback_blocks);
    CHECK(rig.convolution->convolver()->status_snapshot().selected_engine == GpuAudioEngine::Cpu);

    // Continuous output across the transition: the whole render, including
    // the loss boundary, stays within tolerance of the oracle.
    const auto declared = rig.graph.latency_samples();
    const auto ref = oracle(rig.input, make_ir());
    const double worst = max_error_at(rig.output, ref, declared);
    INFO("max abs error across loss " << worst << " loss frame " << loss_frame);
    CHECK(worst <= manifest.entries[0].output_tolerance_abs);
    CHECK(silent_quanta(rig.output, static_cast<std::size_t>(declared), 64) == 0);
    CHECK(rig.callback_allocations == 0);
    std::cout << "{\"schema\":\"pulp.test.graph-resource-admission.loss.v0\",\"fixture\":\""
              << fixture.name << "\",\"injection\":\"fault_injected\",\"loss_frame\":" << loss_frame
              << ",\"neural_cpu_fallback_blocks_before\":" << before_neural.cpu_fallback_blocks
              << ",\"neural_cpu_fallback_blocks_after\":" << after_neural.cpu_fallback_blocks
              << ",\"max_abs_error\":" << worst
              << ",\"callback_allocations\":" << rig.callback_allocations << "}\n";
}

TEST_CASE("shared admission refuses each planted manifest or consumer defect",
          "[gpu_audio][graph][resource-admission][p4]") {
    const GeometryFixture fixture{"sr48k-b64", 48000.0, 64, {64}};
    GraphRig rig;
    require_gpu_host(rig, fixture);
    const auto observed = rig.observe(fixture.sample_rate);
    const auto good = reference_manifest(*rig.neural, *rig.convolution);
    // Positive control.
    REQUIRE(admit(good, observed).error == AdmissionError::None);

    SECTION("identity drift") {
        rig.neural->plant_weight_drift();
        auto drifted = rig.observe(fixture.sample_rate);
        CHECK(admit(good, drifted).error == AdmissionError::IdentityMismatch);
    }
    SECTION("convolver claimed as attested Dawn") {
        auto m = good;
        m.entries[1].provider = ProviderClaim::AttestedDawn;
        CHECK(admit(m, observed).error == AdmissionError::ProviderMismatch);
    }
    SECTION("residency claim") {
        auto m = good;
        m.entries[0].residency = Residency::HostStaged;
        CHECK(admit(m, observed).error == AdmissionError::ResidencyMismatch);
    }
    SECTION("latency budget tighter than the measured geometry") {
        auto m = good;
        m.entries[1].latency_budget_ms = 2.0;
        CHECK(admit(m, observed).error == AdmissionError::LatencyOverBudget);
    }
    SECTION("payload budget") {
        auto m = good;
        m.entries[1].payload_budget_bytes = 64;
        CHECK(admit(m, observed).error == AdmissionError::PayloadOverBudget);
    }
    SECTION("consumer missing from the manifest") {
        auto m = good;
        m.entries.pop_back();
        CHECK(admit(m, observed).error == AdmissionError::UnknownConsumer);
    }
    SECTION("manifest names a consumer the graph lacks") {
        auto m = good;
        m.entries.push_back(m.entries[1]);
        m.entries.back().consumer = "convolution.second";
        CHECK(admit(m, observed).error == AdmissionError::MissingConsumer);
    }
}

TEST_CASE("a consumer without a CPU fallback is refused and goes silent on device loss",
          "[gpu_audio][graph][resource-admission][p4]") {
    const GeometryFixture fixture{"sr48k-b64-nofallback", 48000.0, 64, {64}};
    GraphRig rig(NeuralOptions{.cpu_shadow = false});
    require_gpu_host(rig, fixture);
    const auto manifest = reference_manifest(*rig.neural, *rig.convolution);
    const auto verdict = admit(manifest, rig.observe(fixture.sample_rate));
    CHECK(verdict.error == AdmissionError::FallbackMissing);
    CHECK(verdict.consumer == "neural.wavenet");

    // What admission prevents: the same loss now produces silence and the
    // output-tolerance control fails.
    rig.render(64 * 60, fixture.pattern);
    rig.neural->inject_device_loss();
    rig.render(64 * 60, fixture.pattern);
    // The transport selects its fallback slot, which emits zeros without a
    // shadow, so silence is measured on the audio rather than on the counter.
    CHECK_FALSE(rig.neural->transport().capability_report().fallback_available);
    CHECK(silent_quanta(rig.output, static_cast<std::size_t>(rig.graph.latency_samples()), 64) > 0);
    const auto ref = oracle(rig.input, make_ir());
    CHECK(max_error_at(rig.output, ref, rig.graph.latency_samples()) >
          manifest.entries[0].output_tolerance_abs);
}

TEST_CASE("graph admission controls can fail: misalignment and a planted allocation",
          "[gpu_audio][graph][resource-admission][p4]") {
    const GeometryFixture fixture{"sr48k-b64", 48000.0, 64, {64}};
    GraphRig rig;
    require_gpu_host(rig, fixture);
    rig.render(64 * 80, fixture.pattern);
    const auto declared = rig.graph.latency_samples();
    const auto ref = oracle(rig.input, make_ir());
    const double tolerance = 1.0e-4;
    // Positive control, then a one-sample misalignment must break tolerance.
    CHECK(max_error_at(rig.output, ref, declared) <= tolerance);
    CHECK(max_error_at(rig.output, ref, declared + 1) > tolerance);
    CHECK(max_error_at(rig.output, ref, declared - 1) > tolerance);

    // The allocation probe sees an allocation in this binary.
    std::size_t planted = 0;
    {
        pulp::test::RtAllocationProbe probe;
        g_allocation_sink = std::make_unique<std::vector<float>>(16);
        planted = probe.allocation_count();
    }
    g_allocation_sink.reset();
    CHECK(planted > 0);
}

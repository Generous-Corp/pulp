#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_program.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include <cstdint>

using namespace pulp::gpu_audio;
using pulp::audio::BufferView;

namespace {

using CapError = GpuAudioCapabilityError;

CapError capability_error(const GpuAudioCapabilityReport& capability) {
    return validate_gpu_audio_capability(capability).error;
}

// The shapes the shipped producers emit. Each mirrors one branch of
// capability_from() in gpu_audio_transport.cpp or of
// GpuNeuralRealtimeNode::capability_report().
GpuAudioCapabilityReport transport_staged() {
    GpuAudioCapabilityReport c;
    c.path = GpuAudioExecutionPath::Staged;
    c.eligibility = GpuAudioEligibility::Eligible;
    c.fallback_policy = MissPolicy::CpuFallback;
    c.prepared = true;
    c.fallback_available = true;
    c.diagnostics_available = true;
    c.prepared_latency_blocks = 2;
    return c;
}

GpuAudioCapabilityReport transport_shared_dawn() {
    auto c = transport_staged();
    c.path = GpuAudioExecutionPath::SharedMemory;
    c.provider = GpuAudioProvider::Dawn;
    c.prepared_lead_blocks = 2;
    return c;
}

GpuAudioCapabilityReport transport_cpu() {
    auto c = transport_staged();
    c.path = GpuAudioExecutionPath::Cpu;
    c.provider = GpuAudioProvider::Cpu;
    // A CPU stream that is latency-aligned to a GPU one reports that delay.
    c.prepared_lead_blocks = 2;
    return c;
}

GpuAudioCapabilityReport neural_shared_unauthenticated() {
    auto c = transport_shared_dawn();
    c.provider = GpuAudioProvider::Unknown;
    c.gpu_kernel_available = true;
    return c;
}

GpuAudioProgramDescriptor shared_program() {
    return {.kind = GpuAudioProgramKind::Neural,
            .path = GpuAudioExecutionPath::SharedMemory,
            .provider = GpuAudioProvider::Dawn,
            .miss_policy = MissPolicy::CpuFallback,
            .channels = 2,
            .block_size = 32,
            .sample_rate = 48000,
            .algorithmic_lead_blocks = 2,
            .pipeline_depth = 4,
            .provider_slots = 4,
            .provider_owned_resources = true,
            .cpu_fallback_prepared = true};
}

GpuAudioProgramDescriptor staged_program() {
    auto p = shared_program();
    p.path = GpuAudioExecutionPath::Staged;
    p.provider = GpuAudioProvider::Unknown;
    p.provider_owned_resources = false;
    return p;
}

GpuAudioCapabilityReport staged_capability_for(const GpuAudioProgramDescriptor& program) {
    auto c = transport_staged();
    c.prepared_lead_blocks = program.algorithmic_lead_blocks;
    return c;
}

template <typename Enum> Enum out_of_domain() {
    return static_cast<Enum>(0x7f);
}

class RateNode final : public GpuAudioNode {
  public:
    explicit RateNode(std::uint32_t sample_rate) : sample_rate_(sample_rate) {}

    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "rate";
        d.input_channels = 1;
        d.output_channels = 1;
        d.block_size = 32;
        d.sample_rate = sample_rate_;
        d.latency_blocks = 2;
        d.miss_policy = MissPolicy::Silence;
        return d;
    }
    bool prepare() override {
        return true;
    }
    void process_block(const BufferView<const float>& in, BufferView<float>& out,
                       std::uint32_t n) override {
        for (std::uint32_t i = 0; i < n; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
    }

  private:
    std::uint32_t sample_rate_;
};

} // namespace

TEST_CASE("validate_gpu_audio_capability accepts every shipped producer shape",
          "[gpu_audio][capability]") {
    CHECK(capability_error(GpuAudioCapabilityReport{}) == CapError::None);
    CHECK(capability_error(transport_staged()) == CapError::None);
    CHECK(capability_error(transport_shared_dawn()) == CapError::None);
    CHECK(capability_error(transport_cpu()) == CapError::None);
    CHECK(capability_error(neural_shared_unauthenticated()) == CapError::None);

    // A staged node may name a provider and a lead it established itself.
    auto staged = transport_staged();
    staged.provider = GpuAudioProvider::Metal;
    staged.prepared_lead_blocks = 1;
    CHECK(capability_error(staged) == CapError::None);

    auto silent = transport_shared_dawn();
    silent.fallback_policy = MissPolicy::Silence;
    silent.fallback_available = false;
    silent.recovery_policy = GpuAudioRecoveryPolicy::AutoReacquire;
    CHECK(capability_error(silent) == CapError::None);
}

TEST_CASE("validate_gpu_audio_capability rejects each planted inconsistency",
          "[gpu_audio][capability][negative]") {
    auto c = transport_shared_dawn();
    c.path = out_of_domain<GpuAudioExecutionPath>();
    CHECK(capability_error(c) == CapError::InvalidEnumValue);
    c = transport_shared_dawn();
    c.provider = out_of_domain<GpuAudioProvider>();
    CHECK(capability_error(c) == CapError::InvalidEnumValue);
    c = transport_shared_dawn();
    c.eligibility = out_of_domain<GpuAudioEligibility>();
    CHECK(capability_error(c) == CapError::InvalidEnumValue);
    c = transport_shared_dawn();
    c.fallback_policy = out_of_domain<MissPolicy>();
    CHECK(capability_error(c) == CapError::InvalidEnumValue);
    c = transport_shared_dawn();
    c.recovery_policy = out_of_domain<GpuAudioRecoveryPolicy>();
    CHECK(capability_error(c) == CapError::InvalidEnumValue);

    c = transport_shared_dawn();
    c.path = GpuAudioExecutionPath::Unavailable;
    CHECK(capability_error(c) == CapError::PreparedPathUnavailable);

    c = transport_shared_dawn();
    c.eligibility = GpuAudioEligibility::Unavailable;
    CHECK(capability_error(c) == CapError::PreparedPathIneligible);

    c = transport_cpu();
    c.provider = GpuAudioProvider::Unknown;
    CHECK(capability_error(c) == CapError::CpuPathWithoutCpuProvider);
    c.provider = GpuAudioProvider::Dawn;
    CHECK(capability_error(c) == CapError::CpuPathWithoutCpuProvider);

    c = transport_staged();
    c.provider = GpuAudioProvider::Cpu;
    CHECK(capability_error(c) == CapError::CpuProviderOnGpuPath);
    c = transport_shared_dawn();
    c.provider = GpuAudioProvider::Cpu;
    CHECK(capability_error(c) == CapError::CpuProviderOnGpuPath);

    c = transport_cpu();
    c.gpu_kernel_available = true;
    CHECK(capability_error(c) == CapError::CpuPathWithGpuKernel);

    c = transport_shared_dawn();
    c.prepared_lead_blocks = 0;
    CHECK(capability_error(c) == CapError::SharedLeadMissing);

    c = transport_shared_dawn();
    c.fallback_available = false;
    CHECK(capability_error(c) == CapError::FallbackUnavailable);
}

TEST_CASE("validate_gpu_audio_capability requires the default unprepared snapshot",
          "[gpu_audio][capability][negative]") {
    const auto unprepared = [](auto mutate) {
        GpuAudioCapabilityReport c;
        mutate(c);
        return capability_error(c);
    };
    constexpr auto kError = CapError::UnpreparedStateHasPreparation;
    CHECK(unprepared([](auto& c) { c.path = GpuAudioExecutionPath::Staged; }) == kError);
    CHECK(unprepared([](auto& c) { c.provider = GpuAudioProvider::Dawn; }) == kError);
    CHECK(unprepared([](auto& c) { c.eligibility = GpuAudioEligibility::Eligible; }) == kError);
    CHECK(unprepared([](auto& c) { c.fallback_policy = MissPolicy::PassthroughDry; }) == kError);
    CHECK(unprepared([](auto& c) { c.prepared_lead_blocks = 1; }) == kError);
    CHECK(unprepared([](auto& c) { c.fallback_available = true; }) == kError);
    CHECK(unprepared([](auto& c) { c.diagnostics_available = true; }) == kError);
    CHECK(unprepared([](auto& c) { c.prepared_latency_blocks = 1; }) == kError);
    CHECK(unprepared([](auto& c) { c.gpu_kernel_available = true; }) == kError);
    CHECK(unprepared([](auto& c) { c.recovery_policy = GpuAudioRecoveryPolicy::AutoReacquire; }) ==
          kError);
}

TEST_CASE("validate_gpu_audio_program rejects descriptor enums outside their domain",
          "[gpu_audio][program][negative]") {
    CHECK(validate_gpu_audio_program(shared_program()).accepted());

    auto program = shared_program();
    program.kind = static_cast<GpuAudioProgramKind>(4);
    CHECK(validate_gpu_audio_program(program).error == GpuAudioProgramError::InvalidEnumValue);
    program.kind = GpuAudioProgramKind::Custom;
    CHECK(validate_gpu_audio_program(program).accepted());

    program = shared_program();
    program.path = out_of_domain<GpuAudioExecutionPath>();
    CHECK(validate_gpu_audio_program(program).error == GpuAudioProgramError::InvalidEnumValue);

    program = shared_program();
    program.provider = out_of_domain<GpuAudioProvider>();
    CHECK(validate_gpu_audio_program(program).error == GpuAudioProgramError::InvalidEnumValue);

    program = shared_program();
    program.miss_policy = out_of_domain<MissPolicy>();
    CHECK(validate_gpu_audio_program(program).error == GpuAudioProgramError::InvalidEnumValue);

    // A GPU path cannot name the CPU provider; the CPU path keeps its
    // established ProviderOnCpu rule for any named provider.
    for (const auto path : {GpuAudioExecutionPath::Staged, GpuAudioExecutionPath::SharedMemory}) {
        program = path == GpuAudioExecutionPath::Staged ? staged_program() : shared_program();
        CHECK(validate_gpu_audio_program(program).accepted());
        program.provider = GpuAudioProvider::Cpu;
        CHECK(validate_gpu_audio_program(program).error ==
              GpuAudioProgramError::CpuProviderOnGpuPath);
    }
    program = shared_program();
    program.miss_policy = out_of_domain<MissPolicy>();

    // The two-argument overload reports the descriptor error first.
    CHECK(validate_gpu_audio_program(program, transport_shared_dawn()).error ==
          GpuAudioProgramError::InvalidEnumValue);
}

TEST_CASE("validate_gpu_audio_program rejects a capability that contradicts itself",
          "[gpu_audio][program][capability][negative]") {
    const auto program = staged_program();
    auto capability = staged_capability_for(program);
    CHECK(validate_gpu_audio_program(program, capability).accepted());
    CHECK(validate_gpu_audio_program(shared_program(), transport_shared_dawn()).accepted());

    // Each report below agrees with the descriptor on every compared field.
    capability.provider = out_of_domain<GpuAudioProvider>();
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityInvalid);

    capability = staged_capability_for(program);
    capability.provider = GpuAudioProvider::Cpu;
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityInvalid);

    capability = staged_capability_for(program);
    capability.recovery_policy = out_of_domain<GpuAudioRecoveryPolicy>();
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityInvalid);

    auto shared = transport_shared_dawn();
    shared.recovery_policy = out_of_domain<GpuAudioRecoveryPolicy>();
    CHECK(validate_gpu_audio_program(shared_program(), shared).error ==
          GpuAudioProgramError::CapabilityInvalid);

    // Established relational errors keep their own codes.
    capability = staged_capability_for(program);
    capability.eligibility = GpuAudioEligibility::Unavailable;
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityIneligible);
    capability = staged_capability_for(program);
    capability.fallback_available = false;
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityFallbackUnavailable);
}

TEST_CASE("GpuAudioTransport refuses a node with no sample rate",
          "[gpu_audio][transport][negative]") {
    GpuAudioTransport transport;

    RateNode zero_rate(0);
    CHECK_FALSE(transport.prepare(&zero_rate, {}));
    CHECK_FALSE(transport.capability_report().prepared);
    CHECK(capability_error(transport.capability_report()) == CapError::None);
    const auto refused = transport.status_snapshot();
    CHECK(refused.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK(validate_gpu_audio_status(refused).accepted());

    // Positive control: the same node with a rate prepares, and what the
    // transport then reports passes both validators.
    RateNode rated(48000);
    REQUIRE(transport.prepare(&rated, {}));
    const auto capability = transport.capability_report();
    CHECK(capability.prepared);
    CHECK(capability.path == GpuAudioExecutionPath::Staged);
    CHECK(capability_error(capability) == CapError::None);
    const auto status = transport.status_snapshot();
    CHECK(status.sample_rate == 48000);
    CHECK(validate_gpu_audio_status(status).accepted());

    // Preparing a zero-rate node over a prepared one releases the old one and
    // does not leave a prepared shape behind.
    CHECK_FALSE(transport.prepare(&zero_rate, {}));
    CHECK_FALSE(transport.capability_report().prepared);
    transport.release();
}

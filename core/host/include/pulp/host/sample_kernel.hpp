#pragma once

// Fixed scalar-kernel ABI used by sample-region planning. This stays separate
// from CustomNodeType so existing aggregate initializers and block callbacks do
// not change when a type opts into scalar execution.

#include <cstddef>
#include <cstdint>
#include <string>

namespace pulp::host {

static_assert(sizeof(float) == 4 && alignof(float) == 4,
              "sample-kernel ABI requires 32-bit, 4-byte-aligned float");

enum class SampleKernelCausality : std::uint8_t {
    Combinational = 0,
    OneSampleDelay = 1,
};

enum class SampleKernelScope : std::uint8_t {
    RegionOnly = 0,
};

enum class SampleKernelConfigKind : std::uint8_t {
    Invalid = 0,
    None = 1,
    BoundaryIndex = 2,
    FiniteConstant = 3,
    PromotedParameterId = 4,
};

struct SampleKernelConfig {
    SampleKernelConfigKind kind = SampleKernelConfigKind::Invalid;
    std::uint32_t boundary_index_or_parameter_id = 0;
    float constant = 0.0f;
};

enum class PreparedSampleKernelConfigKind : std::uint8_t {
    Invalid = 0,
    None = 1,
    BoundaryIndex = 2,
    FiniteConstant = 3,
    PromotedParameterIndex = 4,
};

struct PreparedSampleKernelConfig {
    PreparedSampleKernelConfigKind kind = PreparedSampleKernelConfigKind::Invalid;
    std::uint32_t boundary_or_parameter_index = 0;
    float constant = 0.0f;
};

struct SampleKernelPrepareContext {
    double sample_rate = 0.0;
    std::uint32_t max_block_size = 0;
    PreparedSampleKernelConfig config;
};

struct SampleFrameContext {
    const float* promoted_values = nullptr;
    std::uint32_t promoted_value_count = 0;
    std::uint32_t sample_offset = 0;
};

using SampleKernelConstructFn = bool (*)(void* state,
                                         const SampleKernelPrepareContext& context) noexcept;
using SampleKernelResetFn = void (*)(void* state) noexcept;
using SampleKernelDestroyFn = void (*)(void* state) noexcept;
using SampleKernelProcessFn = void (*)(void* state, const PreparedSampleKernelConfig& config,
                                       const SampleFrameContext& frame, const float* inputs,
                                       float* outputs) noexcept;
using SampleDelayPublishFn = void (*)(const void* state, const PreparedSampleKernelConfig& config,
                                      float* outputs) noexcept;
using SampleDelayCommitFn = void (*)(void* state, const PreparedSampleKernelConfig& config,
                                     const float* inputs) noexcept;

struct SampleKernelMetadata {
    std::string category;
    std::string parameter;
    std::string units;
    float minimum_value = 0.0f;
    float maximum_value = 0.0f;
    bool has_value_range = false;
    std::uint64_t capability_flags = 0;
};

struct SampleKernelDescriptor {
    static constexpr std::uint32_t kAbiVersion = 1;

    std::uint32_t abi_version = kAbiVersion;
    std::string type_id;
    int version = 1;
    std::uint32_t num_input_ports = 0;
    std::uint32_t num_output_ports = 0;
    SampleKernelCausality causality = SampleKernelCausality::Combinational;
    SampleKernelScope scope = SampleKernelScope::RegionOnly;
    SampleKernelConfigKind authored_config_kind = SampleKernelConfigKind::Invalid;
    std::uint32_t state_size = 0;
    std::uint32_t state_alignment = 1;
    SampleKernelConstructFn construct = nullptr;
    SampleKernelResetFn reset = nullptr;
    SampleKernelDestroyFn destroy = nullptr;
    SampleKernelProcessFn process = nullptr;
    SampleDelayPublishFn delay_publish = nullptr;
    SampleDelayCommitFn delay_commit = nullptr;
    std::uint32_t latency_samples = 0;
    SampleKernelMetadata metadata;

    bool is_valid_registration() const noexcept;
};

// Validates the runtime non-aliasing precondition before invoking a scalar
// callback. Zero-sized ranges may be null; every non-zero range must be present.
bool sample_kernel_storage_is_disjoint(const SampleKernelDescriptor& descriptor, const void* state,
                                       const PreparedSampleKernelConfig& config,
                                       const SampleFrameContext& frame, const float* inputs,
                                       float* outputs) noexcept;

} // namespace pulp::host

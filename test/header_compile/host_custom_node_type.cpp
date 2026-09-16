#include <pulp/host/custom_node_type.hpp>

#include <type_traits>

static_assert(std::is_default_constructible_v<pulp::host::CustomNodeType>);
static_assert(std::is_copy_constructible_v<pulp::host::CustomNodeTypeMetadata>);
static_assert(std::is_copy_assignable_v<pulp::host::CustomNodeTypeMetadata>);
static_assert(std::is_default_constructible_v<pulp::host::SampleKernelDescriptor>);
static_assert(
    std::is_same_v<pulp::host::SampleKernelProcessFn,
                   void (*)(void*, const pulp::host::PreparedSampleKernelConfig&,
                            const pulp::host::SampleFrameContext&, const float*, float*) noexcept>);

// Preserve the historical positional prefix. Metadata discovery is a separate
// type so adding it cannot shift aggregate registrar fields.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
[[maybe_unused]] pulp::host::CustomNodeType positional_custom_node_type{
    "pulp.test.positional", 1, 1, 1, "Positional", {}};
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

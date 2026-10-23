#include <pulp/host/processor_signal_graph_binding.hpp>

#include <type_traits>

static_assert(!std::is_copy_constructible_v<pulp::host::ProcessorSignalGraphBinding>);
static_assert(!std::is_copy_assignable_v<pulp::host::ProcessorSignalGraphBinding>);
static_assert(std::is_default_constructible_v<pulp::host::ProcessorSignalGraphBinding::Lease>);

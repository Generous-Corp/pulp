#include <pulp/host/signal_graph_authoring.hpp>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<pulp::host::GraphAuthoringReceipt>);
static_assert(std::is_standard_layout_v<pulp::host::GraphAuthoringReceipt>);
static_assert(
    std::is_same_v<std::underlying_type_t<pulp::host::GraphAuthoringReceiptStatus>, std::uint8_t>);

bool pulp_host_authoring_receipt_contract() {
    const pulp::host::GraphAuthoringReceipt empty;
    const pulp::host::GraphAuthoringReceipt current{1, 2};
    return !empty.valid() && current.valid() && current == pulp::host::GraphAuthoringReceipt{1, 2};
}

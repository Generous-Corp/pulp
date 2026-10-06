#include <pulp/host/processor_signal_graph_binding.hpp>

#include <pulp/format/processor.hpp>

#include <mutex>
#include <new>
#include <unordered_map>

namespace pulp::host {
namespace {
std::mutex& binding_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<const format::Processor*, ProcessorSignalGraphBinding*>& bindings() {
    static std::unordered_map<const format::Processor*, ProcessorSignalGraphBinding*> table;
    return table;
}
} // namespace

ProcessorSignalGraphBinding::ProcessorSignalGraphBinding(format::Processor& processor,
                                                         SignalGraph& graph, double sample_rate,
                                                         int maximum_block_size) noexcept
    : processor_(processor), authority_(graph, sample_rate, maximum_block_size) {}

ProcessorSignalGraphBinding::~ProcessorSignalGraphBinding() {
    lifetime_.retire();
    std::lock_guard<std::mutex> lock(binding_mutex());
    const auto it = bindings().find(&processor_);
    if (it != bindings().end() && it->second == this)
        bindings().erase(it);
}

std::unique_ptr<ProcessorSignalGraphBinding>
ProcessorSignalGraphBinding::install(format::Processor& processor, SignalGraph& graph,
                                     double sample_rate, int maximum_block_size) noexcept {
    auto binding = std::unique_ptr<ProcessorSignalGraphBinding>(
        new (std::nothrow)
            ProcessorSignalGraphBinding(processor, graph, sample_rate, maximum_block_size));
    if (!binding)
        return nullptr;

    std::lock_guard<std::mutex> lock(binding_mutex());
    if (!bindings().emplace(&processor, binding.get()).second)
        return nullptr;
    return binding;
}

SignalGraphControlAuthority*
create_processor_signal_graph_authority(format::Processor& processor) noexcept {
    std::lock_guard<std::mutex> lock(binding_mutex());
    const auto it = bindings().find(&processor);
    return it == bindings().end() ? nullptr : &it->second->authority();
}

ProcessorSignalGraphBinding::Lease
create_processor_signal_graph_authority_lease(format::Processor& processor) noexcept {
    std::lock_guard<std::mutex> lock(binding_mutex());
    const auto it = bindings().find(&processor);
    return it == bindings().end() ? ProcessorSignalGraphBinding::Lease{} : it->second->lease();
}

} // namespace pulp::host

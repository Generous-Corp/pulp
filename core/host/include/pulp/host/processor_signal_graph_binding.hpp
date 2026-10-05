#pragma once

#include <pulp/host/signal_graph_control.hpp>
#include <pulp/runtime/alive_token.hpp>

#include <memory>

namespace pulp::format {
class Processor;
}

namespace pulp::host {

/// Lifetime-bound access from a Processor instance to the SignalGraph it owns.
///
/// This is a host-side side table, so adding the binding does not change the
/// public Processor ABI. The returned token must be owned by the Processor (or
/// by an object whose lifetime is strictly inside the Processor lifetime), and
/// it must be created after the graph is prepared and destroyed before the
/// graph is released. A second binding for the same Processor is refused.
class ProcessorSignalGraphBinding final {
  public:
    static std::unique_ptr<ProcessorSignalGraphBinding> install(format::Processor& processor,
                                                                SignalGraph& graph,
                                                                double sample_rate,
                                                                int maximum_block_size) noexcept;

    ~ProcessorSignalGraphBinding();
    ProcessorSignalGraphBinding(const ProcessorSignalGraphBinding&) = delete;
    ProcessorSignalGraphBinding& operator=(const ProcessorSignalGraphBinding&) = delete;

    SignalGraphControlAuthority& authority() noexcept {
        return authority_;
    }
    const SignalGraphControlAuthority& authority() const noexcept {
        return authority_;
    }

    struct Lease {
        SignalGraphControlAuthority* authority = nullptr;
        runtime::AliveToken::Handle alive;

        bool valid() const noexcept {
            return authority != nullptr && runtime::AliveToken::is_alive(alive);
        }
    };
    Lease lease() noexcept {
        return {.authority = &authority_, .alive = lifetime_.capture()};
    }

  private:
    ProcessorSignalGraphBinding(format::Processor& processor, SignalGraph& graph,
                                double sample_rate, int maximum_block_size) noexcept;

    format::Processor& processor_;
    runtime::AliveToken lifetime_;
    SignalGraphControlAuthority authority_;
};

/// Resolver used by the canonical standalone control host. It returns the
/// authority registered for this exact Processor instance, or nullptr when the
/// product has not installed a binding or has released it.
SignalGraphControlAuthority*
create_processor_signal_graph_authority(format::Processor& processor) noexcept;
ProcessorSignalGraphBinding::Lease
create_processor_signal_graph_authority_lease(format::Processor& processor) noexcept;

} // namespace pulp::host

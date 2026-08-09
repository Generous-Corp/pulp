#pragma once

#include <memory>

namespace pulp::format {
class Processor;
namespace detail {
class StandaloneTestInputHost;
}
}

namespace pulp::state {
class StateStore;
}

namespace pulp::format {

/// Optional host-side control composition for one ordinary StandaloneApp.
///
/// The base standalone archive owns only this lifecycle boundary and remains
/// independent of every control protocol and transport type. An explicitly
/// control-enabled executable installs one factory from its generated target
/// source; production-stripped executables install nothing.
class StandaloneControlHost {
  public:
    virtual ~StandaloneControlHost() = default;

    /// Called after the processor has registered its complete parameter
    /// catalog and the Standalone app has otherwise started successfully. A
    /// direct user launch with no broker bootstrap remains an inert launch.
    virtual bool start(Processor& processor, state::StateStore& store,
                       detail::StandaloneTestInputHost* test_input = nullptr,
                       double sample_rate = 0.0) = 0;

    /// Revoke host-side authority before the processor or StateStore retires.
    virtual void stop() noexcept = 0;

    /// Run bounded host-main work admitted by the broker. The Standalone event
    /// loop and the dedicated companion both call this on their owning main
    /// thread; an inert direct launch has no queued work.
    virtual void poll() noexcept {}
};

using StandaloneControlHostFactory = std::unique_ptr<StandaloneControlHost> (*)();

struct StandaloneControlHostCreation {
    bool factory_installed = false;
    std::unique_ptr<StandaloneControlHost> host;
};

namespace detail {

/// Installs the single generated factory for this executable. Repeated or
/// empty installation fails closed.
bool install_standalone_control_host_factory(StandaloneControlHostFactory factory) noexcept;

/// Creates the explicitly installed host. The separate installation bit lets
/// callers distinguish a production-stripped executable from a control-enabled
/// executable whose factory failed, which must fail closed.
StandaloneControlHostCreation create_standalone_control_host();

} // namespace detail
} // namespace pulp::format

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

namespace pulp::view {
class View;
class WindowHost;
}

namespace pulp::format {

class ViewBridge;

enum class StandaloneControlUiMode {
    Headless,
    DeferToEditor,
};

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
                       double sample_rate = 0.0,
                       StandaloneControlUiMode ui_mode =
                           StandaloneControlUiMode::Headless) = 0;

    /// Complete a deferred UI-capable startup against the editor that the
    /// Standalone is actually presenting. The control host borrows all three
    /// objects until stop(); it must never create a second processor view.
    virtual bool attach_editor(ViewBridge&, view::View&, view::WindowHost&) {
        return true;
    }

    /// Revoke host-side authority before the processor or StateStore retires.
    virtual void stop() noexcept = 0;

    /// Whether the host can still serve the broker connection. Inert direct
    /// launches remain ready; a broker-launched companion becomes unready as
    /// soon as its authenticated carrier closes.
    virtual bool ready() const noexcept { return true; }

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

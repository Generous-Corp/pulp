#pragma once

#include <pulp/inspect/control_execution.hpp>

#include <memory>

namespace pulp::inspect {

/// One-time late binding for a host operation executor.
///
/// The callback returned by executor() is safe to give to ControlHostConnection
/// before host enrollment has minted its registration. Until install() succeeds
/// it fails immediately with HostUnavailable. close() permanently disables the
/// slot; later calls fail immediately with Cancelled. An executor can never be
/// replaced once installed.
class ControlOperationExecutorSlot {
  public:
    ControlOperationExecutorSlot();
    ~ControlOperationExecutorSlot();

    ControlOperationExecutorSlot(const ControlOperationExecutorSlot&) = delete;
    ControlOperationExecutorSlot& operator=(const ControlOperationExecutorSlot&) = delete;
    ControlOperationExecutorSlot(ControlOperationExecutorSlot&& other) noexcept;
    ControlOperationExecutorSlot& operator=(ControlOperationExecutorSlot&& other) noexcept;

    /// Returns a lifetime-safe facade over this slot. Existing facades observe
    /// later install() and close() transitions.
    ControlOperationExecutor executor() const;

    /// Installs the only executor accepted by this slot. Returns false for an
    /// empty executor, a repeated install, or a closed/moved-from slot.
    [[nodiscard]] bool install(ControlOperationExecutor executor);

    /// Permanently closes the slot. Already-started calls may finish through
    /// the executor they acquired before close(); new calls fail immediately.
    void close() noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulp::inspect

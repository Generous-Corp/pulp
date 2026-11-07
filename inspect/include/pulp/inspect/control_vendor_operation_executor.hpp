#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <pulp/inspect/control_vendor_operations.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

/// One admitted call of a vendor operation. The broker has already checked
/// the grant, consent, lease and tier, and validated `arguments_json` against
/// the declared input schema; the handler must still treat argument values as
/// untrusted data.
struct ControlVendorOperationCall {
    std::string_view operation_id;
    std::uint32_t operation_version = 1;
    ControlVendorTier tier = ControlVendorTier::Read;
    std::string_view arguments_json;
    /// The broker's durable receipt for this call.
    std::string_view receipt_id;
    /// Returns false once the call was cancelled, revoked or ran out of time.
    /// Long handlers check it between steps and stop without applying more.
    std::function<bool()> still_authorized;
};

struct ControlVendorOperationResult {
    bool ok = false;
    /// On success, a JSON object that satisfies the declared output schema.
    /// The broker rejects any other result before the receipt can complete.
    std::string result_json = "{}";
    ControlResultCode error_code = ControlResultCode::InvalidRequest;
    std::string explanation;

    static ControlVendorOperationResult success(std::string result_json) {
        return {.ok = true, .result_json = std::move(result_json)};
    }
    static ControlVendorOperationResult failure(ControlResultCode code, std::string explanation) {
        return {.ok = false, .error_code = code, .explanation = std::move(explanation)};
    }
};

using ControlVendorOperationHandler =
    std::function<ControlVendorOperationResult(const ControlVendorOperationCall&)>;

/// Code bound to vendor operation ids. Binding a handler adds no authority:
/// the broker admits only operations the target's build-time declaration
/// contains, and an admitted operation without a handler fails as not
/// implemented.
class ControlVendorOperationHandlers {
  public:
    /// Returns false for a malformed vendor id, an empty handler, or a second
    /// handler for the same id.
    bool add(std::string operation_id, ControlVendorOperationHandler handler);
    const ControlVendorOperationHandler* find(std::string_view operation_id) const;
    std::vector<std::string> operation_ids() const;
    bool empty() const {
        return handlers_.empty();
    }
    std::size_t size() const {
        return handlers_.size();
    }

  private:
    std::map<std::string, ControlVendorOperationHandler, std::less<>> handlers_;
};

/// Adapts vendor handlers onto the canonical executor contract. Compose it on
/// the thread the handlers need (the Standalone host fences it onto the main
/// thread). A handler exception becomes an internal failure, never a crash.
ControlOperationExecutor make_control_vendor_operation_executor(
    std::shared_ptr<const ControlVendorOperationHandlers> handlers);

} // namespace pulp::inspect

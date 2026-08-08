#pragma once

#include <pulp/inspect/control_operations.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect::detail {

std::string idempotency_scope_hash(const ControlOperationBinding& binding);
std::string request_scope_hash(const ControlOperationBinding& binding);
std::string binding_hash(const ControlOperationBinding& binding);
std::string idempotency_content_hash(const ControlOperationBinding& binding);
bool valid_binding(const ControlOperationBinding& binding);
bool valid_store_transition(ControlReceiptState from, ControlReceiptState to);
bool valid_receipt_id(std::string_view value);
std::optional<std::string> random_receipt_id();
std::int64_t unix_milliseconds(std::chrono::system_clock::time_point time);
std::string serialize_receipt(const ControlOperationReceipt& receipt);
std::optional<ControlOperationReceipt> parse_receipt(std::string_view contents,
                                                     std::size_t maximum_bytes);
bool normalize_transition_result(ControlReceiptState next, ControlOperationResult& result);
bool elapsed_at_least(std::int64_t now, std::int64_t since, std::chrono::milliseconds duration);

} // namespace pulp::inspect::detail

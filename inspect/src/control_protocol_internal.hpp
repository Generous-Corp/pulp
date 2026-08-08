#pragma once

#include <pulp/inspect/control_protocol.hpp>

#include <choc/text/choc_JSON.h>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect::control_protocol_detail {

inline constexpr std::size_t kMaximumIdBytes = 128;
inline constexpr std::size_t kMaximumOperationIdBytes = 256;
inline constexpr std::size_t kMaximumFeatureBytes = 64;
inline constexpr std::size_t kMaximumFeatures = 32;
inline constexpr std::size_t kMaximumExplanationBytes = kControlReceiptMaximumExplanationBytes;
inline constexpr std::size_t kMaximumPayloadBytes = kControlMaximumRequestPayloadBytes;
inline constexpr std::size_t kMaximumProgressBytes = 32u * 1024u;
inline constexpr std::size_t kMaximumArtifacts = kControlReceiptMaximumArtifacts;
inline constexpr std::size_t kMaximumJsonDepth = 32;
inline constexpr std::size_t kMaximumJsonNodes = 8192;
inline constexpr std::size_t kMaximumProgressJsonNodes = 4096;
inline constexpr std::size_t kMaximumResultJsonNodes = 32u * 1024u;

using ValueView = choc::value::ValueView;

bool valid_control_json_bytes(std::string_view bytes, std::size_t maximum_bytes,
                              std::size_t maximum_nodes = kMaximumJsonNodes);
bool valid_utf8(std::string_view bytes);
bool bounded_json_shape(ValueView value, std::size_t depth, std::size_t& remaining_nodes);
choc::value::Value canonical_value(ValueView value);
std::optional<choc::value::Value>
parse_bounded_control_json(std::string_view json, std::size_t maximum_bytes,
                           std::size_t maximum_nodes = kMaximumJsonNodes);
std::optional<std::string> canonicalize_control_result_json(std::string_view json);

bool valid_token(std::string_view value, std::size_t maximum);
bool valid_text(std::string_view value, std::size_t maximum);
bool valid_hash(std::string_view value);
bool valid_features(const std::vector<std::string>& features);
bool valid_offer(const ControlNegotiationOffer& offer);
bool valid_request(const ControlRequestEnvelope& request, bool require_hash);
bool valid_progress(const ControlProgressEnvelope& progress);
bool valid_receipt(const ControlReceiptEnvelope& receipt);
bool valid_host_open(const ControlHostOpenEnvelope& message);
bool valid_host_open_result(const ControlHostOpenResult& message);
bool valid_host_execute(const ControlHostExecuteEnvelope& message);
bool valid_host_progress(const ControlHostProgressEnvelope& message);
bool valid_host_cancel(const ControlHostCancelEnvelope& message);
bool valid_host_complete(const ControlHostCompleteEnvelope& message);
bool is_host_control_kind(std::string_view kind);
std::optional<ControlEnvelopePayload>
decode_host_control_payload(std::string_view kind, ValueView payload,
                            ControlProtocolDiagnostics& diagnostics);
bool is_host_preflight_kind(std::string_view kind);
std::optional<ControlEnvelopePayload>
decode_host_preflight_payload(std::string_view kind, ValueView payload,
                              ControlProtocolDiagnostics& diagnostics);

bool only_fields(ValueView value, std::initializer_list<std::string_view> allowed,
                 ControlProtocolDiagnostics& diagnostics);
bool required_string(ValueView value, std::string_view name, std::string& out, std::size_t maximum,
                     ControlProtocolDiagnostics& diagnostics, bool token = true);
bool required_u32(ValueView value, std::string_view name, std::uint32_t& out,
                  ControlProtocolDiagnostics& diagnostics, bool nonzero = true);
bool required_u64(ValueView value, std::string_view name, std::uint64_t& out,
                  ControlProtocolDiagnostics& diagnostics);
bool required_i64(ValueView value, std::string_view name, std::int64_t& out,
                  ControlProtocolDiagnostics& diagnostics);
bool required_bool(ValueView value, std::string_view name, bool& out,
                   ControlProtocolDiagnostics& diagnostics);
bool parse_features(ValueView value, std::string_view name, std::vector<std::string>& out,
                    ControlProtocolDiagnostics& diagnostics);
choc::value::Value string_array(const std::vector<std::string>& strings);

} // namespace pulp::inspect::control_protocol_detail

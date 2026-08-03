// protocol.hpp — Inspector protocol: message types and JSON encoding
// Modeled on Chrome DevTools Protocol with domain.method naming.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::inspect {

/// Bounded transfer ceiling used by clients and by hosts that expose image
/// capture. Ordinary inspector servers may retain their smaller 1 MiB default.
inline constexpr std::size_t kInspectorExtendedMessageBytes =
    16u * 1024u * 1024u;

/// A single inspector protocol message (request, response, or event).
struct InspectorMessage {
    int64_t id = 0;             ///< Non-zero for request/response pairs. Zero for events.
    std::string method;         ///< "Domain.method" — e.g. "DOM.getDocument"
    std::string params_json;    ///< JSON object string (params for request, result for response)
    bool is_error = false;      ///< True if this is an error response
    std::string error_code;     ///< Stable machine-readable code for error responses
    std::string error_data_json; ///< Optional structured error details
};

// ── Encode / Decode ─────────────────────────────────────────────────────

/// Serialize a message to a JSON string.
std::string encode_message(const InspectorMessage& msg);

/// Parse a JSON string into a message. Returns false on parse failure.
bool decode_message(const std::string& json, InspectorMessage& out);

// ── Factory helpers ─────────────────────────────────────────────────────

InspectorMessage make_request(int64_t id, std::string method, std::string params_json = "{}");
InspectorMessage make_response(int64_t id, std::string result_json);
InspectorMessage make_error(int64_t id, std::string error_message,
                            std::string error_code = "internal_error",
                            std::string error_data_json = "{}");
InspectorMessage make_event(std::string method, std::string params_json = "{}");

// ── Method name constants ───────────────────────────────────────────────

namespace methods {
#define PULP_INSPECT_METHOD(symbol, wire_name, capability, kind) \
    inline constexpr auto symbol = wire_name;
#include <pulp/inspect/protocol_methods.inc>
#undef PULP_INSPECT_METHOD
}

} // namespace pulp::inspect

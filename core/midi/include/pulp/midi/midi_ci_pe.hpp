#pragma once

// MIDI-CI Property Exchange (PE) — multipart Get/Set messaging plus
// Subscribe/Notify management on top of the discovery layer.
//
// The PE wire format wraps every Get/Set/Subscribe Inquiry and Reply in
// SysEx envelopes whose body looks like:
//
//   request_id        : 1 byte (1..0x7F; 0 reserved)
//   header_length     : 2 bytes 7-bit LE
//   header_data       : N bytes Mcoded7-encoded JSON UTF-8
//   total_chunks      : 2 bytes 7-bit LE
//   chunk_number      : 2 bytes 7-bit LE  (1-indexed)
//   payload_length    : 2 bytes 7-bit LE
//   payload_data      : N bytes Mcoded7-encoded resource bytes
//
// Pulp implements the framing + Mcoded7 + JSON-header construction here.
// zlib payload encoding (MIDI-CI v1.2 §6.3) is out-of-scope for this slice
// and is tracked in the follow-up note in the spec doc.
//
// This file is intentionally header-only-ish: it owns no I/O. The caller
// drives SysEx in/out around it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <pulp/midi/midi_ci.hpp>

namespace pulp::midi {

/// PE sub-IDs (MIDI-CI sub-ID #2 byte). Mirrors `CiMessageType` for PE.
enum class PeMessageType : uint8_t {
    GetInquiry      = 0x34,
    GetReply        = 0x35,
    SetInquiry      = 0x36,
    SetReply        = 0x37,
    SubscribeInquiry = 0x38,
    SubscribeReply   = 0x39,
    Notify           = 0x3F,
};

/// A single PE chunk, after Mcoded7 decode of header + payload.
/// Caller-owned UTF-8 strings; decoded JSON header is a string the caller
/// can parse with `choc::json` (already vendored).
struct PeChunk {
    uint8_t request_id = 0;
    uint16_t total_chunks = 0;
    uint16_t chunk_number = 0;       ///< 1-indexed
    std::string header_json;         ///< Decoded JSON UTF-8
    std::vector<uint8_t> payload;    ///< Decoded resource bytes
};

/// Builder for PE message wire bytes. `pe_type` is one of the PE
/// `CiMessageType` sub-IDs. `header_json` is plain UTF-8 JSON — the
/// caller owns its schema (resource/, command/, status/, etc.).
///
/// Returns a complete SysEx envelope: F0 7E 7F 0D <sub-id> version
/// src(4) dst(4) request_id hdr_len(2) hdr(Mcoded7) total(2)
/// chunk_num(2) pay_len(2) pay(Mcoded7) F7.
std::vector<uint8_t> pe_build_message(PeMessageType pe_type,
                                      uint8_t ci_version,
                                      MUID source,
                                      MUID destination,
                                      uint8_t request_id,
                                      std::string_view header_json,
                                      uint16_t total_chunks,
                                      uint16_t chunk_number,
                                      const uint8_t* payload,
                                      std::size_t payload_size);

inline std::vector<uint8_t> pe_build_message(PeMessageType pe_type,
                                             uint8_t ci_version,
                                             MUID source,
                                             MUID destination,
                                             uint8_t request_id,
                                             std::string_view header_json,
                                             uint16_t total_chunks,
                                             uint16_t chunk_number,
                                             const std::vector<uint8_t>& payload) {
    return pe_build_message(pe_type, ci_version, source, destination,
                            request_id, header_json, total_chunks,
                            chunk_number, payload.data(), payload.size());
}

/// Parse a single PE SysEx envelope into a `PeChunk`. Returns `nullopt` if
/// the buffer is not a well-formed PE message or fails Mcoded7 decode.
/// Caller is responsible for matching `request_id` and reassembling chunks.
std::optional<PeChunk> pe_parse_message(const uint8_t* data, std::size_t size);

/// Split a logical PE Get/Set into wire-ready chunks bounded by
/// `max_payload_bytes` of decoded resource per chunk. The header is
/// repeated verbatim in each chunk per the spec.
std::vector<std::vector<uint8_t>>
pe_split_into_chunks(PeMessageType pe_type,
                     uint8_t ci_version,
                     MUID source,
                     MUID destination,
                     uint8_t request_id,
                     std::string_view header_json,
                     const uint8_t* payload,
                     std::size_t payload_size,
                     std::size_t max_payload_bytes);

/// Reassemble a stream of incoming `PeChunk`s for the same `request_id`.
/// Holds chunks until `total_chunks` have all been observed, then returns
/// the concatenated payload and the (first) header.
class PeReassembler {
public:
    /// Feed one chunk. Returns the fully reassembled chunk if this chunk
    /// completes the transfer; otherwise `nullopt`.
    std::optional<PeChunk> push(PeChunk chunk);

    /// Drop any in-progress transfer for `request_id`.
    void cancel(uint8_t request_id);

    /// Number of in-flight transfers (test introspection).
    std::size_t pending_transfers() const { return slots_.size(); }

private:
    struct Slot {
        uint16_t total = 0;
        uint16_t received = 0;
        std::string header_json;
        std::vector<std::vector<uint8_t>> chunks;  ///< chunk-num-1 -> bytes
        std::vector<bool> seen;                    ///< per-chunk dup guard
    };
    std::unordered_map<uint8_t, Slot> slots_;
};

/// Builds a minimal PE JSON header. Plenty of producers will want to
/// build their own, but this covers the common "resource + status" case
/// and keeps tests from depending on a JSON library at the surface.
///
/// `command` is one of "start" / "end" / "partial" / "full" / "notify".
/// Empty `command` is omitted.
std::string pe_header_make(std::string_view resource,
                           std::string_view command = {},
                           int status = 200);

/// Parse a PE JSON header. Returns true on success and fills the out
/// params; returns false if the header is not valid JSON. Only the
/// fields used by the PE framing are extracted; everything else is
/// considered application data and ignored here.
bool pe_header_parse(std::string_view json,
                     std::string* resource,
                     std::string* command,
                     int* status);

// ── Subscription management ─────────────────────────────────────────────

/// One active subscription state — owned by `PeSubscriptionManager`.
struct PeSubscription {
    std::string subscription_id;   ///< Server-allocated, opaque to client
    std::string resource;
    MUID subscriber;
};

/// Authoritative subscription registry on the responder side.
///
/// Lifecycle:
///   - `subscribe(resource, peer)` returns a new subscription id and
///     records the binding.
///   - `unsubscribe(id)` removes it.
///   - `subscribers_of(resource)` is used by the responder when a
///     resource changes to fan out Notify messages.
class PeSubscriptionManager {
public:
    /// Allocate a subscription. `subscription_id` is generated
    /// deterministically and is unique within this manager instance.
    std::string subscribe(std::string_view resource, MUID subscriber);

    /// Remove a subscription. Returns true if it existed.
    bool unsubscribe(std::string_view subscription_id);

    /// Returns all subscribers (MUID + id) currently bound to `resource`.
    std::vector<PeSubscription> subscribers_of(std::string_view resource) const;

    /// All known subscriptions (test introspection).
    const std::vector<PeSubscription>& all() const { return subs_; }

private:
    std::vector<PeSubscription> subs_;
    uint64_t next_id_ = 1;
};

}  // namespace pulp::midi

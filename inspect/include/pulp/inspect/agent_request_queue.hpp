#pragma once

// agent_request_queue.hpp — a durable, file-backed queue of free-text requests
// a human raises from the running design (the inspector's "send to agent"
// affordance) for the driving agent to pick up on its next turn.
//
// The channel is PULL-BASED by design: the inspector appends a request; the
// agent reads the pending set at the top of its turn and acks what it consumes.
// Nothing is pushed to the agent, so it degrades honestly across agent harnesses
// — the worst case is the agent sees a request one turn later, never that a
// request is lost. This inverts nothing about the MCP request/response model: a
// `pulp_inspect_pending_requests` read tool surfaces the queue; the human's
// "send" button just enqueues a payload the agent reliably picks up.
//
// The queue lives beside the design ledger (`.pulp-design-meta.json`) as
// `.pulp-design-requests.json`, so a request is anchored to a specific project.
// On-disk schema:
//
//   {
//     "$schema": "pulp-design-requests://v1",
//     "version": 1,
//     "requests": [ { "id","text","design","screen",
//                     "editmode_state","screenshot_path","created_at",
//                     "consumed" }, ... ]
//   }
//
// The pure parse/serialize/append/ack functions are separable from disk I/O so
// the model is unit-testable without a filesystem. The file-backed wrappers add
// an atomic write (`<path>.tmp` then rename) so a crash mid-write never leaves a
// half-flushed queue at the canonical path. The core takes no clock and no RNG:
// `created_at` is supplied by the caller and ids are assigned deterministically
// from the existing contents, so a given input always yields the same output.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::inspect {

/// One queued request from the human, with the context the agent needs to act
/// without guessing which design/screen/state it refers to.
struct AgentRequest {
    std::string id;              ///< stable id; assigned on append when empty
    std::string text;            ///< the human's free-text request
    std::string design;          ///< design/asset name (ties to the ledger)
    std::string screen;          ///< optional screen within the design
    std::string editmode_state;  ///< the current EDITMODE block JSON, for context
    std::string screenshot_path; ///< optional path to a captured screenshot
    std::string created_at;      ///< caller-supplied timestamp (the core keeps no clock)
    bool consumed = false;       ///< true once the agent has acked it
};

/// The canonical queue filename (beside `.pulp-design-meta.json`).
std::string_view queue_filename();

/// Serialize a request list to the canonical, stable JSON document.
std::string requests_to_json(const std::vector<AgentRequest>& requests);

/// Parse a queue document. An empty/whitespace input is treated as an empty
/// queue (returns an empty list, not nullopt) so a not-yet-created file reads
/// cleanly. Returns nullopt only when the input is non-empty and not a valid
/// queue document (bad JSON, or no "requests" array). Unknown members are
/// ignored; a request missing "id" or "text" is skipped.
std::optional<std::vector<AgentRequest>> requests_from_json(std::string_view queue_json);

/// Append `request` to the queue encoded in `queue_json`, assigning a fresh id
/// when `request.id` is empty (one greater than the largest numeric id present,
/// starting at "1"). Returns the new queue document. Pure — no I/O. Returns
/// nullopt when `queue_json` is a malformed queue document.
std::optional<std::string> append_request(std::string_view queue_json, AgentRequest request);

/// The not-yet-consumed requests in `queue_json`, in order. A malformed document
/// yields an empty list.
std::vector<AgentRequest> pending_requests(std::string_view queue_json);

/// Mark the request whose id is `id` consumed in `queue_json`. Returns the new
/// document and whether an id matched (false: no such id, document unchanged).
/// Returns nullopt when `queue_json` is a malformed queue document.
std::optional<std::pair<std::string, bool>> ack_request(std::string_view queue_json,
                                                        std::string_view id);

// ── file-backed wrappers (atomic write) ─────────────────────────────────────

/// The queue file path inside `project_dir`.
std::string queue_path(std::string_view project_dir);

/// Read + append + atomically rewrite the queue at `path`. Creates the file when
/// absent. Returns the assigned/kept id on success, nullopt on read/parse/write
/// failure.
std::optional<std::string> enqueue_to_file(const std::string& path, AgentRequest request);

/// The pending requests in the queue file at `path` (empty when the file is
/// absent or unreadable).
std::vector<AgentRequest> read_pending_file(const std::string& path);

/// Ack `id` in the queue file at `path`, rewriting atomically. Returns whether an
/// id matched (false when absent/unreadable/no-match).
bool ack_in_file(const std::string& path, std::string_view id);

}  // namespace pulp::inspect

// Classifies the JSON `pulp control call --json` prints for a graph-file
// reload, so the watch knows whether to resend, stop, or move on.
#pragma once

#include <cstdint>
#include <string>

enum class GraphReloadOutcome {
    // Completed: the file is live. The generation is updated.
    Published,
    // The executor ran and refused the file (typed detail.code). Final.
    Refused,
    // Refused before the executor ran (deadline before start, cancelled).
    // Not applied; the same change may be sent again.
    NotApplied,
    // The CLI or broker refused the caller's authority (no receipt, an
    // `admission-denied` error). Resending with the same grant cannot succeed.
    AuthorityRefused,
    // No terminal receipt: the file may or may not be live.
    Unknown,
};

GraphReloadOutcome classify_graph_reload_receipt(int exit_code, const std::string& output,
                                                 std::uint64_t& generation);

// A stable key for one kind of refusal: the CLI error, explanation and state,
// without per-call identifiers such as receipt ids. Unparseable output is its
// own key.
std::string graph_reload_refusal_key(const std::string& output);

// True when the output is the CLI's admission refusal of this grant.
bool graph_reload_authority_refused(const std::string& output);

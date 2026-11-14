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
    // The executor ran and refused because the request's expected generation
    // no longer matches the live graph (a concurrent edit moved it). Resending
    // the same generation cannot succeed, and the file is not consumed: the
    // watch stops until it is restarted with the current generation.
    StaleGeneration,
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

// Counts pre-executor failures for the file contents currently on offer. A
// different file content starts a fresh budget, so failures of one change never
// halt the watch on the next.
class GraphReloadRetryBudget {
  public:
    explicit GraphReloadRetryBudget(unsigned limit) : limit_(limit) {}
    // Call with the contents about to be sent.
    void offer(const std::string& contents) {
        if (contents != offered_) {
            offered_ = contents;
            failures_ = 0;
        }
    }
    // Records one not-applied failure; true when the limit is reached.
    bool record_failure() {
        return ++failures_ >= limit_;
    }
    void reset() {
        failures_ = 0;
    }
    unsigned failures() const {
        return failures_;
    }

  private:
    unsigned limit_;
    unsigned failures_ = 0;
    std::string offered_;
};

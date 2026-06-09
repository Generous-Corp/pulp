// SPDX-License-Identifier: MIT
//
// IMPORTER_TERMS accept-to-run gate for `pulp import`.
//
// Before the SDK drives a framework importer (inspect / emit), the user must
// have given explicit affirmative acceptance of that importer's terms of use.
// This mirrors `pulp add --accept-license`: an interactive type-to-accept
// prompt, or a `--accept-importer-terms` flag for non-interactive/CI use, with
// acceptance recorded under ~/.pulp.
//
// Vendor-agnostic rule (firm): this unit names NO vendor and NO framework. The
// terms TEXT (and its version + vendor id) is runtime DATA carried by the
// add-on importer's tool-registry descriptor; the SDK only knows the SHAPE of a
// terms record, surfaces it, hashes it, and records acceptance keyed by
// importer id + a hash of the terms text. A changed terms text changes the hash
// and re-prompts. The SDK ships no terms body of its own.
#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace pulp::cli::import_terms {

namespace fs = std::filesystem;

// The terms a single importer presents, supplied as DATA by the add-on (its
// tool-registry descriptor). All fields are opaque to the SDK.
struct TermsDescriptor {
    std::string importer_id;    // tool id of the importer add-on
    std::string terms_version;  // add-on-versioned; bumping it re-prompts
    std::string terms_text;     // the body shown at the gate (vendor-supplied)
    std::string vendor_id;      // recorded for the audit trail (opaque)
};

// True when the importer declares a non-empty terms body. When false there is
// nothing to accept and the gate passes through.
bool has_terms(const TermsDescriptor& td);

// FNV-1a (64-bit) hex digest of the terms text. Stable, dependency-free, and
// sufficient to detect a changed body (which must re-prompt). NOT a security
// hash — it only keys acceptance to a specific terms revision.
std::string terms_hash(const std::string& terms_text);

// One recorded acceptance. Mirrors the template's record shape:
// {terms_version, importer, importer_version, vendor_id, timestamp,
//  acceptance_hash}. `importer_version` is currently unused by the SDK (the
// add-on owns versioning via `terms_version`) but is kept in the record shape
// for forward compatibility and round-trips through the store.
struct AcceptanceRecord {
    std::string importer_id;
    std::string terms_version;
    std::string terms_hash;
    std::string vendor_id;
    std::string accepted_at;   // ISO-8601 UTC
    std::string method;        // "interactive" | "flag"
};

// Pure: is there a recorded acceptance for this importer id whose hash matches
// the current terms text? A version change or text edit changes the hash and so
// fails this check (forcing a re-prompt).
bool is_accepted(const std::vector<AcceptanceRecord>& records,
                 const std::string& importer_id,
                 const std::string& current_hash);

// Pure round-trip with the on-disk acceptance store. The store is a small JSON
// array; parsing tolerates absent/old fields and ignores malformed entries.
std::vector<AcceptanceRecord> parse_acceptance_store(const std::string& json_text);
std::string format_acceptance_store(const std::vector<AcceptanceRecord>& records);

// Upsert `rec` into `records` (replace any prior acceptance for the same
// importer id). Pure — returns the new vector.
std::vector<AcceptanceRecord> upsert_acceptance(
    std::vector<AcceptanceRecord> records, const AcceptanceRecord& rec);

// Default acceptance-store path: ~/.pulp/importer-terms-accepted.json (honours
// $PULP_HOME). Empty when no home is resolvable.
fs::path acceptance_store_path();

// Load / save the acceptance store at `path` (reads/writes JSON). `load` never
// throws — a missing or unreadable file yields an empty vector.
std::vector<AcceptanceRecord> load_acceptance_store(const fs::path& path);
bool save_acceptance_store(const fs::path& path,
                           const std::vector<AcceptanceRecord>& records);

// The full gate body shown to the user: a header, the vendor-supplied terms
// text, and the type-to-accept instruction. Pure — no I/O.
std::string render_gate(const TermsDescriptor& td);

// Outcome of the accept-to-run gate.
enum class GateResult {
    Accepted,           // already accepted, or accepted just now (recorded)
    NoTermsToAccept,    // importer declares no terms — pass through
    Declined,           // user declined at the interactive prompt
    NonInteractive,     // not a TTY and no --accept flag — cannot prompt
    StoreError,         // could not persist the acceptance record
};

// Drive the accept-to-run gate for one importer. Reads the acceptance store at
// `store_path`; if already accepted (matching hash) returns Accepted without
// prompting. Otherwise, if `accept_flag` is set, records acceptance
// (method="flag") and returns Accepted. Otherwise, if interactive, renders the
// gate to `out`, reads a typed confirmation from `in`, and records on accept.
// When not interactive and no flag, returns NonInteractive.
//
// `now_utc` is injected (the time-of-accept) so tests are deterministic.
struct GateIo {
    std::istream& in;
    std::ostream& out;
    bool interactive;  // is `in` a TTY the user can type into?
};

GateResult run_gate(const TermsDescriptor& td,
                    const fs::path& store_path,
                    bool accept_flag,
                    const std::string& now_utc,
                    GateIo io);

}  // namespace pulp::cli::import_terms

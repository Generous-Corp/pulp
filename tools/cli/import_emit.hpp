// SPDX-License-Identifier: MIT
//
// Emission-manifest parsing + write-plan for `pulp import emit`.
//
// The SPI `emit` verb returns an EmissionManifest: a list of files the importer
// PROPOSES (generated/stub carry inline content; verbatim portable-core copies
// carry an absolute `copy_from` source path), plus the migration-status
// document, the format split, and the unresolved notes. The SDK — not the
// importer — WRITES each file under the user's output dir, runs the clean-room
// output scan over the generated files, and records provenance.
//
// This header keeps the manifest model and the write-plan computation as PURE
// functions over plain structs so they are unit-testable without spawning an
// importer. The spawn/IO shell lives in cmd_import.cpp.
//
// Framework identity is runtime DATA throughout — this header names no vendor.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulp::cli::import_emit {

namespace fs = std::filesystem;

// The `schema` marker every EmissionManifest must carry (SPI emit_result).
// parse_manifest rejects a manifest whose schema is present but differs — the
// guard against an analyze ProjectIR (or a future incompatible version) being
// misrouted into the emit path.
inline constexpr const char* kEmissionManifestSchema =
    "pulp.import.emission_manifest.v0";

// Provenance vocabulary mirrors the SPI emit_file schema.
enum class Provenance { Generated, CopiedUserFile, Stub, Unknown };

Provenance parse_provenance(const std::string& s);
const char* provenance_name(Provenance p);

// One proposed file from the manifest. Exactly one of `content` (generated/
// stub) or `copy_from` (verbatim copied-user-file) is meaningful, keyed by
// `provenance`.
struct ManifestFile {
    std::string path;            // relative to the output dir
    Provenance provenance = Provenance::Unknown;
    std::string classification;  // optional hint ("source"/"build"/…)
    std::string content;         // inline content (generated/stub)
    std::string copy_from;       // absolute source path (copied-user-file)
    bool has_content = false;    // whether the `content` field was present
    bool has_copy_from = false;  // whether the `copy_from` field was present
};

// A parsed EmissionManifest (the `emit` verb's `result` object).
struct Manifest {
    bool ok = false;             // parsed into a usable manifest?
    std::string parse_error;     // populated when ok == false
    std::string schema;
    std::string importer_id;
    std::string framework;
    std::vector<ManifestFile> files;
    std::string migration_status_json;  // raw JSON object text (or empty)
    std::vector<std::string> formats;
    std::vector<std::string> deferred_formats;
    std::vector<std::string> unresolved;
    std::string verdict;
};

// Parse the EmissionManifest result JSON (the `result` object the SPI runner
// re-serialised). Pure — no IO. On a structural problem, returns a Manifest
// with ok==false and parse_error set.
Manifest parse_manifest(const std::string& result_json);

// A single resolved write action, after validating the manifest against the
// output dir. The write-plan is pure: it computes WHERE each file goes and HOW
// (write inline content vs. copy a source file) without touching disk.
struct WriteAction {
    fs::path dest;               // absolute destination path
    Provenance provenance = Provenance::Unknown;
    bool is_copy = false;        // copy `source` -> dest, vs. write `content`
    fs::path source;             // absolute source for copies
    const ManifestFile* file = nullptr;  // back-pointer into the manifest
};

struct WritePlan {
    bool ok = false;
    std::string error;           // first validation failure (path escape, etc.)
    std::vector<WriteAction> actions;
};

// Compute the write plan for `manifest` materialised under `output_dir`.
// Validates that no destination escapes `output_dir` (no absolute paths, no
// `..` traversal) and that copy sources are absolute and named. Pure — no IO.
WritePlan compute_write_plan(const Manifest& manifest, const fs::path& output_dir);

// User-facing summary fragments for `pulp import emit`. Kept pure so the CLI
// wording can be pinned without spawning an importer.
std::string format_unresolved_summary(const Manifest& manifest);
std::string format_parity_review_step(const Manifest& manifest);

}  // namespace pulp::cli::import_emit

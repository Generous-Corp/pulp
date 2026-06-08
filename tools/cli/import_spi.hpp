// SPDX-License-Identifier: MIT
//
// JSON-over-stdio SPI runner for project importers.
//
// The Pulp SDK (host) drives an installed importer process by writing one
// request envelope (a single JSON line) to its stdin and reading one
// response envelope (a single JSON line) from its stdout. stderr is
// human/log only. The wire contract is `tools/import/schemas/
// import-spi-v0.schema.json`: verbs detect/analyze/plan/emit, request
// `{spi_version,verb,id,payload}`, response `{spi_version,id,ok,result|
// error,diagnostics}`.
//
// Framework identity is runtime DATA throughout — this header names no
// vendor and no framework. The importer command string is resolved by the
// caller (from the tool registry or an explicit --importer-cmd override).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pulp::cli::import_spi {

// The SPI version this SDK build speaks. Bump only with a wire change.
inline constexpr int kSpiVersion = 0;

// One structured diagnostic carried alongside a response.
struct Diagnostic {
    std::string severity;  // "info" | "warning" | "error" (importer-defined)
    std::string code;
    std::string message;
};

// A parsed SPI response envelope. `result_json` / `error_*` are the raw
// payload fields; callers that need typed access reparse `result_json`
// with their own JSON reader (the result shape is verb-specific and the
// SDK keeps this runner shape-agnostic).
struct SpiResponse {
    bool transport_ok = false;   // did we spawn + read a single line at all?
    bool ok = false;             // the `ok` field of the response envelope
    int spi_version = -1;
    std::string id;
    std::string result_json;     // raw JSON text of `result` (object), or empty
    std::string error_code;
    std::string error_message;
    std::vector<Diagnostic> diagnostics;
    std::string transport_error; // populated when transport_ok == false
    std::string raw_stdout;      // first non-empty stdout line, for diagnostics
};

// Build a one-line request envelope `{spi_version,verb,id,payload}`.
// `payload_json` must be a JSON object literal (e.g. `{"project_dir":"…"}`);
// it is embedded verbatim. Returns a single line WITHOUT a trailing newline.
std::string build_request(const std::string& verb,
                          const std::string& id,
                          const std::string& payload_json,
                          int spi_version = kSpiVersion);

// Parse a single response-envelope JSON line. Pure function — unit-tested
// directly. `transport_ok` is true when the line parsed as a response
// object carrying `ok`; otherwise `transport_error` explains why.
SpiResponse parse_response(const std::string& line);

// Validate the negotiated version against the importer's [spi_min,spi_max]
// registry window. Returns an empty string when compatible, else a
// human-facing mismatch message ("upgrade Pulp" / "upgrade importer").
std::string check_version(int response_spi_version, int spi_min, int spi_max);

// How to launch the importer. Exactly one of `command_line` (a shell
// command string, split on whitespace honouring simple quotes) or
// `argv` (pre-split) is used; `argv` wins when non-empty.
struct ImporterInvocation {
    std::string command_line;
    std::vector<std::string> argv;
    std::string working_directory;
    int timeout_ms = 60000;
};

// Spawn the importer, write `request_line` + '\n' to stdin, read the first
// non-empty stdout line, and parse it. Reuses pulp::platform::ChildProcess
// (posix_spawn / CreateProcess). Never throws; transport failures land in
// `transport_ok == false` with `transport_error` set.
SpiResponse run(const ImporterInvocation& inv, const std::string& request_line);

// Split a shell-style command string into argv. Honours single and double
// quotes and backslash escapes; does NOT expand globs or variables. Exposed
// for tests and for --importer-cmd parsing.
std::vector<std::string> split_command(const std::string& cmd);

}  // namespace pulp::cli::import_spi

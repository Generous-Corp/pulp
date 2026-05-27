// notary_env.hpp — App Store Connect notary credentials env-file parser
//
// Parses `~/.config/pulp/secrets/notary.env` (or any KEY=VALUE bash-style
// file) and resolves the App Store Connect API key trio used by both
// `xcrun notarytool submit --key/--key-id/--issuer` and `rcodesign notary
// --api-key-path` on Linux. The legacy `--apple-id` / `--team-id` /
// app-specific-password flow remains as a fallback so existing users
// don't break.
//
// Resolution precedence (highest wins):
//   1. CLI flags (e.g. `--api-key`, `--api-key-id`, `--api-issuer`)
//   2. Environment variables already exported in the shell
//      (PULP_NOTARY_KEY_PATH, PULP_NOTARY_KEY_ID, PULP_NOTARY_ISSUER_ID)
//   3. notary.env file under PULP_NOTARY_ENV (test override) or
//      $HOME/.config/pulp/secrets/notary.env
//
// The parser is intentionally CHOC-flavoured but does not require CHOC —
// the surface is small enough that hand-rolling avoids a build dependency
// on `pulp::runtime` from the test target.

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace pulp::cli::notary {

// Parsed notary.env contents — every key is optional. Empty strings
// indicate "key was not present in the file"; the caller layers env
// vars + CLI flags over the top.
struct NotaryEnvFile {
    std::string key_path;     // PULP_NOTARY_KEY_PATH
    std::string key_id;       // PULP_NOTARY_KEY_ID
    std::string issuer_id;    // PULP_NOTARY_ISSUER_ID
    std::string sign_identity;  // PULP_SIGN_IDENTITY
    std::string team_id;      // PULP_TEAM_ID
    // Raw parsed map for forward-compat (lets callers read keys the
    // struct doesn't model yet without re-parsing).
    std::unordered_map<std::string, std::string> raw;
};

// Parse a bash-style `KEY=VALUE` env file. Supports:
//   - `# …` line comments and inline `… # comment`
//   - blank lines
//   - quoted values: KEY="value with spaces", KEY='literal $value'
//   - `$HOME` expansion in double-quoted and unquoted values
//   - `export KEY=value` (the leading `export` is dropped)
//   - leading/trailing whitespace on keys and unquoted values is trimmed
//
// Single-quoted values are taken literally (no $HOME expansion). Lines
// that don't match `KEY=…` are skipped silently. Malformed quotes are
// also skipped silently — the caller checks for required fields after
// parsing, so a half-typed line never propagates as a partial value.
//
// `home` is the path used to expand `$HOME` / `${HOME}`. Pass
// `std::getenv("HOME")` for production code; the test fixture passes a
// fake home so it can verify expansion without touching the real one.
NotaryEnvFile parse_env_text(const std::string& text,
                             const std::string& home);

// Convenience: read a file and parse it. Returns empty struct (with
// empty fields) if the file doesn't exist. Caller uses `path_exists`
// to distinguish "no file" from "file with no keys".
NotaryEnvFile parse_env_file(const std::filesystem::path& path,
                             const std::string& home);

// Default lookup path used when --api-key-env-file is not supplied:
//   1. $PULP_NOTARY_ENV (test override)
//   2. $HOME/.config/pulp/secrets/notary.env
std::filesystem::path default_env_path();

// Resolved credentials after CLI > env > file precedence. Every field
// is a final string ready to be passed to notarytool — including the
// `$HOME`-expanded `key_path`.
struct ResolvedNotaryCreds {
    std::string key_path;    // Path to the .p8 file (or empty)
    std::string key_id;      // Apple Key ID (e.g. "4HY9U7QPZQ")
    std::string issuer_id;   // Apple Issuer UUID

    // Source diagnostics. Each string says where the value came from
    // — "cli", "env", "file", or "" (unset). Lets the CLI print
    // "Resolved key-id from notary.env" hints without exposing the
    // actual value beyond a redacted tail.
    std::string key_path_source;
    std::string key_id_source;
    std::string issuer_id_source;

    // Convenience: did we resolve all three required pieces?
    bool complete() const {
        return !key_path.empty() && !key_id.empty() && !issuer_id.empty();
    }
};

// Layered resolution. `cli_*` strings come from `--api-key/--api-key-id/
// `--api-issuer` flags (empty when not supplied). `getenv` is a
// pluggable lookup so tests can inject env without touching the real
// process environment. `file` is the parsed env file (empty struct is
// fine — just means no file was found).
ResolvedNotaryCreds resolve_creds(
    const std::string& cli_key_path,
    const std::string& cli_key_id,
    const std::string& cli_issuer_id,
    const NotaryEnvFile& file,
    const std::function<std::optional<std::string>(const std::string&)>& getenv);

// Redact a path for diagnostics: keep the last 16 chars, prefix the
// rest with "…/". So "/Users/dr/.config/pulp/secrets/AuthKey_X.p8"
// becomes "…/AuthKey_X.p8". Never logs the raw key contents.
std::string redact_path(const std::string& path);

} // namespace pulp::cli::notary

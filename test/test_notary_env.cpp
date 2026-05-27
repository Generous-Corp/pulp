// test_notary_env.cpp — unit tests for the App Store Connect notary
// env-file parser and CLI > env > file resolution precedence.
//
// Compiles `tools/cli/notary_env.cpp` directly so the test target has
// no dependency on pulp::runtime / pulp::ship — the parser is pure
// stdlib by design (see tools/cli/notary_env.hpp commentary).
//
// Pin-by-test coverage maps directly to the SCOPE items in the
// 2026-05-26 ASC-key flow task:
//   - parse a fixture env file (the exact format the user stores)
//   - $HOME expansion both inside double quotes and on bare values
//   - resolution precedence CLI > env > file
//   - graceful empty-result when no file and no env vars
//   - redaction of the .p8 path tail for diagnostics

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/notary_env.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;
namespace notary = pulp::cli::notary;

namespace {

void set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

void unset_env_var(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

fs::path write_tmp_env(const std::string& body) {
    auto path = fs::temp_directory_path()
        / ("notary-env-" + std::to_string(reinterpret_cast<uintptr_t>(&body))
           + "-" + std::to_string(std::rand()) + ".env");
    std::ofstream out(path);
    out << body;
    out.close();
    return path;
}

constexpr const char* kFixture =
    "# Pulp signing/notary creds. Local only. NEVER commit.\n"
    "\n"
    "# Developer ID Application (already in macOS Keychain):\n"
    "PULP_SIGN_IDENTITY=\"Developer ID Application: Daniel Raffel (95CX6P84C4)\"\n"
    "PULP_SIGN_IDENTITY_SHA=\"B9848C0971AEA614DA72C0E9C1A3A791BB957FF0\"\n"
    "PULP_TEAM_ID=\"95CX6P84C4\"\n"
    "\n"
    "# App Store Connect API key:\n"
    "PULP_NOTARY_KEY_ID=\"4HY9U7QPZQ\"\n"
    "PULP_NOTARY_ISSUER_ID=\"5e8f0b95-3e2f-48e7-b7c2-52e7c220502a\"\n"
    "PULP_NOTARY_KEY_PATH=\"$HOME/.config/pulp/secrets/AuthKey_4HY9U7QPZQ.p8\"\n"
    "\n"
    "# Optional: P12 for Linux rcodesign\n"
    "PULP_SIGN_P12_PATH=\"$HOME/.config/pulp/secrets/developerID.p12\"\n";

} // namespace

TEST_CASE("notary env: parses the user's reference fixture", "[notary][env]") {
    auto env = notary::parse_env_text(kFixture, "/Users/dr");
    REQUIRE(env.key_id == "4HY9U7QPZQ");
    REQUIRE(env.issuer_id == "5e8f0b95-3e2f-48e7-b7c2-52e7c220502a");
    REQUIRE(env.key_path == "/Users/dr/.config/pulp/secrets/AuthKey_4HY9U7QPZQ.p8");
    REQUIRE(env.team_id == "95CX6P84C4");
    REQUIRE(env.sign_identity == "Developer ID Application: Daniel Raffel (95CX6P84C4)");
    // Raw map still carries forward-compat keys
    REQUIRE(env.raw.at("PULP_SIGN_P12_PATH") == "/Users/dr/.config/pulp/secrets/developerID.p12");
}

TEST_CASE("notary env: empty HOME leaves $HOME literal", "[notary][env]") {
    auto env = notary::parse_env_text(
        R"(PULP_NOTARY_KEY_PATH="$HOME/key.p8")", /*home=*/"");
    REQUIRE(env.key_path == "$HOME/key.p8");
}

TEST_CASE("notary env: single quotes are literal", "[notary][env]") {
    auto env = notary::parse_env_text(
        R"(PULP_NOTARY_KEY_PATH='$HOME/key.p8')", "/Users/dr");
    REQUIRE(env.key_path == "$HOME/key.p8");
}

TEST_CASE("notary env: bare unquoted values strip inline comments", "[notary][env]") {
    auto env = notary::parse_env_text(
        "PULP_NOTARY_KEY_ID=ABC123 # the bare key id\n", "/Users/dr");
    REQUIRE(env.key_id == "ABC123");
}

TEST_CASE("notary env: export prefix is dropped", "[notary][env]") {
    auto env = notary::parse_env_text(
        "export PULP_NOTARY_KEY_ID=\"XYZ\"\n", "/Users/dr");
    REQUIRE(env.key_id == "XYZ");
}

TEST_CASE("notary env: ${HOME} braced form expands", "[notary][env]") {
    auto env = notary::parse_env_text(
        R"(PULP_NOTARY_KEY_PATH="${HOME}/key.p8")", "/Users/dr");
    REQUIRE(env.key_path == "/Users/dr/key.p8");
}

TEST_CASE("notary env: malformed lines are skipped", "[notary][env]") {
    auto env = notary::parse_env_text(
        "# only a comment\n"
        "no equals sign here\n"
        "not-an-identifier=should-skip\n"
        "PULP_NOTARY_KEY_ID=GOOD\n",
        "/Users/dr");
    REQUIRE(env.key_id == "GOOD");
    REQUIRE(env.key_path.empty());
}

TEST_CASE("notary env: parse_env_file returns empty on missing file",
          "[notary][env]") {
    auto env = notary::parse_env_file(
        "/no/such/path/notary.env", "/Users/dr");
    REQUIRE(env.key_id.empty());
    REQUIRE(env.key_path.empty());
}

TEST_CASE("notary env: parse_env_file reads the fixture from disk",
          "[notary][env]") {
    auto path = write_tmp_env(kFixture);
    auto env = notary::parse_env_file(path, "/Users/dr");
    REQUIRE(env.key_id == "4HY9U7QPZQ");
    REQUIRE(env.issuer_id == "5e8f0b95-3e2f-48e7-b7c2-52e7c220502a");
    fs::remove(path);
}

// ── precedence: CLI > env > file ────────────────────────────────────

TEST_CASE("notary creds: CLI flag beats env beats file", "[notary][resolve]") {
    auto file = notary::parse_env_text(kFixture, "/Users/dr");

    // env-fn that returns "env-value" for KEY_ID only — the issuer and
    // key-path are unset in the env, so those must come from the file.
    auto getenv_fn = [](const std::string& name)
        -> std::optional<std::string> {
        if (name == "PULP_NOTARY_KEY_ID") return std::string("ENV_KEY_ID");
        return std::nullopt;
    };

    auto creds = notary::resolve_creds(
        /*cli_key_path=*/"/tmp/cli-key.p8",
        /*cli_key_id=*/"",       // not set on CLI → env wins
        /*cli_issuer_id=*/"",     // not set on CLI/env → file wins
        file, getenv_fn);

    REQUIRE(creds.key_path == "/tmp/cli-key.p8");
    REQUIRE(creds.key_path_source == "cli");
    REQUIRE(creds.key_id == "ENV_KEY_ID");
    REQUIRE(creds.key_id_source == "env");
    REQUIRE(creds.issuer_id == "5e8f0b95-3e2f-48e7-b7c2-52e7c220502a");
    REQUIRE(creds.issuer_id_source == "file");
    REQUIRE(creds.complete());
}

TEST_CASE("notary creds: empty everywhere yields no source", "[notary][resolve]") {
    notary::NotaryEnvFile empty;
    auto creds = notary::resolve_creds(
        "", "", "", empty,
        [](const std::string&) -> std::optional<std::string> { return std::nullopt; });
    REQUIRE_FALSE(creds.complete());
    REQUIRE(creds.key_path_source.empty());
    REQUIRE(creds.key_id_source.empty());
    REQUIRE(creds.issuer_id_source.empty());
}

TEST_CASE("notary creds: CLI alone is sufficient (no env file needed)",
          "[notary][resolve]") {
    notary::NotaryEnvFile empty;
    auto creds = notary::resolve_creds(
        "/path/to/AuthKey_X.p8", "X", "uuid-here", empty,
        [](const std::string&) -> std::optional<std::string> { return std::nullopt; });
    REQUIRE(creds.complete());
    REQUIRE(creds.key_path_source == "cli");
    REQUIRE(creds.key_id_source == "cli");
    REQUIRE(creds.issuer_id_source == "cli");
}

TEST_CASE("notary creds: env alone is sufficient", "[notary][resolve]") {
    notary::NotaryEnvFile empty;
    auto getenv_fn = [](const std::string& name)
        -> std::optional<std::string> {
        if (name == "PULP_NOTARY_KEY_PATH")  return std::string("/p/key.p8");
        if (name == "PULP_NOTARY_KEY_ID")    return std::string("KID");
        if (name == "PULP_NOTARY_ISSUER_ID") return std::string("ISSUER");
        return std::nullopt;
    };
    auto creds = notary::resolve_creds("", "", "", empty, getenv_fn);
    REQUIRE(creds.complete());
    REQUIRE(creds.key_path == "/p/key.p8");
    REQUIRE(creds.key_path_source == "env");
    REQUIRE(creds.key_id_source == "env");
    REQUIRE(creds.issuer_id_source == "env");
}

// ── redaction ───────────────────────────────────────────────────────

TEST_CASE("notary: redact_path keeps only the trailing filename",
          "[notary][redact]") {
    REQUIRE(notary::redact_path(
        "/Users/dr/.config/pulp/secrets/AuthKey_4HY9U7QPZQ.p8")
        == "…/AuthKey_4HY9U7QPZQ.p8");
    // Short paths stay untouched
    REQUIRE(notary::redact_path("key.p8") == "key.p8");
    REQUIRE(notary::redact_path("") == "");
}

// ── default_env_path ────────────────────────────────────────────────
//
// Cover both the override env-var and the default $HOME/.config path.
// We use ScopedEnvVar-style RAII inline so we don't drag in the
// shell-out fixture.

namespace {
struct ScopedEnv {
    std::string name;
    std::optional<std::string> prior;
    ScopedEnv(const char* n, const char* v) : name(n) {
        if (auto* old = std::getenv(n)) prior = old;
        if (v) set_env_var(n, v); else unset_env_var(n);
    }
    ~ScopedEnv() {
        if (prior) set_env_var(name.c_str(), prior->c_str());
        else unset_env_var(name.c_str());
    }
};
} // namespace

TEST_CASE("notary: PULP_NOTARY_ENV overrides the default path",
          "[notary][env-path]") {
    ScopedEnv override("PULP_NOTARY_ENV", "/tmp/override.env");
    REQUIRE(notary::default_env_path() == fs::path("/tmp/override.env"));
}

TEST_CASE("notary: default path falls under $HOME/.config when no override",
          "[notary][env-path]") {
    ScopedEnv clear_override("PULP_NOTARY_ENV", nullptr);
    ScopedEnv home("HOME", "/Users/test");
    auto p = notary::default_env_path();
    REQUIRE(p == fs::path("/Users/test/.config/pulp/secrets/notary.env"));
}

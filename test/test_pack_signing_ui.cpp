// Human-facing signing helpers: consistent key names across keychain + GitHub
// secrets, a signing summary that surfaces the policy fields, and a provenance
// banner that names a freshly generated key and its backup consequences.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pulp/format/reload/pack_signing_ui.hpp>

using namespace pulp::format::reload;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("key names are consistent + discoverable across keychain and secrets",
          "[reload][signing]") {
    REQUIRE(reload_keychain_service(KeyRole::Signing, "com.pulp.demo") ==
            "pulp.reload.signing.com.pulp.demo");
    REQUIRE(reload_keychain_service(KeyRole::Revocation, "com.pulp.demo") ==
            "pulp.reload.revocation.com.pulp.demo");
    // Generation + short fingerprint keep rotations from overwriting in place.
    REQUIRE(reload_key_account(1, "ab12cd34ef") == "gen1-ab12cd34");
    REQUIRE(reload_key_account(2, "") == "gen2");
    // GitHub secret mirrors the keychain shape, secret-safe (upper-snake).
    REQUIRE(reload_github_secret_name(KeyRole::Signing, "com.pulp.demo", 1) ==
            "PULP_RELOAD_SIGNING_KEY_COM_PULP_DEMO_GEN1");
    REQUIRE(reload_github_secret_name(KeyRole::Revocation, "my-synth", 3) ==
            "PULP_RELOAD_REVOCATION_KEY_MY_SYNTH_GEN3");
}

TEST_CASE("signing summary surfaces the policy fields the signature covers",
          "[reload][signing]") {
    SwapPackManifest m;
    m.id = "p1"; m.plugin_id = "com.pulp.demo";
    m.pack_version = 4;
    m.pack_type = SwapPackKind::UiScript;
    m.update_channel = "stable";
    m.min_host_version = 8;
    m.declared_capabilities = {"filesystem", "network"};
    m.files = {{"ui.js", "abcdef0123456789", SwapPackKind::UiScript}};

    const auto s = swap_pack_signing_summary(m);
    REQUIRE_THAT(s, ContainsSubstring("com.pulp.demo"));
    REQUIRE_THAT(s, ContainsSubstring("pack version:  4"));
    REQUIRE_THAT(s, ContainsSubstring("ui-script"));
    REQUIRE_THAT(s, ContainsSubstring("stable"));
    REQUIRE_THAT(s, ContainsSubstring("filesystem, network"));
    REQUIRE_THAT(s, ContainsSubstring("ui.js"));
    REQUIRE_THAT(s, ContainsSubstring("abcdef01"));  // short hash shown
}

TEST_CASE("signing summary says pure-UI when no capabilities are declared",
          "[reload][signing]") {
    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "q";
    m.files = {{"ui.js", "00", SwapPackKind::UiScript}};
    REQUIRE_THAT(swap_pack_signing_summary(m), ContainsSubstring("none — pure UI"));
}

TEST_CASE("github backup refuses the core repo + malformed targets, allows a plugin repo",
          "[reload][signing]") {
    // Allowed: a developer's own plugin repo (various URL forms all normalize).
    REQUIRE(github_backup_repo_allowed("me/my-synth"));
    REQUIRE(github_backup_repo_allowed("git@github.com:me/my-synth.git"));
    REQUIRE(github_backup_repo_allowed("https://github.com/me/my-synth"));

    // Refused: the core Pulp repo, in every form — BOTH names. The framework
    // moved danielraffel/pulp -> Generous-Corp/pulp and GitHub redirects the
    // old name to the new repo, so a key targeting either lands in core.
    REQUIRE_FALSE(github_backup_repo_allowed("danielraffel/pulp"));
    REQUIRE_FALSE(github_backup_repo_allowed("DanielRaffel/Pulp"));
    REQUIRE_FALSE(github_backup_repo_allowed("git@github.com:danielraffel/pulp.git"));
    REQUIRE_FALSE(github_backup_repo_allowed("https://github.com/danielraffel/pulp"));
    REQUIRE_FALSE(github_backup_repo_allowed("Generous-Corp/pulp"));
    REQUIRE_FALSE(github_backup_repo_allowed("generous-corp/pulp"));
    REQUIRE_FALSE(github_backup_repo_allowed("git@github.com:Generous-Corp/pulp.git"));
    REQUIRE_FALSE(github_backup_repo_allowed("https://github.com/Generous-Corp/pulp"));

    // Refused: malformed / ambiguous targets (fail closed).
    REQUIRE_FALSE(github_backup_repo_allowed(""));
    REQUIRE_FALSE(github_backup_repo_allowed("just-a-name"));
    REQUIRE_FALSE(github_backup_repo_allowed("owner/"));
    REQUIRE_FALSE(github_backup_repo_allowed("/name"));
    REQUIRE_FALSE(github_backup_repo_allowed("a/b/c"));
}

TEST_CASE("key-generation banner names the key, its location, and the consequences",
          "[reload][signing]") {
    const auto svc = reload_keychain_service(KeyRole::Signing, "com.pulp.demo");
    const auto sign = key_generation_banner(KeyRole::Signing, svc, "com.pulp.demo");
    REQUIRE_THAT(sign, ContainsSubstring("generated a new signing key"));
    REQUIRE_THAT(sign, ContainsSubstring(svc));                    // names where it lives
    REQUIRE_THAT(sign, ContainsSubstring("back it up"));
    REQUIRE_THAT(sign, ContainsSubstring("cannot ship signed updates"));

    const auto rev = key_generation_banner(KeyRole::Revocation,
                                           reload_keychain_service(KeyRole::Revocation, "com.pulp.demo"),
                                           "com.pulp.demo");
    REQUIRE_THAT(rev, ContainsSubstring("Keep it OFFLINE"));
    REQUIRE_THAT(rev, ContainsSubstring("cannot revoke"));
}

// Signed Revocation List (SRL) + namespaced monotonic epoch floor.
//
// The SRL is the reload kill-switch: after a signer key or artifact is found
// malicious, the publisher signs an SRL naming the revoked fingerprints/hashes.
// These tests exercise the two independent gates fail-closed:
//   * verify_srl()      — Ed25519 authenticity (bad-sig / tamper / untrusted / malformed reject)
//   * EpochFloorStore    — monotonic anti-rollback + namespaced isolation + offline-last-known
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/revocation.hpp>
#include <pulp/runtime/crypto.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace pulp::format::reload;
namespace fs = std::filesystem;

namespace {

// Deterministic keypair from a fixed seed byte (reproducible test vectors).
pulp::runtime::Ed25519KeyPair key_from(std::uint8_t seed_byte) {
    std::vector<std::uint8_t> seed(pulp::runtime::ed25519_seed_size, seed_byte);
    auto kp = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());
    return *kp;
}

// Sign an SRL in place with the given private key, over the canonical message.
void sign_srl(SignedRevocationList& srl, const pulp::runtime::Ed25519KeyPair& kp) {
    srl.signer_public_key = kp.public_key;
    const auto msg = srl_signed_message(srl);
    auto sig = pulp::runtime::ed25519_sign(kp.private_key.data(), kp.private_key.size(),
                                           msg.data(), msg.size());
    REQUIRE(sig.has_value());
    srl.signature = *sig;
}

SignedRevocationList make_srl(std::uint64_t epoch) {
    SignedRevocationList srl;
    srl.schema_version = 1;
    srl.epoch = epoch;
    srl.issued_utc = "2026-07-05T00:00:00Z";
    srl.revoked_signer_key_fprs = {"deadbeef", "cafef00d"};
    srl.revoked_artifact_hashes = {"aa11bb22", "cc33dd44"};
    return srl;
}

fs::path make_root(const std::string& name) {
    auto root = fs::temp_directory_path() / ("pulp-srl-" + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    return root;
}

}  // namespace

TEST_CASE("SRL parse + verify round-trip", "[reload][revocation]") {
    auto kp = key_from(0x11);
    auto srl = make_srl(5);
    sign_srl(srl, kp);

    // Serialize → parse → verify against the trusted revocation key.
    const std::string json = serialize_srl_json(srl);
    std::string err;
    auto parsed = parse_srl(json, err);
    REQUIRE(parsed.has_value());
    REQUIRE(err.empty());
    CHECK(parsed->epoch == 5);
    CHECK(parsed->schema_version == 1);
    CHECK(parsed->issued_utc == "2026-07-05T00:00:00Z");
    CHECK(parsed->revoked_signer_key_fprs.size() == 2);
    CHECK(parsed->revoked_artifact_hashes.size() == 2);

    auto res = verify_srl(*parsed, kp.public_key);
    CHECK(res.ok());
    CHECK(res.status == SrlVerify::Ok);
}

TEST_CASE("SRL rejects a signature from an untrusted signer", "[reload][revocation]") {
    auto issuer = key_from(0x22);
    auto attacker = key_from(0x23);  // different trust root
    auto srl = make_srl(3);
    sign_srl(srl, issuer);

    // Signed correctly by `issuer`, but the gate only trusts `attacker`'s key.
    auto res = verify_srl(srl, attacker.public_key);
    CHECK_FALSE(res.ok());
    CHECK(res.status == SrlVerify::UntrustedSigner);
}

TEST_CASE("SRL rejects a bad / mismatched signature", "[reload][revocation]") {
    auto kp = key_from(0x33);
    auto srl = make_srl(7);
    sign_srl(srl, kp);

    // Signature is for epoch 7; mutate the field it covers → verify must fail.
    srl.epoch = 8;
    auto res = verify_srl(srl, kp.public_key);
    CHECK_FALSE(res.ok());
    CHECK(res.status == SrlVerify::BadSignature);
}

TEST_CASE("SRL rejects a single-bit tamper of the signature", "[reload][revocation]") {
    auto kp = key_from(0x44);
    auto srl = make_srl(9);
    sign_srl(srl, kp);

    // Flip one bit of the detached signature.
    REQUIRE(srl.signature.size() == pulp::runtime::ed25519_signature_size);
    srl.signature[10] ^= 0x01;
    auto res = verify_srl(srl, kp.public_key);
    CHECK_FALSE(res.ok());
    CHECK(res.status == SrlVerify::BadSignature);
}

TEST_CASE("SRL tamper of a revoked-hash entry breaks the signature", "[reload][revocation]") {
    auto kp = key_from(0x45);
    auto srl = make_srl(4);
    sign_srl(srl, kp);

    // Attacker tries to remove an artifact from the revocation set after signing.
    srl.revoked_artifact_hashes = {"aa11bb22"};  // dropped one entry
    auto res = verify_srl(srl, kp.public_key);
    CHECK_FALSE(res.ok());
    CHECK(res.status == SrlVerify::BadSignature);
}

TEST_CASE("SRL parse fails closed on malformed input", "[reload][revocation]") {
    std::string err;
    CHECK_FALSE(parse_srl("not json at all", err).has_value());
    CHECK_FALSE(err.empty());
    CHECK_FALSE(parse_srl("[1,2,3]", err).has_value());                 // not an object
    CHECK_FALSE(parse_srl(R"({"schema_version":1})", err).has_value()); // no epoch
    CHECK_FALSE(parse_srl(R"({"epoch":"5"})", err).has_value());        // epoch not integer
    CHECK_FALSE(parse_srl(R"({"epoch":-1})", err).has_value());         // negative epoch
    CHECK_FALSE(parse_srl(
        R"({"epoch":1,"revoked_signer_key_fprs":"nope"})", err).has_value());  // not an array

    // A malformed signature hex parses to an empty sig → verification fails closed.
    auto kp = key_from(0x46);
    std::string good = serialize_srl_json(make_srl(1));  // unsigned (empty sig/signer)
    auto parsed = parse_srl(good, err);
    REQUIRE(parsed.has_value());
    auto res = verify_srl(*parsed, kp.public_key);
    CHECK_FALSE(res.ok());  // no signer/sig present → UntrustedSigner
    CHECK(res.status == SrlVerify::UntrustedSigner);
}

TEST_CASE("is_revoked matches signer keys and artifact hashes case-insensitively",
          "[reload][revocation]") {
    auto srl = make_srl(1);  // fprs {deadbeef, cafef00d}, hashes {aa11bb22, cc33dd44}
    CHECK(is_revoked(srl, "deadbeef", ""));
    CHECK(is_revoked(srl, "DEADBEEF", ""));            // case-insensitive
    CHECK(is_revoked(srl, "", "cc33dd44"));
    CHECK(is_revoked(srl, "", "CC33DD44"));
    CHECK(is_revoked(srl, "not-revoked", "aa11bb22")); // artifact hit even if signer misses
    CHECK_FALSE(is_revoked(srl, "unknownkey", "unknownhash"));
    CHECK_FALSE(is_revoked(srl, "", ""));              // empty query never matches
}

TEST_CASE("epoch floor accepts a newer SRL and rejects a rollback", "[reload][revocation]") {
    auto root = make_root("rollback");
    EpochFloorStore store(root);
    const std::string ns = EpochFloorStore::make_namespace("com.pulp.demo", "abcd", "stable");

    // First SRL: no floor yet → accepted, becomes floor 5.
    CHECK(store.accept_srl(make_srl(5), ns) == SrlAcceptance::Accepted);
    // Newer epoch → accepted, floor advances to 6.
    CHECK(store.accept_srl(make_srl(6), ns) == SrlAcceptance::Accepted);
    // Replay the SAME epoch → rollback (defeats un-revoke by replay).
    CHECK(store.accept_srl(make_srl(6), ns) == SrlAcceptance::RejectedRollback);
    // Replay an OLDER epoch → rollback.
    CHECK(store.accept_srl(make_srl(3), ns) == SrlAcceptance::RejectedRollback);

    // Floor persists across a fresh store instance (on-disk, not in-memory).
    EpochFloorStore reopened(root);
    CHECK(reopened.accept_srl(make_srl(6), ns) == SrlAcceptance::RejectedRollback);
    CHECK(reopened.accept_srl(make_srl(7), ns) == SrlAcceptance::Accepted);
}

TEST_CASE("epoch floors are isolated per namespace", "[reload][revocation]") {
    auto root = make_root("namespace-isolation");
    EpochFloorStore store(root);

    // Two plugins, two channels, two trust roots — all distinct namespaces.
    const std::string a  = EpochFloorStore::make_namespace("plugin.A", "key1", "stable");
    const std::string b  = EpochFloorStore::make_namespace("plugin.B", "key1", "stable");
    const std::string a2 = EpochFloorStore::make_namespace("plugin.A", "key1", "beta");   // channel differs
    const std::string a3 = EpochFloorStore::make_namespace("plugin.A", "key2", "stable"); // trust root differs

    // Advance plugin.A/stable to a high floor.
    CHECK(store.accept_srl(make_srl(100), a) == SrlAcceptance::Accepted);

    // Plugin.B is unaffected — a low epoch is still its first SRL.
    CHECK(store.accept_srl(make_srl(1), b) == SrlAcceptance::Accepted);
    // A different channel for the same plugin is a different floor.
    CHECK(store.accept_srl(make_srl(1), a2) == SrlAcceptance::Accepted);
    // A different revocation trust root is a different floor.
    CHECK(store.accept_srl(make_srl(1), a3) == SrlAcceptance::Accepted);

    // And plugin.A/stable still holds its high floor.
    CHECK(store.accept_srl(make_srl(50), a) == SrlAcceptance::RejectedRollback);

    // Guard against key aliasing: ("a","bc") must not collide with ("ab","c").
    CHECK(EpochFloorStore::make_namespace("a", "bc", "x") !=
          EpochFloorStore::make_namespace("ab", "c", "x"));
}

TEST_CASE("offline-last-known SRL is honored (never fail-open)", "[reload][revocation]") {
    auto root = make_root("offline");
    auto kp = key_from(0x55);
    const std::string ns = EpochFloorStore::make_namespace("com.pulp.demo", "abcd", "stable");

    // Publisher issues + we accept an SRL that revokes an artifact.
    auto srl = make_srl(10);
    srl.revoked_artifact_hashes = {"badc0ffee"};
    sign_srl(srl, kp);
    REQUIRE(verify_srl(srl, kp.public_key).ok());

    {
        EpochFloorStore store(root);
        CHECK(store.accept_srl(srl, ns) == SrlAcceptance::Accepted);
    }

    // Later, "offline": no newer SRL fetched. A fresh store loads the last-known
    // SRL from disk and STILL reports the artifact as revoked (fail-closed).
    EpochFloorStore offline_store(root);
    auto last = offline_store.last_known(ns);
    REQUIRE(last.has_value());
    CHECK(last->epoch == 10);
    CHECK(is_revoked(*last, "", "badc0ffee"));
    // The persisted last-known still verifies against the trusted key.
    CHECK(verify_srl(*last, kp.public_key).ok());
}

TEST_CASE("a corrupt floor file is refused, not treated as epoch 0", "[reload][revocation]") {
    auto root = make_root("corrupt");
    EpochFloorStore store(root);
    const std::string ns = EpochFloorStore::make_namespace("com.pulp.demo", "abcd", "stable");

    // Establish a floor, then corrupt the backing file on disk.
    CHECK(store.accept_srl(make_srl(5), ns) == SrlAcceptance::Accepted);
    const auto p = store.path_for(ns);
    REQUIRE(fs::exists(p));
    { std::ofstream(p, std::ios::binary | std::ios::trunc) << "}{ this is not json"; }

    // A corrupt floor must NOT silently reopen the rollback window (epoch 0);
    // it fails closed.
    CHECK(store.read_floor(ns).kind == SrlFloorState::Kind::Corrupt);
    CHECK(store.accept_srl(make_srl(6), ns) == SrlAcceptance::FloorCorrupt);
    CHECK_FALSE(store.last_known(ns).has_value());
}

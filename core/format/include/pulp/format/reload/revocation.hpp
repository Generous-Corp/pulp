#pragma once

/// @file revocation.hpp
/// Signed Revocation List (SRL) + namespaced monotonic epoch floor.
///
/// A swap-pack (see swap_pack.hpp) proves a pack is authentic and unmodified at
/// *install* time. A revocation list is the *kill switch*: after a signer key is
/// compromised or a specific artifact is found malicious, the publisher signs an
/// SRL that names the revoked signer-key fingerprints and/or artifact hashes. The
/// reload gate (the pack-load call site, wired separately) consults
/// `is_revoked()` before honoring a pack, so a previously-trusted-but-now-revoked
/// pack fails closed.
///
/// Two independent gates, mirroring swap_pack's layering:
///   1. `verify_srl()`  — Ed25519 authenticity. Fail-closed on malformed/bad-sig.
///   2. `EpochFloorStore::accept_srl()` — monotonic anti-rollback. An SRL whose
///      `epoch` is <= the highest epoch already seen for its namespace is
///      rejected, so an attacker cannot replay an *older* SRL to "un-revoke" a
///      key/artifact. The floor is namespaced by
///      (plugin_id + revocation_pubkey_fpr + update_channel) so one plugin /
///      channel / trust-root's floor never affects another's.
///
/// Offline-last-known: the store persists the full accepted SRL, not just its
/// epoch. When no newer SRL can be fetched (device offline), the gate loads the
/// last-known SRL from disk and still honors it — revocations never lapse to
/// fail-open just because the network is down.
///
/// Reuses `runtime::ed25519_verify` + `runtime::sha256_hex` — no hand-rolled
/// crypto. Header-only, like swap_pack. Control/main thread only (does file I/O);
/// never call from the audio thread.

#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp::format::reload {

// ── SRL data model ──────────────────────────────────────────────────────────

/// A signed revocation list. `signature` is an Ed25519 detached signature over
/// `srl_signed_message()` — a canonical, JSON-formatting-independent encoding.
struct SignedRevocationList {
    int schema_version = 1;
    std::uint64_t epoch = 0;                            ///< monotonic; higher = newer
    std::string issued_utc;                             ///< ISO-8601 UTC issue time (advisory)
    std::vector<std::string> revoked_signer_key_fprs;   ///< lowercase-hex Ed25519 pubkey fingerprints
    std::vector<std::string> revoked_artifact_hashes;   ///< lowercase-hex sha256 artifact hashes
    std::vector<std::uint8_t> signer_public_key;        ///< 32 bytes (Ed25519), empty if unsigned
    std::vector<std::uint8_t> signature;                ///< 64 bytes (Ed25519 detached), empty if unsigned
};

enum class SrlVerify {
    Ok = 0,
    ParseError,        ///< JSON malformed or a required field missing/mistyped
    UntrustedSigner,   ///< the SRL's signer key is not the trusted revocation key
    BadSignature,      ///< Ed25519 verification failed (or malformed sig/key sizes)
};

struct SrlVerifyResult {
    SrlVerify status = SrlVerify::Ok;
    std::string detail;
    bool ok() const { return status == SrlVerify::Ok; }
};

// ── small helpers ────────────────────────────────────────────────────────────

/// Lowercase a hex/ASCII string for case-insensitive fingerprint/hash compares.
inline std::string srl_to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Decode a hex string to bytes. Returns nullopt on odd length or a non-hex
/// digit (fail-closed — a malformed key/sig never verifies).
inline std::optional<std::vector<std::uint8_t>> srl_hex_decode(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

/// Encode bytes to lowercase hex.
inline std::string srl_hex_encode(const std::vector<std::uint8_t>& bytes) {
    return runtime::hex_encode(bytes);
}

// ── canonical signed message ───────────────────────────────────────────────

/// Deterministic canonical payload the SRL signature covers. Every
/// variable-length field is LENGTH-PREFIXED (u32 LE) and every count is bound,
/// so the encoding is unambiguous — no field value (even one containing the
/// magic or a newline) can be reinterpreted to collide with a different SRL.
/// The signer key + signature fields are NOT bound (Ed25519 already binds the
/// message to the verifying key; a signature cannot cover itself). Fingerprints
/// and hashes are lowercased so sign/verify agree regardless of input case.
///
/// Field order is fixed: magic, schema_version, epoch, issued_utc, then the two
/// revocation lists (count-prefixed). Order MUST match between signer + verifier.
inline std::vector<std::uint8_t> srl_signed_message(const SignedRevocationList& srl) {
    std::vector<std::uint8_t> out;
    auto put_u32 = [&out](std::uint32_t n) {
        out.push_back(static_cast<std::uint8_t>(n & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n >> 24) & 0xFF));
    };
    auto put_u64 = [&out](std::uint64_t n) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((n >> (i * 8)) & 0xFF));
    };
    auto put_field = [&](std::string_view s) {
        put_u32(static_cast<std::uint32_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    };
    put_field("pulp-revocation-list-v1");
    put_u32(static_cast<std::uint32_t>(srl.schema_version));
    put_u64(srl.epoch);
    put_field(srl.issued_utc);
    put_u32(static_cast<std::uint32_t>(srl.revoked_signer_key_fprs.size()));
    for (const auto& fpr : srl.revoked_signer_key_fprs) put_field(srl_to_lower(fpr));
    put_u32(static_cast<std::uint32_t>(srl.revoked_artifact_hashes.size()));
    for (const auto& h : srl.revoked_artifact_hashes) put_field(srl_to_lower(h));
    return out;
}

// ── parse / serialize (JSON wire form) ───────────────────────────────────────

/// Parse an SRL from JSON. Fail-closed (returns nullopt, writes @p error) on any
/// structural problem. Required: integer `epoch`. Optional: `schema_version`
/// (default 1), `issued_utc`, `revoked_signer_key_fprs` / `revoked_artifact_hashes`
/// (arrays of strings, default empty), `signer` / `signature` (hex strings). A
/// malformed hex signer/signature is left empty so verification fails closed
/// rather than parsing to garbage bytes.
inline std::optional<SignedRevocationList> parse_srl(std::string_view json,
                                                     std::string& error) {
    SignedRevocationList srl;
    try {
        auto v = choc::json::parse(std::string(json));
        if (!v.isObject()) { error = "SRL is not a JSON object"; return std::nullopt; }
        if (!v.hasObjectMember("epoch") || !v["epoch"].isInt()) {
            error = "missing/invalid integer 'epoch'"; return std::nullopt;
        }
        const std::int64_t raw_epoch = v["epoch"].getInt64();
        if (raw_epoch < 0) { error = "'epoch' must be non-negative"; return std::nullopt; }
        srl.epoch = static_cast<std::uint64_t>(raw_epoch);

        if (v.hasObjectMember("schema_version") && v["schema_version"].isInt())
            srl.schema_version = static_cast<int>(v["schema_version"].getInt64());
        if (v.hasObjectMember("issued_utc") && v["issued_utc"].isString())
            srl.issued_utc = v["issued_utc"].getString();

        auto read_str_array = [&](const char* key, std::vector<std::string>& dst) -> bool {
            if (!v.hasObjectMember(key)) return true;  // optional
            const auto& arr = v[key];
            if (!arr.isArray()) { error = std::string("'") + key + "' must be an array"; return false; }
            for (uint32_t i = 0; i < arr.size(); ++i) {
                if (!arr[i].isString()) {
                    error = std::string("'") + key + "[" + std::to_string(i) + "]' must be a string";
                    return false;
                }
                dst.push_back(srl_to_lower(std::string(arr[i].getString())));
            }
            return true;
        };
        if (!read_str_array("revoked_signer_key_fprs", srl.revoked_signer_key_fprs)) return std::nullopt;
        if (!read_str_array("revoked_artifact_hashes", srl.revoked_artifact_hashes)) return std::nullopt;

        if (v.hasObjectMember("signer") && v["signer"].isString()) {
            if (auto k = srl_hex_decode(v["signer"].getString())) srl.signer_public_key = std::move(*k);
        }
        if (v.hasObjectMember("signature") && v["signature"].isString()) {
            if (auto s = srl_hex_decode(v["signature"].getString())) srl.signature = std::move(*s);
        }
    } catch (const std::exception& e) {
        error = std::string("SRL JSON parse failed: ") + e.what();
        return std::nullopt;
    }
    return srl;
}

/// Serialize an SRL to a stable, canonical JSON string (fixed field order). Used
/// as the on-disk offline-last-known form; round-trips through `parse_srl`.
inline std::string serialize_srl_json(const SignedRevocationList& srl) {
    auto esc = [](std::string_view s) {
        std::string o;
        o.reserve(s.size() + 2);
        for (char c : s) {
            switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                default:   o += c;      break;
            }
        }
        return o;
    };
    auto str_array = [&](const std::vector<std::string>& xs) {
        std::string o = "[";
        for (std::size_t i = 0; i < xs.size(); ++i) {
            if (i) o += ",";
            o += "\"" + esc(xs[i]) + "\"";
        }
        o += "]";
        return o;
    };
    std::ostringstream os;
    os << "{"
       << "\"schema_version\":" << srl.schema_version << ","
       << "\"epoch\":" << srl.epoch << ","
       << "\"issued_utc\":\"" << esc(srl.issued_utc) << "\","
       << "\"revoked_signer_key_fprs\":" << str_array(srl.revoked_signer_key_fprs) << ","
       << "\"revoked_artifact_hashes\":" << str_array(srl.revoked_artifact_hashes) << ","
       << "\"signer\":\"" << srl_hex_encode(srl.signer_public_key) << "\","
       << "\"signature\":\"" << srl_hex_encode(srl.signature) << "\""
       << "}";
    return os.str();
}

// ── verification ─────────────────────────────────────────────────────────────

/// Verify the SRL's Ed25519 signature against @p trusted_revocation_pubkey.
/// Fail-closed: the signer key must be exactly the trusted revocation key
/// (UntrustedSigner otherwise), and the detached signature must authenticate the
/// canonical message (BadSignature otherwise, including any malformed size).
inline SrlVerifyResult verify_srl(const SignedRevocationList& srl,
                                  const std::vector<std::uint8_t>& trusted_revocation_pubkey) {
    if (srl.signer_public_key.size() != runtime::ed25519_public_key_size ||
        trusted_revocation_pubkey.size() != runtime::ed25519_public_key_size ||
        srl.signer_public_key != trusted_revocation_pubkey) {
        return {SrlVerify::UntrustedSigner, "SRL signer key is not the trusted revocation key"};
    }
    if (srl.signature.size() != runtime::ed25519_signature_size) {
        return {SrlVerify::BadSignature, "signature size invalid"};
    }
    const auto message = srl_signed_message(srl);
    if (!runtime::ed25519_verify(srl.signer_public_key.data(), srl.signer_public_key.size(),
                                 srl.signature.data(), srl.signature.size(),
                                 message.data(), message.size())) {
        return {SrlVerify::BadSignature, "Ed25519 verification failed"};
    }
    return {SrlVerify::Ok, ""};
}

/// Parse + verify in one fail-closed step. A parse failure maps to ParseError.
inline SrlVerifyResult parse_and_verify_srl(std::string_view json,
                                            const std::vector<std::uint8_t>& trusted_revocation_pubkey,
                                            SignedRevocationList& out) {
    std::string err;
    auto parsed = parse_srl(json, err);
    if (!parsed) return {SrlVerify::ParseError, err};
    auto res = verify_srl(*parsed, trusted_revocation_pubkey);
    if (res.ok()) out = std::move(*parsed);
    return res;
}

// ── revocation query ─────────────────────────────────────────────────────────

/// True iff @p signer_key_fpr is a revoked signer OR @p artifact_hash is a
/// revoked artifact in @p srl. Comparison is case-insensitive (lowercase hex).
/// An empty query field is not matched (skips that dimension). Callers should
/// verify the SRL (or load a previously-verified one) before trusting this.
inline bool is_revoked(const SignedRevocationList& srl,
                       std::string_view signer_key_fpr,
                       std::string_view artifact_hash) {
    if (!signer_key_fpr.empty()) {
        const std::string want = srl_to_lower(std::string(signer_key_fpr));
        if (std::find(srl.revoked_signer_key_fprs.begin(),
                      srl.revoked_signer_key_fprs.end(), want) !=
            srl.revoked_signer_key_fprs.end()) {
            return true;
        }
    }
    if (!artifact_hash.empty()) {
        const std::string want = srl_to_lower(std::string(artifact_hash));
        if (std::find(srl.revoked_artifact_hashes.begin(),
                      srl.revoked_artifact_hashes.end(), want) !=
            srl.revoked_artifact_hashes.end()) {
            return true;
        }
    }
    return false;
}

// ── namespaced monotonic epoch floor store ───────────────────────────────────

/// Outcome of offering an SRL to the store.
enum class SrlAcceptance {
    Accepted = 0,      ///< epoch is strictly newer than the floor; SRL persisted as last-known
    RejectedRollback,  ///< epoch <= stored floor — replay/rollback of an older SRL, refused
    FloorCorrupt,      ///< a floor file exists but is unreadable/garbage — refuse (fail-closed)
    PersistError,      ///< could not atomically write the new last-known SRL
};

/// State of the persisted floor for a namespace.
struct SrlFloorState {
    enum class Kind { Absent, Present, Corrupt } kind = Kind::Absent;
    std::uint64_t epoch = 0;                         ///< valid only when kind == Present
    std::optional<SignedRevocationList> last_known;  ///< the stored SRL when kind == Present
};

/// Directory-backed store of the highest SRL epoch SEEN, namespaced by
/// (plugin_id + revocation_pubkey_fpr + update_channel). Each namespace maps to
/// one file named by the sha256 of a length-prefixed key (so plugin_id / channel
/// strings can never collide or escape the directory via path characters). The
/// file holds the full canonical SRL JSON — both the epoch floor AND the
/// offline-last-known revocation content live in it.
///
/// Writes are atomic (temp file + rename). All methods are control-thread only.
class EpochFloorStore {
public:
    explicit EpochFloorStore(std::filesystem::path root) : root_(std::move(root)) {}

    /// Canonical namespace key: length-prefixed so ("a","bc",...) can't collide
    /// with ("ab","c",...). The revocation pubkey fingerprint is lowercased.
    static std::string make_namespace(std::string_view plugin_id,
                                      std::string_view revocation_pubkey_fpr,
                                      std::string_view update_channel) {
        auto put = [](std::string& o, std::string_view s) {
            o += std::to_string(s.size());
            o += ':';
            o.append(s.begin(), s.end());
        };
        std::string key;
        put(key, plugin_id);
        put(key, srl_to_lower(std::string(revocation_pubkey_fpr)));
        put(key, update_channel);
        return key;
    }

    /// Absolute path of the file backing a namespace.
    std::filesystem::path path_for(std::string_view namespace_key) const {
        return root_ / (runtime::sha256_hex(namespace_key) + ".srl.json");
    }

    /// Read the persisted floor for a namespace. A missing file → Absent
    /// (first-ever SRL, no floor to violate). A present-but-unparseable file →
    /// Corrupt (fail-closed — never silently treated as epoch 0, which would
    /// re-open the rollback window).
    SrlFloorState read_floor(std::string_view namespace_key) const {
        const auto p = path_for(namespace_key);
        std::error_code ec;
        if (!std::filesystem::exists(p, ec) || ec) return {};  // Absent
        std::ifstream in(p, std::ios::binary);
        if (!in) return {SrlFloorState::Kind::Corrupt, 0, std::nullopt};
        std::string contents((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        if (in.bad()) return {SrlFloorState::Kind::Corrupt, 0, std::nullopt};
        std::string err;
        auto parsed = parse_srl(contents, err);
        if (!parsed) return {SrlFloorState::Kind::Corrupt, 0, std::nullopt};
        return {SrlFloorState::Kind::Present, parsed->epoch, std::move(parsed)};
    }

    /// The last-known accepted SRL for a namespace, if any (offline-last-known).
    /// Returns nullopt when Absent or Corrupt.
    std::optional<SignedRevocationList> last_known(std::string_view namespace_key) const {
        auto st = read_floor(namespace_key);
        if (st.kind == SrlFloorState::Kind::Present) return st.last_known;
        return std::nullopt;
    }

    /// Offer a (already signature-verified) SRL for the given namespace. Accepts
    /// only if its epoch is STRICTLY greater than the stored floor; an epoch <=
    /// the floor is a rollback and is refused. On acceptance the SRL is persisted
    /// atomically as the new last-known. A corrupt existing floor is refused
    /// (fail-closed) rather than overwritten.
    SrlAcceptance accept_srl(const SignedRevocationList& srl,
                             std::string_view plugin_id,
                             std::string_view revocation_pubkey_fpr,
                             std::string_view update_channel) {
        return accept_srl(srl, make_namespace(plugin_id, revocation_pubkey_fpr, update_channel));
    }

    /// Overload taking a pre-computed namespace key.
    SrlAcceptance accept_srl(const SignedRevocationList& srl, std::string_view namespace_key) {
        auto st = read_floor(namespace_key);
        if (st.kind == SrlFloorState::Kind::Corrupt) return SrlAcceptance::FloorCorrupt;
        if (st.kind == SrlFloorState::Kind::Present && srl.epoch <= st.epoch) {
            return SrlAcceptance::RejectedRollback;
        }
        return persist(srl, namespace_key) ? SrlAcceptance::Accepted
                                           : SrlAcceptance::PersistError;
    }

private:
    /// Atomic persist: write to a temp file, fsync via close, then rename over
    /// the target. rename() on the same filesystem is atomic, so a crash leaves
    /// either the old floor or the new one — never a truncated file.
    bool persist(const SignedRevocationList& srl, std::string_view namespace_key) {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec) return false;
        const auto target = path_for(namespace_key);
        const auto tmp = std::filesystem::path(target).concat(".tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << serialize_srl_json(srl);
            out.flush();
            if (!out) { std::filesystem::remove(tmp, ec); return false; }
        }
        std::filesystem::rename(tmp, target, ec);
        if (ec) { std::error_code rmec; std::filesystem::remove(tmp, rmec); return false; }
        return true;
    }

    std::filesystem::path root_;
};

}  // namespace pulp::format::reload

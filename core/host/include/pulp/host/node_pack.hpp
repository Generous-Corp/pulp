// pulp/host/node_pack.hpp — load signed, precompiled custom-node packs.
//
// A "node pack" is a dynamic library (.dylib / .so / .dll) exporting the public
// `pulp_node_v1_entry` symbol, plus a JSON manifest that declares the pack's
// identity, ABI major, the binary's SHA-256, node metadata, resource/runtime
// requirements, and an Ed25519 signature by a trusted publisher key. The loader
// verifies trust and host policy BEFORE it ever dlopen()s the binary: untrusted
// signer, bad signature, unsupported requirements, hash mismatch, or ABI
// mismatch all reject the pack without loading code.
//
// Desktop + Android only. iOS / AUv3 / sandboxed targets do NOT do dynamic
// loading (App Store dlopen policy): native components there are source-built,
// statically linked, and signed as part of the app — this whole translation
// unit is compiled out with `core/host` on iOS.
#pragma once

#include <pulp/native_components/pulp_node_v1.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::host {

// One node declared by a pack manifest (for host-side discovery before load).
struct NodePackEntry {
    std::string type_id;
    std::uint32_t capabilities = 0;
};

struct NodePackResource {
    std::string id;
    std::string kind;
    std::string sha256_hex;
    bool required = true;
};

struct NodePackRuntimeRequirements {
    bool realtime_safe = true;
    bool audio_thread_allocations = false;
    std::uint32_t max_block_size = 0;  // 0 means unspecified by the pack
    std::uint64_t persistent_bytes = 0;
    std::uint64_t scratch_bytes = 0;
};

// Parsed, not-yet-verified manifest.
struct NodePackManifest {
    std::string pack_id;
    std::uint32_t abi_major = 0;
    std::string binary;                  // filename relative to the manifest dir
    std::string sha256_hex;              // expected SHA-256 of the binary
    std::vector<std::uint8_t> signer_public_key;  // 32 bytes (Ed25519)
    std::vector<std::uint8_t> signature;          // 64 bytes (Ed25519, detached)
    std::vector<NodePackEntry> nodes;
    std::vector<NodePackResource> resources;
    NodePackRuntimeRequirements requirements;
};

// Trust policy: the set of publisher public keys the host accepts. A pack signed
// by a key not in this set is rejected (this is where revocation lives — drop a
// key to revoke it).
struct NodePackTrust {
    std::vector<std::vector<std::uint8_t>> trusted_public_keys;  // each 32 bytes
};

struct NodePackHostPolicy {
    std::uint32_t supported_capabilities =
        PULP_NODE_CAP_STATE_V1 | PULP_NODE_CAP_RESET_V1 |
        PULP_NODE_CAP_EVENTS_V1 | PULP_NODE_CAP_LATENCY_V1;
    bool require_realtime_safe = true;
    bool allow_audio_thread_allocations = false;
    std::uint32_t max_block_size = 0;  // 0 means unlimited/host-specific
    std::uint64_t max_persistent_bytes = 0;
    std::uint64_t max_scratch_bytes = 0;
};

enum class NodePackError {
    Ok = 0,
    ManifestInvalid,   // unparseable / missing fields / wrong key sizes
    UntrustedSigner,   // signer_public_key not in the trust set
    BadSignature,      // Ed25519 verification failed
    UnsupportedRequirements, // capabilities / RT / resource needs unsupported
    HashMismatch,      // binary's SHA-256 != manifest sha256_hex
    AbiMismatch,       // manifest or entry abi_major != PULP_NODE_V1_ABI_MAJOR
    NodeMetadataMismatch, // manifest node declarations != loaded descriptor
    LoadFailed,        // dlopen/LoadLibrary failed
    SymbolMissing,     // no pulp_node_v1_entry export
};

struct NodePackLoadResult {
    NodePackError error = NodePackError::ManifestInvalid;
    const pulp_node_entry_v1* entry = nullptr;  // valid only when error == Ok
    void* handle = nullptr;                     // opaque dl handle; pass to unload
    bool ok() const { return error == NodePackError::Ok; }
};

// Parse a manifest JSON string. Returns false (and leaves `out` unspecified) on
// malformed input or wrong key/signature sizes.
bool parse_node_pack_manifest(const std::string& json, NodePackManifest& out);

// The canonical byte string the manifest signature covers: binds pack identity,
// ABI major, binary hash, declared node type-ids/capabilities, resources, and
// runtime requirements. Publishers sign exactly these bytes.
std::vector<std::uint8_t> node_pack_signed_message(const NodePackManifest& m);

// Verify trust + integrity, then load the binary and resolve the node entry.
// `manifest_dir` is the directory containing the manifest and the binary. No
// code is loaded unless the signer is trusted, the signature is authentic, and
// the on-disk binary matches the signed hash.
NodePackLoadResult load_node_pack(const std::string& manifest_dir,
                                  const NodePackManifest& manifest,
                                  const NodePackTrust& trust);
NodePackLoadResult load_node_pack(const std::string& manifest_dir,
                                  const NodePackManifest& manifest,
                                  const NodePackTrust& trust,
                                  const NodePackHostPolicy& policy);

// Unload a previously loaded pack (dlclose/FreeLibrary). Safe on nullptr.
void unload_node_pack(void* handle);

}  // namespace pulp::host

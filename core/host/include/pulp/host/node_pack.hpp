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
#include <memory>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace pulp::host {

enum class GeneratedDevicePlatform : std::uint8_t {
    Desktop,
    Android,
    Ios,
    Web,
};

enum class GeneratedDeviceArtifact : std::uint8_t {
    DeclarativeGraph,
    InterpretedProgram,
    NativePack,
    WebAssembly,
    FrozenAudio,
    Recipe,
    GeneratedUi,
};

enum class GeneratedDeviceOrigin : std::uint8_t {
    AppBundled,
    RuntimeDownloaded,
};

struct GeneratedDeviceAdmission {
    bool allowed = false;
    bool requires_separate_origin = false;
    bool universal_fallback_part = false;
};

[[nodiscard]] constexpr GeneratedDevicePlatform build_generated_device_platform() noexcept {
#if defined(__ANDROID__)
    return GeneratedDevicePlatform::Android;
#elif defined(__EMSCRIPTEN__)
    return GeneratedDevicePlatform::Web;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return GeneratedDevicePlatform::Ios;
#else
    return GeneratedDevicePlatform::Desktop;
#endif
}

/// Returns the platform policy before any executable artifact is opened.
/// Frozen audio plus its recipe is the portable fallback on every platform.
[[nodiscard]] constexpr GeneratedDeviceAdmission
generated_device_admission(GeneratedDevicePlatform platform,
                           GeneratedDeviceArtifact artifact,
                           GeneratedDeviceOrigin origin) noexcept {
    if (platform < GeneratedDevicePlatform::Desktop ||
        platform > GeneratedDevicePlatform::Web ||
        artifact < GeneratedDeviceArtifact::DeclarativeGraph ||
        artifact > GeneratedDeviceArtifact::GeneratedUi ||
        (origin != GeneratedDeviceOrigin::AppBundled &&
         origin != GeneratedDeviceOrigin::RuntimeDownloaded)) {
        return {};
    }
    if (artifact == GeneratedDeviceArtifact::FrozenAudio ||
        artifact == GeneratedDeviceArtifact::Recipe) {
        return {true, false, true};
    }
    if (artifact == GeneratedDeviceArtifact::DeclarativeGraph) {
        return {true, false, false};
    }
    if (artifact == GeneratedDeviceArtifact::InterpretedProgram) {
        const bool supported = platform == GeneratedDevicePlatform::Desktop ||
                               platform == GeneratedDevicePlatform::Ios ||
                               platform == GeneratedDevicePlatform::Web;
        return {supported, false, false};
    }
    if (artifact == GeneratedDeviceArtifact::GeneratedUi) {
        const bool supported = platform == GeneratedDevicePlatform::Desktop ||
                               platform == GeneratedDevicePlatform::Web;
        return {supported, supported, false};
    }
    if (artifact == GeneratedDeviceArtifact::WebAssembly) {
        const bool supported = platform == GeneratedDevicePlatform::Web;
        return {supported, false, false};
    }
    if (artifact == GeneratedDeviceArtifact::NativePack) {
        const bool supported = platform == GeneratedDevicePlatform::Desktop ||
                               (platform == GeneratedDevicePlatform::Android &&
                                origin == GeneratedDeviceOrigin::AppBundled);
        return {supported, false, false};
    }
    return {};
}

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
    GeneratedDevicePlatform platform = build_generated_device_platform();
    GeneratedDeviceOrigin origin = GeneratedDeviceOrigin::RuntimeDownloaded;
};

enum class NodePackError {
    Ok = 0,
    ManifestInvalid = 1,         // unparseable / missing fields / wrong key sizes
    UntrustedSigner = 2,         // signer_public_key not in the trust set
    BadSignature = 3,            // Ed25519 verification failed
    HashMismatch = 4,            // binary's SHA-256 != manifest sha256_hex
    AbiMismatch = 5,             // manifest or entry abi_major != PULP_NODE_V1_ABI_MAJOR
    LoadFailed = 6,              // dlopen/LoadLibrary failed
    SymbolMissing = 7,           // no pulp_node_v1_entry export
    UnsupportedRequirements = 8, // capabilities / RT / resource needs unsupported
    NodeMetadataMismatch = 9,    // manifest node declarations != loaded descriptor
    PlatformPolicyDenied = 10,   // native artifact is forbidden on this platform/origin
    BinaryChanged = 11,          // the mapped module is not the file whose bytes were verified
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

// The pack binary, opened once and hashed from that open file. The load stage
// maps exactly these bytes, so a file swapped at the path after verification
// is never the code that runs:
//   - macOS / Linux: the dynamic loader is handed the descriptor path
//     (/dev/fd/N, /proc/self/fd/N), which the kernel resolves to this open file
//     instead of walking the pack directory again. dyld maps the inode behind
//     the descriptor even when the directory entry already points elsewhere.
//   - Windows: the file is held open without FILE_SHARE_WRITE / FILE_SHARE_DELETE
//     so the path cannot be rewritten or replaced while it is held, and the
//     mapped module's volume + file index are compared with this handle after
//     LoadLibrary; a mismatch unloads it and reports BinaryChanged.
// The file stays open for as long as the pack is loaded; unload_node_pack
// releases it.
struct NodePackVerifyResult;

class VerifiedNodePackBinary {
  public:
    VerifiedNodePackBinary();
    ~VerifiedNodePackBinary();
    VerifiedNodePackBinary(VerifiedNodePackBinary&&) noexcept;
    VerifiedNodePackBinary& operator=(VerifiedNodePackBinary&&) noexcept;
    VerifiedNodePackBinary(const VerifiedNodePackBinary&) = delete;
    VerifiedNodePackBinary& operator=(const VerifiedNodePackBinary&) = delete;

    bool is_open() const;
    // Path that was opened (manifest_dir + "/" + manifest.binary).
    const std::string& path() const;
    // SHA-256 of the bytes actually read from the open file.
    const std::string& sha256_hex() const;

    // Opaque on-disk handle; the loader's platform helpers operate on it.
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;

    friend NodePackVerifyResult verify_node_pack_binary(const std::string&,
                                                        const NodePackManifest&);
    friend NodePackLoadResult load_verified_node_pack(VerifiedNodePackBinary,
                                                      const NodePackManifest&, const NodePackTrust&,
                                                      const NodePackHostPolicy&);
};

struct NodePackVerifyResult {
    // Ok, LoadFailed (cannot open, or not a regular file) or HashMismatch.
    NodePackError error = NodePackError::LoadFailed;
    VerifiedNodePackBinary binary; // open only when error == Ok
};

// Stage one of loading: open the pack binary once and compare the hash of the
// bytes read from that open file with the manifest's signed hash. Does not
// check trust or policy; load_verified_node_pack re-checks both.
NodePackVerifyResult verify_node_pack_binary(const std::string& manifest_dir,
                                             const NodePackManifest& manifest);

// Stage two: re-run the manifest trust and policy checks, then map the bytes
// behind `binary` and resolve the node entry. Takes ownership of the open
// file; it is released on any failure and otherwise by unload_node_pack.
NodePackLoadResult load_verified_node_pack(VerifiedNodePackBinary binary,
                                           const NodePackManifest& manifest,
                                           const NodePackTrust& trust,
                                           const NodePackHostPolicy& policy);

// Verify trust + integrity, then load the binary and resolve the node entry.
// `manifest_dir` is the directory containing the manifest and the binary. No
// code is loaded unless the signer is trusted, the signature is authentic, and
// the binary matches the signed hash; the two stages above are composed so the
// bytes that were hashed are the bytes that are mapped.
NodePackLoadResult load_node_pack(const std::string& manifest_dir,
                                  const NodePackManifest& manifest,
                                  const NodePackTrust& trust);
NodePackLoadResult load_node_pack(const std::string& manifest_dir,
                                  const NodePackManifest& manifest,
                                  const NodePackTrust& trust,
                                  const NodePackHostPolicy& policy);

// Unload a previously loaded pack (dlclose/FreeLibrary) and release the open
// file it was mapped from. Safe on nullptr.
void unload_node_pack(void* handle);

}  // namespace pulp::host

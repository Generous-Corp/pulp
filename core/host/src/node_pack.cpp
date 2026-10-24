#include <pulp/host/node_pack.hpp>

#include <pulp/native_components/pulp_node_v1.h>
#include <pulp/native_components/pulp_node_v1.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::host {
namespace {

std::vector<std::uint8_t> hex_decode(const std::string& hex) {
    std::vector<std::uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]);
        const int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

void* dl_sym(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
}
void dl_close(void* handle) {
#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

bool key_is_trusted(const std::vector<std::uint8_t>& key,
                    const NodePackTrust& trust) {
    for (const auto& k : trust.trusted_public_keys) {
        if (k.size() == key.size() && !k.empty() &&
            std::equal(k.begin(), k.end(), key.begin())) {
            return true;
        }
    }
    return false;
}

bool has_valid_node_declarations(const std::vector<NodePackEntry>& nodes) {
    if (nodes.empty()) return false;
    for (const auto& node : nodes) {
        if (node.type_id.empty()) return false;
    }
    return true;
}

bool has_valid_resource_declarations(const std::vector<NodePackResource>& resources) {
    for (const auto& resource : resources) {
        if (resource.id.empty() || resource.kind.empty()) return false;
        if (resource.required && resource.sha256_hex.empty()) return false;
    }
    return true;
}

bool requirements_are_supported(const NodePackManifest& manifest,
                                const NodePackHostPolicy& policy) {
    for (const auto& node : manifest.nodes) {
        if ((node.capabilities & ~policy.supported_capabilities) != 0) {
            return false;
        }
    }

    const auto& req = manifest.requirements;
    if (policy.require_realtime_safe && !req.realtime_safe) return false;
    if (!policy.allow_audio_thread_allocations && req.audio_thread_allocations) {
        return false;
    }
    if (policy.max_block_size != 0 && req.max_block_size > policy.max_block_size) {
        return false;
    }
    if (policy.max_persistent_bytes != 0 &&
        req.persistent_bytes > policy.max_persistent_bytes) {
        return false;
    }
    if (policy.max_scratch_bytes != 0 &&
        req.scratch_bytes > policy.max_scratch_bytes) {
        return false;
    }
    return true;
}

bool manifest_declares_descriptor(const NodePackManifest& manifest,
                                  const pulp_node_descriptor_v1* descriptor) {
    if (descriptor == nullptr || descriptor->stable_id == nullptr
        || descriptor->stable_id_len == 0) {
        return false;
    }

    const std::string stable_id(
        descriptor->stable_id,
        descriptor->stable_id + descriptor->stable_id_len);
    for (const auto& node : manifest.nodes) {
        if (node.type_id == stable_id
            && node.capabilities == descriptor->capability_flags) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool parse_node_pack_manifest(const std::string& json, NodePackManifest& out) {
    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (...) {
        return false;  // never unwind out of the loader
    }
    if (!root.isObject()) return false;
    auto get_str = [](const auto& v, const char* k) -> std::string {
        return v.hasObjectMember(k) ? std::string(v[k].getString())
                                    : std::string();
    };
    auto get_u32 = [](const auto& v, const char* k) -> std::uint32_t {
        return v.hasObjectMember(k)
                   ? static_cast<std::uint32_t>(v[k].getInt64())
                   : 0u;
    };
    auto get_u64 = [](const auto& v, const char* k) -> std::uint64_t {
        if (!v.hasObjectMember(k)) return 0u;
        const auto raw = v[k].getInt64();
        if (raw < 0) return std::numeric_limits<std::uint64_t>::max();
        return static_cast<std::uint64_t>(raw);
    };
    auto get_bool = [](const auto& v, const char* k, bool fallback) -> bool {
        return v.hasObjectMember(k) ? v[k].getBool() : fallback;
    };
    out.pack_id = get_str(root, "pack_id");
    out.abi_major = get_u32(root, "abi_major");
    out.binary = get_str(root, "binary");
    out.sha256_hex = get_str(root, "sha256");
    out.signer_public_key = hex_decode(get_str(root, "signer_public_key"));
    out.signature = hex_decode(get_str(root, "signature"));
    out.nodes.clear();
    out.resources.clear();
    out.requirements = {};
    if (root.hasObjectMember("nodes") && root["nodes"].isArray()) {
        const auto& arr = root["nodes"];
        for (uint32_t i = 0; i < arr.size(); ++i) {
            NodePackEntry e;
            e.type_id = get_str(arr[i], "type_id");
            e.capabilities = get_u32(arr[i], "capabilities");
            out.nodes.push_back(std::move(e));
        }
    }
    if (root.hasObjectMember("resources") && root["resources"].isArray()) {
        const auto& arr = root["resources"];
        for (uint32_t i = 0; i < arr.size(); ++i) {
            NodePackResource r;
            r.id = get_str(arr[i], "id");
            r.kind = get_str(arr[i], "kind");
            r.sha256_hex = get_str(arr[i], "sha256");
            r.required = get_bool(arr[i], "required", true);
            out.resources.push_back(std::move(r));
        }
    }
    if (root.hasObjectMember("requirements") &&
        root["requirements"].isObject()) {
        const auto& req = root["requirements"];
        out.requirements.realtime_safe = get_bool(req, "realtime_safe", true);
        out.requirements.audio_thread_allocations =
            get_bool(req, "audio_thread_allocations", false);
        out.requirements.max_block_size = get_u32(req, "max_block_size");
        out.requirements.persistent_bytes = get_u64(req, "persistent_bytes");
        out.requirements.scratch_bytes = get_u64(req, "scratch_bytes");
    }
    // Structural validation: required fields + exact Ed25519 key/sig sizes.
    if (out.pack_id.empty() || out.binary.empty() || out.sha256_hex.empty())
        return false;
    if (!has_valid_node_declarations(out.nodes)) return false;
    if (!has_valid_resource_declarations(out.resources)) return false;
    if (out.signer_public_key.size() != runtime::ed25519_public_key_size)
        return false;
    if (out.signature.size() != runtime::ed25519_signature_size) return false;
    return true;
}

std::vector<std::uint8_t> node_pack_signed_message(const NodePackManifest& m) {
    // Binds pack identity, ABI major, binary hash, and load-relevant metadata.
    // Deterministic and independent of JSON formatting, so re-serialization
    // can't break the sig.
    std::string s = "pulp-node-pack-v1\n";
    s += m.pack_id;
    s += '\n';
    s += std::to_string(m.abi_major);
    s += '\n';
    s += m.sha256_hex;
    s += '\n';
    for (const auto& node : m.nodes) {
        s += node.type_id;
        s += '\n';
        s += std::to_string(node.capabilities);
        s += '\n';
    }
    s += "resources\n";
    for (const auto& resource : m.resources) {
        s += resource.id;
        s += '\n';
        s += resource.kind;
        s += '\n';
        s += resource.sha256_hex;
        s += '\n';
        s += resource.required ? "1\n" : "0\n";
    }
    s += "requirements\n";
    s += m.requirements.realtime_safe ? "1\n" : "0\n";
    s += m.requirements.audio_thread_allocations ? "1\n" : "0\n";
    s += std::to_string(m.requirements.max_block_size);
    s += '\n';
    s += std::to_string(m.requirements.persistent_bytes);
    s += '\n';
    s += std::to_string(m.requirements.scratch_bytes);
    s += '\n';
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

namespace {

// Trust + policy checks that need only the manifest. Run before the binary
// is opened and again by load_verified_node_pack so the second stage is never
// a bypass.
NodePackError check_manifest_admission(const NodePackManifest& manifest,
                                       const NodePackTrust& trust,
                                       const NodePackHostPolicy& policy) {
    if (manifest.abi_major != PULP_NODE_V1_ABI_MAJOR) return NodePackError::AbiMismatch;
    if (manifest.signer_public_key.size() != runtime::ed25519_public_key_size ||
        manifest.signature.size() != runtime::ed25519_signature_size ||
        !has_valid_node_declarations(manifest.nodes) ||
        !has_valid_resource_declarations(manifest.resources)) {
        return NodePackError::ManifestInvalid;
    }
    if (!generated_device_admission(policy.platform, GeneratedDeviceArtifact::NativePack,
                                    policy.origin)
             .allowed) {
        return NodePackError::PlatformPolicyDenied;
    }
    if (!key_is_trusted(manifest.signer_public_key, trust)) return NodePackError::UntrustedSigner;
    const std::vector<std::uint8_t> message = node_pack_signed_message(manifest);
    if (!runtime::ed25519_verify(
            manifest.signer_public_key.data(), manifest.signer_public_key.size(),
            manifest.signature.data(), manifest.signature.size(),
            message.data(), message.size())) {
        return NodePackError::BadSignature;
    }
    if (!requirements_are_supported(manifest, policy)) return NodePackError::UnsupportedRequirements;
    return NodePackError::Ok;
}

}  // namespace

// ── Verified binary: open once, hash from the open file, map that file ──────

struct VerifiedNodePackBinary::Impl {
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    bool is_open() const { return file != INVALID_HANDLE_VALUE; }
    void close() {
        if (is_open()) ::CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
    }
#else
    int fd = -1;
    bool is_open() const { return fd >= 0; }
    void close() {
        if (is_open()) ::close(fd);
        fd = -1;
    }
#endif
    std::string path;
    std::string sha256_hex;
    ~Impl() { close(); }
};

namespace {

using BinaryImpl = VerifiedNodePackBinary::Impl;

// Opens `path` read-only and keeps the descriptor; refuses anything that is
// not a regular file. On Windows the open shares reads only, so the file can
// be neither rewritten nor renamed away while the handle is held.
bool open_regular_file(BinaryImpl& impl, const std::string& path) {
#if defined(_WIN32)
    impl.file = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl.file == INVALID_HANDLE_VALUE) return false;
    if (::GetFileType(impl.file) != FILE_TYPE_DISK) {
        impl.close();
        return false;
    }
    return true;
#else
    impl.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (impl.fd < 0) return false;
    struct stat st{};
    if (::fstat(impl.fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        impl.close();
        return false;
    }
    return true;
#endif
}

// Reads the whole open file. The bytes come from the descriptor, never from a
// second walk of the path.
bool read_open_file(BinaryImpl& impl, std::vector<std::uint8_t>& out) {
    out.clear();
    std::uint8_t chunk[64 * 1024];
#if defined(_WIN32)
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(impl.file, chunk, static_cast<DWORD>(sizeof(chunk)), &got, nullptr)) return false;
        if (got == 0) break;
        out.insert(out.end(), chunk, chunk + got);
    }
    return true;
#else
    for (;;) {
        const ssize_t got = ::read(impl.fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) break;
        out.insert(out.end(), chunk, chunk + got);
    }
    return true;
#endif
}

#if defined(_WIN32)
bool same_file_identity(HANDLE a, HANDLE b) {
    BY_HANDLE_FILE_INFORMATION ia{}, ib{};
    if (!::GetFileInformationByHandle(a, &ia) || !::GetFileInformationByHandle(b, &ib)) return false;
    return ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
           ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
}
#endif

// Maps the verified file. Sets `error` and returns nullptr on failure.
void* dl_open_verified(BinaryImpl& impl, NodePackError& error) {
#if defined(_WIN32)
    // The held handle blocks writes and renames at `path`, so LoadLibrary maps
    // the same file; the identity check below proves it rather than assuming.
    HMODULE module = ::LoadLibraryA(impl.path.c_str());
    if (module == nullptr) {
        error = NodePackError::LoadFailed;
        return nullptr;
    }
    char module_path[MAX_PATH * 4] = {};
    const DWORD len = ::GetModuleFileNameA(module, module_path, static_cast<DWORD>(sizeof(module_path)));
    bool same = false;
    if (len > 0 && len < sizeof(module_path)) {
        HANDLE mapped = ::CreateFileA(module_path, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mapped != INVALID_HANDLE_VALUE) {
            same = same_file_identity(impl.file, mapped);
            ::CloseHandle(mapped);
        }
    }
    if (!same) {
        ::FreeLibrary(module);
        error = NodePackError::BinaryChanged;
        return nullptr;
    }
    return reinterpret_cast<void*>(module);
#else
    char fd_path[64] = {};
#if defined(__APPLE__)
    // dyld opens this path, which devfs resolves to the held descriptor's
    // vnode, and keys the load on that inode: a different file now sitting at
    // the directory entry is not what gets mapped (measured, not assumed).
    std::snprintf(fd_path, sizeof(fd_path), "/dev/fd/%d", impl.fd);
#else
    std::snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", impl.fd);
    // glibc keys already-loaded objects by the name string they were opened
    // with. The descriptor stays open while a pack is loaded, so a live pack
    // never shares this name; an object dlclose() could not unload still can.
    // Refuse that stale alias instead of handing back the wrong image.
    if (void* probe = ::dlopen(fd_path, RTLD_NOW | RTLD_NOLOAD)) {
        bool other_name = false;
        if (void* sym = ::dlsym(probe, "pulp_node_v1_entry")) {
            Dl_info info{};
            if (::dladdr(sym, &info) != 0 && info.dli_fname != nullptr &&
                std::strcmp(info.dli_fname, fd_path) != 0) {
                other_name = true;  // the same inode, loaded under another path
            }
        }
        ::dlclose(probe);
        if (!other_name) {
            error = NodePackError::BinaryChanged;
            return nullptr;
        }
    }
#endif
    void* handle = ::dlopen(fd_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) error = NodePackError::LoadFailed;
    return handle;
#endif
}

// Open files behind loaded packs, released by unload_node_pack. A pack loaded
// twice holds two entries, matching the loader's own reference count.
std::mutex& loaded_binaries_mutex() {
    static std::mutex m;
    return m;
}
std::vector<std::pair<void*, VerifiedNodePackBinary>>& loaded_binaries() {
    static std::vector<std::pair<void*, VerifiedNodePackBinary>> v;
    return v;
}

}  // namespace

VerifiedNodePackBinary::VerifiedNodePackBinary() : impl_(std::make_unique<Impl>()) {}
VerifiedNodePackBinary::~VerifiedNodePackBinary() = default;
VerifiedNodePackBinary::VerifiedNodePackBinary(VerifiedNodePackBinary&&) noexcept = default;
VerifiedNodePackBinary& VerifiedNodePackBinary::operator=(VerifiedNodePackBinary&&) noexcept = default;

bool VerifiedNodePackBinary::is_open() const { return impl_ && impl_->is_open(); }
const std::string& VerifiedNodePackBinary::path() const {
    static const std::string empty;
    return impl_ ? impl_->path : empty;
}
const std::string& VerifiedNodePackBinary::sha256_hex() const {
    static const std::string empty;
    return impl_ ? impl_->sha256_hex : empty;
}

NodePackVerifyResult verify_node_pack_binary(const std::string& manifest_dir,
                                             const NodePackManifest& manifest) {
    NodePackVerifyResult r;
    auto& impl = *r.binary.impl_;
    impl.path = manifest_dir + "/" + manifest.binary;
    std::vector<std::uint8_t> bytes;
    if (!open_regular_file(impl, impl.path) || !read_open_file(impl, bytes) || bytes.empty()) {
        impl.close();
        r.error = NodePackError::LoadFailed;
        return r;
    }
    impl.sha256_hex = runtime::sha256_hex(bytes.data(), bytes.size());
    if (impl.sha256_hex != manifest.sha256_hex) {
        impl.close();
        r.error = NodePackError::HashMismatch;
        return r;
    }
    r.error = NodePackError::Ok;
    return r;
}

NodePackLoadResult load_verified_node_pack(VerifiedNodePackBinary binary,
                                           const NodePackManifest& manifest,
                                           const NodePackTrust& trust,
                                           const NodePackHostPolicy& policy) {
    NodePackLoadResult r;
    r.error = check_manifest_admission(manifest, trust, policy);
    if (r.error != NodePackError::Ok) return r;
    if (!binary.is_open()) {
        r.error = NodePackError::LoadFailed;
        return r;
    }
    // A binary verified against some other manifest does not satisfy this one.
    if (binary.sha256_hex() != manifest.sha256_hex) {
        r.error = NodePackError::HashMismatch;
        return r;
    }

    NodePackError load_error = NodePackError::LoadFailed;
    void* handle = dl_open_verified(*binary.impl_, load_error);
    if (handle == nullptr) {
        r.error = load_error;
        return r;
    }
    using entry_fn = const pulp_node_entry_v1* (*)(void);
    auto* sym = reinterpret_cast<entry_fn>(dl_sym(handle, "pulp_node_v1_entry"));
    if (sym == nullptr) {
        dl_close(handle);
        r.error = NodePackError::SymbolMissing;
        return r;
    }
    const pulp_node_entry_v1* entry = sym();
    if (!native_components::node_is_compatible(entry)) {
        dl_close(handle);
        r.error = NodePackError::AbiMismatch;
        return r;
    }
    if (!manifest_declares_descriptor(manifest, entry->descriptor())) {
        dl_close(handle);
        r.error = NodePackError::NodeMetadataMismatch;
        return r;
    }

    {
        std::lock_guard<std::mutex> lock(loaded_binaries_mutex());
        loaded_binaries().emplace_back(handle, std::move(binary));
    }
    r.error = NodePackError::Ok;
    r.entry = entry;
    r.handle = handle;
    return r;
}

NodePackLoadResult load_node_pack(const std::string& manifest_dir,
                                  const NodePackManifest& manifest,
                                  const NodePackTrust& trust) {
    return load_node_pack(manifest_dir, manifest, trust, NodePackHostPolicy{});
}

NodePackLoadResult load_node_pack(const std::string& manifest_dir,
                                  const NodePackManifest& manifest,
                                  const NodePackTrust& trust,
                                  const NodePackHostPolicy& policy) {
    // Only an admitted manifest gets its binary opened at all.
    NodePackLoadResult r;
    r.error = check_manifest_admission(manifest, trust, policy);
    if (r.error != NodePackError::Ok) return r;

    NodePackVerifyResult verified = verify_node_pack_binary(manifest_dir, manifest);
    if (verified.error != NodePackError::Ok) {
        r.error = verified.error;
        return r;
    }
    return load_verified_node_pack(std::move(verified.binary), manifest, trust, policy);
}

void unload_node_pack(void* handle) {
    if (handle == nullptr) return;
    dl_close(handle);
    std::lock_guard<std::mutex> lock(loaded_binaries_mutex());
    auto& entries = loaded_binaries();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->first == handle) {
            entries.erase(it);  // releases the open file
            return;
        }
    }
}

}  // namespace pulp::host

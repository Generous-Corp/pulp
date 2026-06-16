#include <pulp/host/node_pack.hpp>

#include <pulp/native_components/pulp_node_v1.h>
#include <pulp/native_components/pulp_node_v1.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
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

std::vector<std::uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
}

// Platform dlopen shim. Returns an opaque handle or nullptr.
void* dl_open(const std::string& path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::LoadLibraryA(path.c_str()));
#else
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
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

NodePackLoadResult load_node_pack(const std::string& manifest_dir,
                                  const NodePackManifest& manifest,
                                  const NodePackTrust& trust) {
    return load_node_pack(manifest_dir, manifest, trust, NodePackHostPolicy{});
}

NodePackLoadResult load_node_pack(const std::string& manifest_dir,
                                  const NodePackManifest& manifest,
                                  const NodePackTrust& trust,
                                  const NodePackHostPolicy& policy) {
    NodePackLoadResult r;

    if (manifest.abi_major != PULP_NODE_V1_ABI_MAJOR) {
        r.error = NodePackError::AbiMismatch;
        return r;
    }
    if (manifest.signer_public_key.size() != runtime::ed25519_public_key_size ||
        manifest.signature.size() != runtime::ed25519_signature_size ||
        !has_valid_node_declarations(manifest.nodes) ||
        !has_valid_resource_declarations(manifest.resources)) {
        r.error = NodePackError::ManifestInvalid;
        return r;
    }
    if (!key_is_trusted(manifest.signer_public_key, trust)) {
        r.error = NodePackError::UntrustedSigner;
        return r;
    }
    const std::vector<std::uint8_t> message = node_pack_signed_message(manifest);
    if (!runtime::ed25519_verify(
            manifest.signer_public_key.data(), manifest.signer_public_key.size(),
            manifest.signature.data(), manifest.signature.size(),
            message.data(), message.size())) {
        r.error = NodePackError::BadSignature;
        return r;
    }
    if (!requirements_are_supported(manifest, policy)) {
        r.error = NodePackError::UnsupportedRequirements;
        return r;
    }

    // Only now do we touch the binary. Verify its hash matches the signed value.
    const std::string binary_path = manifest_dir + "/" + manifest.binary;
    const std::vector<std::uint8_t> bytes = read_file_bytes(binary_path);
    if (bytes.empty()) {
        r.error = NodePackError::LoadFailed;
        return r;
    }
    if (runtime::sha256_hex(bytes.data(), bytes.size()) != manifest.sha256_hex) {
        r.error = NodePackError::HashMismatch;
        return r;
    }

    void* handle = dl_open(binary_path);
    if (handle == nullptr) {
        r.error = NodePackError::LoadFailed;
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

    r.error = NodePackError::Ok;
    r.entry = entry;
    r.handle = handle;
    return r;
}

void unload_node_pack(void* handle) {
    if (handle != nullptr) dl_close(handle);
}

}  // namespace pulp::host

#include <pulp/host/node_pack.hpp>

#include <pulp/native_components/pulp_node_v1.h>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <fstream>
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
    out.pack_id = get_str(root, "pack_id");
    out.abi_major = get_u32(root, "abi_major");
    out.binary = get_str(root, "binary");
    out.sha256_hex = get_str(root, "sha256");
    out.signer_public_key = hex_decode(get_str(root, "signer_public_key"));
    out.signature = hex_decode(get_str(root, "signature"));
    out.nodes.clear();
    if (root.hasObjectMember("nodes") && root["nodes"].isArray()) {
        const auto& arr = root["nodes"];
        for (uint32_t i = 0; i < arr.size(); ++i) {
            NodePackEntry e;
            e.type_id = get_str(arr[i], "type_id");
            e.capabilities = get_u32(arr[i], "capabilities");
            out.nodes.push_back(std::move(e));
        }
    }
    // Structural validation: required fields + exact Ed25519 key/sig sizes.
    if (out.pack_id.empty() || out.binary.empty() || out.sha256_hex.empty())
        return false;
    if (out.signer_public_key.size() != runtime::ed25519_public_key_size)
        return false;
    if (out.signature.size() != runtime::ed25519_signature_size) return false;
    return true;
}

std::vector<std::uint8_t> node_pack_signed_message(const NodePackManifest& m) {
    // Binds pack identity + ABI major + the binary hash. Deterministic and
    // independent of JSON formatting, so re-serialization can't break the sig.
    std::string s = "pulp-node-pack-v1\n";
    s += m.pack_id;
    s += '\n';
    s += std::to_string(m.abi_major);
    s += '\n';
    s += m.sha256_hex;
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

NodePackLoadResult load_node_pack(const std::string& manifest_dir,
                                  const NodePackManifest& manifest,
                                  const NodePackTrust& trust) {
    NodePackLoadResult r;

    if (manifest.abi_major != PULP_NODE_V1_ABI_MAJOR) {
        r.error = NodePackError::AbiMismatch;
        return r;
    }
    if (manifest.signer_public_key.size() != runtime::ed25519_public_key_size ||
        manifest.signature.size() != runtime::ed25519_signature_size) {
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
    if (entry == nullptr || entry->abi_major != PULP_NODE_V1_ABI_MAJOR) {
        dl_close(handle);
        r.error = NodePackError::AbiMismatch;
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

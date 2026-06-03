// Phase 7 — signed node-pack loader. Builds a manifest for a real loadable node
// MODULE, signs it with a test Ed25519 key, and asserts the loader accepts the
// trusted/intact pack and rejects every tampering: untrusted signer, bad
// signature, hash mismatch, ABI mismatch, and a missing binary. The dlopen path
// is exercised end-to-end against the built module.

#include <pulp/host/node_pack.hpp>

#include <pulp/native_components/pulp_node_v1.h>
#include <pulp/runtime/crypto.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pulp::host;
namespace fs = std::filesystem;

#ifndef PULP_NODE_PACK_MODULE_PATH
#error "PULP_NODE_PACK_MODULE_PATH must be defined to the built node module"
#endif

namespace {

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

pulp::runtime::Ed25519KeyPair test_keypair() {
    std::vector<uint8_t> seed(32, 0x07);
    auto kp = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());
    return *kp;
}

// Build a fully-signed manifest for the built module, signed by `kp`.
NodePackManifest signed_manifest(const pulp::runtime::Ed25519KeyPair& kp,
                                 const std::string& module_path) {
    NodePackManifest m;
    m.pack_id = "com.pulp.test.pack";
    m.abi_major = PULP_NODE_V1_ABI_MAJOR;
    m.binary = fs::path(module_path).filename().string();
    const auto bytes = read_bytes(module_path);
    m.sha256_hex = pulp::runtime::sha256_hex(bytes.data(), bytes.size());
    m.signer_public_key = kp.public_key;
    const auto msg = node_pack_signed_message(m);
    auto sig = pulp::runtime::ed25519_sign(kp.private_key.data(),
                                           kp.private_key.size(), msg.data(),
                                           msg.size());
    REQUIRE(sig.has_value());
    m.signature = *sig;
    return m;
}

}  // namespace

TEST_CASE("node pack: a trusted, intact, signed pack loads and runs",
          "[host][node-pack]") {
    const std::string module_path = PULP_NODE_PACK_MODULE_PATH;
    const std::string dir = fs::path(module_path).parent_path().string();
    const auto kp = test_keypair();
    const auto manifest = signed_manifest(kp, module_path);

    NodePackTrust trust;
    trust.trusted_public_keys.push_back(kp.public_key);

    auto r = load_node_pack(dir, manifest, trust);
    REQUIRE(r.ok());
    REQUIRE(r.entry != nullptr);

    const pulp_node_descriptor_v1* d = r.entry->descriptor();
    REQUIRE(std::string_view(d->stable_id, d->stable_id_len) ==
            "pulp.test.pack.gain");

    pulp_node_host_services_v1 host{};
    host.size = sizeof(host);
    host.abi_major = PULP_NODE_V1_ABI_MAJOR;
    pulp_node_instance_v1* inst = nullptr;
    REQUIRE(r.entry->create(&host, &inst) == PULP_NODE_OK_V1);

    const float level = 0.5f;
    uint8_t blob[sizeof(float)];
    std::memcpy(blob, &level, sizeof(float));
    REQUIRE(r.entry->load_state(inst, blob, sizeof(blob)) == PULP_NODE_OK_V1);

    float in[4] = {1, 1, 1, 1};
    float out[4] = {0, 0, 0, 0};
    const float* in_p[1] = {in};
    float* out_p[1] = {out};
    pulp_node_audio_v1 audio{};
    audio.size = sizeof(audio);
    audio.abi_major = PULP_NODE_V1_ABI_MAJOR;
    audio.frame_count = 4;
    audio.input_count = 1;
    audio.output_count = 1;
    audio.inputs = in_p;
    audio.outputs = out_p;
    REQUIRE(r.entry->process(inst, &audio) == PULP_NODE_OK_V1);
    REQUIRE(out[0] == 0.5f);

    r.entry->destroy(inst);
    unload_node_pack(r.handle);
}

TEST_CASE("node pack: rejection paths never load untrusted or tampered code",
          "[host][node-pack]") {
    const std::string module_path = PULP_NODE_PACK_MODULE_PATH;
    const std::string dir = fs::path(module_path).parent_path().string();
    const auto kp = test_keypair();
    const auto good = signed_manifest(kp, module_path);

    SECTION("untrusted signer") {
        NodePackTrust empty;  // no trusted keys
        REQUIRE(load_node_pack(dir, good, empty).error ==
                NodePackError::UntrustedSigner);
    }
    SECTION("bad signature") {
        NodePackTrust trust;
        trust.trusted_public_keys.push_back(kp.public_key);
        NodePackManifest m = good;
        m.signature[0] ^= 0xFF;  // tamper
        REQUIRE(load_node_pack(dir, m, trust).error ==
                NodePackError::BadSignature);
    }
    SECTION("hash mismatch") {
        NodePackTrust trust;
        trust.trusted_public_keys.push_back(kp.public_key);
        // Re-sign a manifest whose declared hash is wrong, so the signature is
        // valid but the binary won't match it.
        NodePackManifest m = good;
        m.sha256_hex =
            "0000000000000000000000000000000000000000000000000000000000000000";
        const auto msg = node_pack_signed_message(m);
        m.signature = *pulp::runtime::ed25519_sign(
            kp.private_key.data(), kp.private_key.size(), msg.data(), msg.size());
        REQUIRE(load_node_pack(dir, m, trust).error ==
                NodePackError::HashMismatch);
    }
    SECTION("abi mismatch") {
        NodePackTrust trust;
        trust.trusted_public_keys.push_back(kp.public_key);
        NodePackManifest m = good;
        m.abi_major = PULP_NODE_V1_ABI_MAJOR + 1;
        REQUIRE(load_node_pack(dir, m, trust).error ==
                NodePackError::AbiMismatch);
    }
    SECTION("missing binary") {
        NodePackTrust trust;
        trust.trusted_public_keys.push_back(kp.public_key);
        NodePackManifest m = good;
        m.binary = "does-not-exist.bin";
        const auto msg = node_pack_signed_message(m);  // re-sign (hash unchanged)
        m.signature = *pulp::runtime::ed25519_sign(
            kp.private_key.data(), kp.private_key.size(), msg.data(), msg.size());
        REQUIRE(load_node_pack(dir, m, trust).error ==
                NodePackError::LoadFailed);
    }
}

TEST_CASE("node pack: manifest parse validates required fields + key sizes",
          "[host][node-pack]") {
    NodePackManifest m;
    REQUIRE_FALSE(parse_node_pack_manifest("not json", m));
    REQUIRE_FALSE(parse_node_pack_manifest("{}", m));
    // Valid shape with 32-byte key + 64-byte signature (hex).
    const std::string key(64, 'a');   // 32 bytes
    const std::string sig(128, 'b');  // 64 bytes
    const std::string json = "{\"pack_id\":\"p\",\"abi_major\":1,\"binary\":"
                             "\"b.so\",\"sha256\":\"deadbeef\",\"signer_public_"
                             "key\":\"" + key + "\",\"signature\":\"" + sig +
                             "\",\"nodes\":[{\"type_id\":\"t\",\"capabilities\":1}]}";
    REQUIRE(parse_node_pack_manifest(json, m));
    REQUIRE(m.pack_id == "p");
    REQUIRE(m.abi_major == 1);
    REQUIRE(m.signer_public_key.size() == 32);
    REQUIRE(m.signature.size() == 64);
    REQUIRE(m.nodes.size() == 1);
    REQUIRE(m.nodes[0].type_id == "t");
}

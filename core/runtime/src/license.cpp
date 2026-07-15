#include <pulp/runtime/license.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/http.hpp>

#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

#include <sstream>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <charconv>

namespace pulp::runtime {

namespace {

// v2 layout: "v2." + base64( IV(12) || ciphertext(N) || tag(16) )
constexpr std::string_view kV2Prefix = "v2.";
constexpr size_t kV2IvSize  = 12;  // AES-GCM IV per NIST SP 800-38D §8.2.1
constexpr size_t kV2TagSize = 16;
constexpr size_t kV2SecretSize = 32;

std::string license_info_to_json(const LicenseInfo& info) {
    std::ostringstream json;
    json << "{\"product_id\":\"" << info.product_id << "\"";
    if (!info.user_email.empty()) json << ",\"email\":\"" << info.user_email << "\"";
    if (!info.machine_id.empty()) json << ",\"machine_id\":\"" << info.machine_id << "\"";
    if (!info.edition.empty()) json << ",\"edition\":\"" << info.edition << "\"";
    json << ",\"issued\":" << info.issued_timestamp;
    if (info.expiry_timestamp > 0) json << ",\"expiry\":" << info.expiry_timestamp;
    json << "}";
    return json.str();
}

}  // namespace

// ── JSON helpers (minimal, no dependency on pugixml for this) ───────────

static std::string json_string_value(std::string_view json, std::string_view key) {
    std::string search = "\"" + std::string(key) + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string_view::npos) return "";
    auto start = pos + search.size();
    auto end = json.find('"', start);
    if (end == std::string_view::npos) return "";
    return std::string(json.substr(start, end - start));
}

static bool json_int_value(std::string_view json, std::string_view key, int64_t& value) {
    value = 0;
    std::string search = "\"" + std::string(key) + "\":";
    auto pos = json.find(search);
    if (pos == std::string_view::npos) return true;

    auto start = pos + search.size();
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start])))
        ++start;

    auto end = start;
    if (end < json.size() && json[end] == '-') ++end;

    auto digits_start = end;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])))
        ++end;
    if (end == digits_start) return true;

    auto result = std::from_chars(json.data() + start, json.data() + end, value);
    if (result.ec != std::errc{} || result.ptr != json.data() + end) return false;

    while (end < json.size() && std::isspace(static_cast<unsigned char>(json[end])))
        ++end;
    return end == json.size() || json[end] == ',' || json[end] == '}';
}

// ── LicenseValidator ────────────────────────────────────────────────────

void LicenseValidator::set_public_key(std::string_view pem) {
    public_key_pem_ = std::string(pem);
}

void LicenseValidator::set_shared_secret(const uint8_t* secret, size_t secret_size) {
    if (secret == nullptr || secret_size != kV2SecretSize) {
        shared_secret_.clear();
        return;
    }
    shared_secret_.assign(secret, secret + secret_size);
}

// ── Format detection ────────────────────────────────────────────────────

std::optional<LicenseFormatVersion> detect_license_format(std::string_view key) {
    if (key.substr(0, kV2Prefix.size()) == kV2Prefix)
        return LicenseFormatVersion::V2;
    // v1 layout is `<base64-payload>.<base64-signature>` — any string with
    // at least one '.' separator counts as v1. Empty payload / empty
    // signature halves are detected and rejected later by validate_v1 as
    // InvalidSignature, matching the pre-v2 behavior.
    if (key.find('.') != std::string_view::npos)
        return LicenseFormatVersion::V1;
    return std::nullopt;
}

std::optional<LicenseInfo> LicenseValidator::parse_payload(std::string_view json) const {
    LicenseInfo info;
    info.product_id = json_string_value(json, "product_id");
    info.user_email = json_string_value(json, "email");
    info.machine_id = json_string_value(json, "machine_id");
    info.edition = json_string_value(json, "edition");
    if (!json_int_value(json, "issued", info.issued_timestamp)) return std::nullopt;
    if (!json_int_value(json, "expiry", info.expiry_timestamp)) return std::nullopt;

    if (info.product_id.empty()) return std::nullopt;
    return info;
}

bool LicenseValidator::verify_signature(std::string_view payload,
                                         std::string_view signature_b64) const {
    if (public_key_pem_.empty()) return false;

    auto sig_bytes = base64_decode(signature_b64);
    if (!sig_bytes) return false;

    // Hash the payload
    uint8_t hash[32];
    mbedtls_sha256(reinterpret_cast<const uint8_t*>(payload.data()),
                   payload.size(), hash, 0);

    // Verify RSA signature
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int ret = mbedtls_pk_parse_public_key(&pk,
        reinterpret_cast<const uint8_t*>(public_key_pem_.c_str()),
        public_key_pem_.size() + 1);

    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return false;
    }

    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                            hash, 32,
                            sig_bytes->data(), sig_bytes->size());

    mbedtls_pk_free(&pk);
    return ret == 0;
}

LicenseStatus LicenseValidator::validate(std::string_view license_key) const {
    auto fmt = detect_license_format(license_key);
    if (!fmt) return LicenseStatus::InvalidFormat;
    return (*fmt == LicenseFormatVersion::V2)
        ? validate_v2(license_key)
        : validate_v1(license_key);
}

LicenseStatus LicenseValidator::validate_v1(std::string_view license_key) const {
    // Format: base64(payload).base64(signature)
    auto dot = license_key.find('.');
    if (dot == std::string_view::npos) return LicenseStatus::InvalidFormat;

    auto payload_b64 = license_key.substr(0, dot);
    auto sig_b64 = license_key.substr(dot + 1);

    // Decode payload
    auto payload_bytes = base64_decode(payload_b64);
    if (!payload_bytes) return LicenseStatus::InvalidFormat;

    std::string payload(payload_bytes->begin(), payload_bytes->end());

    // Verify signature
    if (!verify_signature(payload, sig_b64))
        return LicenseStatus::InvalidSignature;

    // Parse and check expiry
    auto info = parse_payload(payload);
    if (!info) return LicenseStatus::InvalidFormat;

    if (info->expiry_timestamp > 0) {
        auto now = std::time(nullptr);
        if (now > info->expiry_timestamp)
            return LicenseStatus::Expired;
    }

    // Check machine ID
    if (!info->machine_id.empty()) {
        if (info->machine_id != machine_id())
            return LicenseStatus::MachineIdMismatch;
    }

    return LicenseStatus::Valid;
}

LicenseStatus LicenseValidator::validate_v2(std::string_view license_key) const {
    if (shared_secret_.size() != kV2SecretSize) return LicenseStatus::InvalidSignature;
    if (license_key.substr(0, kV2Prefix.size()) != kV2Prefix)
        return LicenseStatus::InvalidFormat;

    auto blob = base64_decode(license_key.substr(kV2Prefix.size()));
    if (!blob) return LicenseStatus::InvalidFormat;
    if (blob->size() < kV2IvSize + kV2TagSize) return LicenseStatus::InvalidFormat;

    const uint8_t* iv  = blob->data();
    size_t ct_size     = blob->size() - kV2IvSize - kV2TagSize;
    const uint8_t* ct  = blob->data() + kV2IvSize;
    const uint8_t* tag = blob->data() + kV2IvSize + ct_size;

    // AAD = "v2." so a tampered prefix can't strip the format marker.
    auto plaintext = aes_gcm_decrypt(
        ct, ct_size,
        shared_secret_.data(),
        iv, kV2IvSize,
        reinterpret_cast<const uint8_t*>(kV2Prefix.data()), kV2Prefix.size(),
        tag);
    if (!plaintext) return LicenseStatus::InvalidSignature;

    std::string payload(plaintext->begin(), plaintext->end());
    auto info = parse_payload(payload);
    if (!info) return LicenseStatus::InvalidFormat;

    if (info->expiry_timestamp > 0) {
        auto now = std::time(nullptr);
        if (now > info->expiry_timestamp)
            return LicenseStatus::Expired;
    }
    if (!info->machine_id.empty()) {
        if (info->machine_id != machine_id())
            return LicenseStatus::MachineIdMismatch;
    }
    return LicenseStatus::Valid;
}

std::optional<LicenseInfo> LicenseValidator::validate_and_parse(std::string_view license_key) const {
    auto fmt = detect_license_format(license_key);
    if (!fmt) return std::nullopt;

    if (*fmt == LicenseFormatVersion::V1) {
        auto dot = license_key.find('.');
        if (dot == std::string_view::npos) return std::nullopt;
        auto payload_b64 = license_key.substr(0, dot);
        auto payload_bytes = base64_decode(payload_b64);
        if (!payload_bytes) return std::nullopt;
        std::string payload(payload_bytes->begin(), payload_bytes->end());
        return parse_payload(payload);
    }

    // v2 requires the shared secret to decrypt
    if (shared_secret_.size() != kV2SecretSize) return std::nullopt;
    auto blob = base64_decode(license_key.substr(kV2Prefix.size()));
    if (!blob || blob->size() < kV2IvSize + kV2TagSize) return std::nullopt;
    const uint8_t* iv  = blob->data();
    size_t ct_size     = blob->size() - kV2IvSize - kV2TagSize;
    const uint8_t* ct  = blob->data() + kV2IvSize;
    const uint8_t* tag = blob->data() + kV2IvSize + ct_size;
    auto plaintext = aes_gcm_decrypt(
        ct, ct_size,
        shared_secret_.data(),
        iv, kV2IvSize,
        reinterpret_cast<const uint8_t*>(kV2Prefix.data()), kV2Prefix.size(),
        tag);
    if (!plaintext) return std::nullopt;
    std::string payload(plaintext->begin(), plaintext->end());
    return parse_payload(payload);
}

LicenseStatus LicenseValidator::validate_file(std::string_view path) const {
    std::string file_path(path);
    std::ifstream file(file_path);
    if (!file) return LicenseStatus::NotFound;

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    // Trim whitespace
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
        content.pop_back();

    return validate(content);
}

bool LicenseValidator::is_valid_for_machine(const LicenseInfo& info) const {
    if (info.machine_id.empty()) return true;
    return info.machine_id == machine_id();
}

// ── LicenseGenerator ────────────────────────────────────────────────────

void LicenseGenerator::set_private_key(std::string_view pem) {
    private_key_pem_ = std::string(pem);
}

void LicenseGenerator::set_shared_secret(const uint8_t* secret, size_t secret_size) {
    if (secret == nullptr || secret_size != kV2SecretSize) {
        shared_secret_.clear();
        return;
    }
    shared_secret_.assign(secret, secret + secret_size);
}

std::optional<std::string> LicenseGenerator::generate(const LicenseInfo& info) const {
    // Prefer v2 if a shared secret is set; otherwise fall back to v1 so
    // existing callers that only ever wired an RSA key keep working.
    if (kCurrentLicenseFormat == LicenseFormatVersion::V2
        && shared_secret_.size() == kV2SecretSize)
        return generate_v2(info);
    return generate_v1(info);
}

std::optional<std::string> LicenseGenerator::generate_v1(const LicenseInfo& info) const {
    if (private_key_pem_.empty()) return std::nullopt;

    std::string payload = license_info_to_json(info);

    // Hash the payload
    uint8_t hash[32];
    mbedtls_sha256(reinterpret_cast<const uint8_t*>(payload.data()),
                   payload.size(), hash, 0);

    // Sign with RSA private key
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, nullptr, 0);

    int ret = mbedtls_pk_parse_key(&pk,
        reinterpret_cast<const uint8_t*>(private_key_pem_.c_str()),
        private_key_pem_.size() + 1, nullptr, 0,
        mbedtls_ctr_drbg_random, &ctr_drbg);

    if (ret != 0) {
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        return std::nullopt;
    }

    uint8_t sig[512];
    size_t sig_len = 0;
    ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256,
                          hash, 32, sig, sizeof(sig), &sig_len,
                          mbedtls_ctr_drbg_random, &ctr_drbg);

    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    if (ret != 0) return std::nullopt;

    // Encode: base64(payload).base64(signature)
    std::string result = base64_encode(payload);
    result += '.';
    result += base64_encode(sig, sig_len);

    return result;
}

std::optional<std::string> LicenseGenerator::generate_v2(const LicenseInfo& info) const {
    if (shared_secret_.size() != kV2SecretSize) return std::nullopt;

    std::string payload = license_info_to_json(info);

    // Draw a fresh 12-byte IV from CTR-DRBG.
    uint8_t iv[kV2IvSize];
    {
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                       reinterpret_cast<const uint8_t*>("pulp::license::v2"),
                                       17);
        if (rc == 0) rc = mbedtls_ctr_drbg_random(&drbg, iv, kV2IvSize);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        if (rc != 0) return std::nullopt;
    }

    auto gcm = aes_gcm_encrypt(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
        shared_secret_.data(),
        iv, kV2IvSize,
        reinterpret_cast<const uint8_t*>(kV2Prefix.data()), kV2Prefix.size());
    if (!gcm) return std::nullopt;

    // Concatenate IV || ciphertext || tag and base64-encode.
    std::vector<uint8_t> blob;
    blob.reserve(kV2IvSize + gcm->ciphertext.size() + gcm->tag.size());
    blob.insert(blob.end(), iv, iv + kV2IvSize);
    blob.insert(blob.end(), gcm->ciphertext.begin(), gcm->ciphertext.end());
    blob.insert(blob.end(), gcm->tag.begin(), gcm->tag.end());

    std::string out(kV2Prefix);
    out += base64_encode(blob.data(), blob.size());
    return out;
}

// ── Migration: v1 -> v2 ─────────────────────────────────────────────────

std::optional<std::string> migrate_v1_to_v2(
    std::string_view v1_license_key,
    std::string_view v1_public_key_pem,
    const uint8_t* v2_secret,
    size_t v2_secret_size) {
    if (v2_secret == nullptr || v2_secret_size != kV2SecretSize)
        return std::nullopt;
    if (detect_license_format(v1_license_key) != LicenseFormatVersion::V1)
        return std::nullopt;

    // 1. Verify the v1 signature so a tampered v1 key cannot upgrade
    //    itself into a valid v2 key.
    LicenseValidator v1;
    v1.set_public_key(v1_public_key_pem);
    if (v1.validate(v1_license_key) != LicenseStatus::Valid) {
        // Allow Expired/MachineId — migration of an expired/cross-machine
        // key is still a useful operation (e.g. server-side re-issue with
        // a fresh expiry). Reject only on InvalidSignature/Format.
        auto status = v1.validate(v1_license_key);
        if (status == LicenseStatus::InvalidSignature
            || status == LicenseStatus::InvalidFormat)
            return std::nullopt;
    }

    auto info = v1.validate_and_parse(v1_license_key);
    if (!info) return std::nullopt;

    LicenseGenerator gen;
    gen.set_shared_secret(v2_secret, v2_secret_size);
    return gen.generate_v2(*info);
}

// ── OnlineActivation ────────────────────────────────────────────────────

std::optional<std::string> OnlineActivation::activate(
    std::string_view server_url, std::string_view serial_number,
    std::string_view product_id)
{
    std::string body = "{\"serial\":\"" + std::string(serial_number)
                     + "\",\"product\":\"" + std::string(product_id)
                     + "\",\"machine\":\"" + machine_id() + "\"}";

    std::string url = std::string(server_url) + "/activate";
    auto response = http_post(url, body, "application/json");

    if (!response.ok()) return std::nullopt;
    return response.body;
}

bool OnlineActivation::deactivate(std::string_view server_url,
                                   std::string_view license_key) {
    std::string body = "{\"key\":\"" + std::string(license_key)
                     + "\",\"machine\":\"" + machine_id() + "\"}";

    std::string url = std::string(server_url) + "/deactivate";
    auto response = http_post(url, body, "application/json");
    return response.ok();
}

LicenseStatus OnlineActivation::check_status(std::string_view server_url,
                                              std::string_view license_key) {
    std::string url = std::string(server_url) + "/status?key="
                    + std::string(license_key) + "&machine=" + machine_id();
    auto response = http_get(url);

    if (!response.ok()) return LicenseStatus::NotFound;
    if (response.body.find("\"valid\"") != std::string::npos) return LicenseStatus::Valid;
    if (response.body.find("\"expired\"") != std::string::npos) return LicenseStatus::Expired;
    return LicenseStatus::InvalidSignature;
}

}  // namespace pulp::runtime

#include <pulp/inspect/authentication.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>

namespace pulp::inspect {
namespace {

std::string proof_payload(const InspectorAuthChallenge& challenge) {
    std::string payload;
    payload.reserve(challenge.scheme.size() + challenge.nonce_hex.size() +
                    challenge.session_id.size() + challenge.instance_id.size() +
                    challenge.publication_id.size() +
                    challenge.protocol_version.size() + 6);
    payload.append(challenge.scheme);
    payload.push_back('\0');
    payload.append(challenge.nonce_hex);
    payload.push_back('\0');
    payload.append(challenge.session_id);
    payload.push_back('\0');
    payload.append(challenge.instance_id);
    payload.push_back('\0');
    payload.append(challenge.publication_id);
    payload.push_back('\0');
    payload.append(challenge.protocol_version);
    return payload;
}

std::string server_proof_payload(
    const InspectorAuthChallenge& challenge,
    std::string_view client_proof) {
    auto payload = proof_payload(challenge);
    payload.insert(0, "server\0", 7);
    payload.push_back('\0');
    payload.append(client_proof);
    return payload;
}

std::optional<std::vector<std::uint8_t>> decode_hex(std::string_view hex) {
    if (hex.size() % 2 != 0)
        return std::nullopt;
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    };

    std::vector<std::uint8_t> bytes(hex.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const int high = nibble(hex[i * 2]);
        const int low = nibble(hex[i * 2 + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return bytes;
}

bool valid_auth_inputs(
    std::span<const std::uint8_t> token,
    const InspectorAuthChallenge& challenge) {
    const auto nonce = decode_hex(challenge.nonce_hex);
    return token.size() == inspector_token_size &&
           challenge.scheme == "pulp-inspector-hmac-sha256-v1" &&
           nonce && nonce->size() == inspector_nonce_size &&
           !challenge.session_id.empty() &&
           !challenge.instance_id.empty() &&
           !challenge.publication_id.empty() &&
           !challenge.protocol_version.empty();
}

} // namespace

std::optional<std::vector<std::uint8_t>> generate_inspector_secret() {
    return pulp::runtime::secure_random_bytes(inspector_token_size);
}

std::optional<InspectorAuthChallenge> make_inspector_auth_challenge(
    std::string session_id,
    std::string instance_id,
    std::string publication_id,
    std::string protocol_version) {
    if (session_id.empty() || instance_id.empty() ||
        publication_id.empty() || protocol_version.empty())
        return std::nullopt;
    const auto nonce = pulp::runtime::secure_random_bytes(inspector_nonce_size);
    if (!nonce)
        return std::nullopt;
    InspectorAuthChallenge challenge;
    challenge.nonce_hex = pulp::runtime::hex_encode(*nonce);
    challenge.session_id = std::move(session_id);
    challenge.instance_id = std::move(instance_id);
    challenge.publication_id = std::move(publication_id);
    challenge.protocol_version = std::move(protocol_version);
    return challenge;
}

std::optional<std::string> make_inspector_auth_proof(
    std::span<const std::uint8_t> token,
    const InspectorAuthChallenge& challenge) {
    if (!valid_auth_inputs(token, challenge)) {
        return std::nullopt;
    }
    const auto payload = proof_payload(challenge);
    const auto tag = pulp::runtime::hmac_sha256(
        token.data(),
        token.size(),
        reinterpret_cast<const std::uint8_t*>(payload.data()),
        payload.size());
    if (!tag)
        return std::nullopt;
    return pulp::runtime::hex_encode(*tag);
}

std::optional<std::string> make_inspector_server_auth_proof(
    std::span<const std::uint8_t> token,
    const InspectorAuthChallenge& challenge,
    std::string_view client_proof) {
    const auto decoded_client_proof = decode_hex(client_proof);
    if (!valid_auth_inputs(token, challenge) ||
        !decoded_client_proof ||
        decoded_client_proof->size() != 32) {
        return std::nullopt;
    }
    const auto payload =
        server_proof_payload(challenge, client_proof);
    const auto tag = pulp::runtime::hmac_sha256(
        token.data(),
        token.size(),
        reinterpret_cast<const std::uint8_t*>(payload.data()),
        payload.size());
    if (!tag)
        return std::nullopt;
    return pulp::runtime::hex_encode(*tag);
}

bool verify_inspector_server_auth_proof(
    std::span<const std::uint8_t> token,
    const InspectorAuthChallenge& challenge,
    std::string_view client_proof,
    std::string_view server_proof) {
    const auto expected = make_inspector_server_auth_proof(
        token, challenge, client_proof);
    const auto expected_bytes =
        expected ? decode_hex(*expected) : std::nullopt;
    const auto supplied = decode_hex(server_proof);
    return expected_bytes && supplied &&
           expected_bytes->size() == supplied->size() &&
           pulp::runtime::constant_time_equal(
               expected_bytes->data(), supplied->data(),
               supplied->size());
}

InspectorAuthVerifier::InspectorAuthVerifier(
    std::vector<std::uint8_t> token,
    InspectorAuthChallenge challenge)
    : token_(std::move(token)),
      challenge_(std::move(challenge)) {}

InspectorAuthVerifier::~InspectorAuthVerifier() {
    pulp::runtime::secure_zero_memory(token_.data(), token_.size());
}

bool InspectorAuthVerifier::verify(std::string_view proof_hex) {
    return authenticate(proof_hex).has_value();
}

std::optional<std::string> InspectorAuthVerifier::authenticate(
    std::string_view proof_hex) {
    if (consumed_)
        return std::nullopt;
    consumed_ = true;

    const auto expected = make_inspector_auth_proof(token_, challenge_);
    const auto supplied = decode_hex(proof_hex);
    std::optional<std::string> server_proof;
    if (expected && supplied && supplied->size() == 32) {
        const auto expected_bytes = decode_hex(*expected);
        if (expected_bytes &&
            expected_bytes->size() == supplied->size() &&
            pulp::runtime::constant_time_equal(
                expected_bytes->data(), supplied->data(),
                supplied->size())) {
            server_proof = make_inspector_server_auth_proof(
                token_, challenge_, proof_hex);
        }
    }
    pulp::runtime::secure_zero_memory(token_.data(), token_.size());
    return server_proof;
}

} // namespace pulp::inspect

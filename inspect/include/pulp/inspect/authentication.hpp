#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

inline constexpr std::size_t inspector_token_size = 32;
inline constexpr std::size_t inspector_nonce_size = 32;

struct InspectorAuthChallenge {
    std::string scheme = "pulp-inspector-hmac-sha256-v1";
    std::string nonce_hex;
    std::string session_id;
    std::string instance_id;
    std::string protocol_version;
};

/// Generate a per-session 256-bit token or per-connection 256-bit nonce.
std::optional<std::vector<std::uint8_t>> generate_inspector_secret();

std::optional<InspectorAuthChallenge> make_inspector_auth_challenge(
    std::string session_id,
    std::string instance_id,
    std::string protocol_version);

/// Construct the versioned HMAC proof used by installed clients. The raw token
/// is never placed in a protocol payload.
std::optional<std::string> make_inspector_auth_proof(
    std::span<const std::uint8_t> token,
    const InspectorAuthChallenge& challenge);

/// Construct and verify the distinct server-side transcript proof returned
/// after the client proof succeeds.
std::optional<std::string> make_inspector_server_auth_proof(
    std::span<const std::uint8_t> token,
    const InspectorAuthChallenge& challenge,
    std::string_view client_proof);
bool verify_inspector_server_auth_proof(
    std::span<const std::uint8_t> token,
    const InspectorAuthChallenge& challenge,
    std::string_view client_proof,
    std::string_view server_proof);

/// One-shot verifier for a server-issued nonce. Every verification attempt
/// consumes the challenge, so failed and replayed proofs fail closed.
class InspectorAuthVerifier {
public:
    InspectorAuthVerifier(std::vector<std::uint8_t> token,
                          InspectorAuthChallenge challenge);
    ~InspectorAuthVerifier();

    InspectorAuthVerifier(const InspectorAuthVerifier&) = delete;
    InspectorAuthVerifier& operator=(const InspectorAuthVerifier&) = delete;
    InspectorAuthVerifier(InspectorAuthVerifier&&) = delete;
    InspectorAuthVerifier& operator=(InspectorAuthVerifier&&) = delete;

    const InspectorAuthChallenge& challenge() const { return challenge_; }
    bool verify(std::string_view proof_hex);
    std::optional<std::string> authenticate(
        std::string_view client_proof);
    bool consumed() const { return consumed_; }

private:
    std::vector<std::uint8_t> token_;
    InspectorAuthChallenge challenge_;
    bool consumed_ = false;
};

} // namespace pulp::inspect

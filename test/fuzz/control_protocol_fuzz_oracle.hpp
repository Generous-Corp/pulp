#pragma once

#include <pulp/inspect/control_protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::test::control_protocol_fuzz {

enum class FindingKind : std::uint8_t {
    None,
    NondeterministicDecode,
    OversizeAccepted,
    OversizeMisclassified,
    AcceptedEnvelopeNotEncodable,
    EncodedEnvelopeTooLarge,
    CanonicalEnvelopeRejected,
    RoundTripChangedEnvelope,
    NoncanonicalEncoding,
};

struct Finding {
    FindingKind kind = FindingKind::None;
    std::string detail;

    explicit operator bool() const noexcept {
        return kind != FindingKind::None;
    }
};

inline std::string_view describe(FindingKind kind) noexcept {
    switch (kind) {
    case FindingKind::None:
        return "none";
    case FindingKind::NondeterministicDecode:
        return "nondeterministic-decode";
    case FindingKind::OversizeAccepted:
        return "oversize-accepted";
    case FindingKind::OversizeMisclassified:
        return "oversize-misclassified";
    case FindingKind::AcceptedEnvelopeNotEncodable:
        return "accepted-envelope-not-encodable";
    case FindingKind::EncodedEnvelopeTooLarge:
        return "encoded-envelope-too-large";
    case FindingKind::CanonicalEnvelopeRejected:
        return "canonical-envelope-rejected";
    case FindingKind::RoundTripChangedEnvelope:
        return "round-trip-changed-envelope";
    case FindingKind::NoncanonicalEncoding:
        return "noncanonical-encoding";
    }
    return "unknown";
}

inline std::string format_finding(const Finding& finding) {
    auto result = std::string(describe(finding.kind));
    if (!finding.detail.empty())
        result += ": " + finding.detail;
    return result;
}

/// Exercises the untrusted decoder and every invariant that must hold whenever
/// it accepts bytes. Exceptions deliberately escape: either driver treats one
/// as a crash, which is itself a finding for this API boundary.
inline Finding inspect(std::string_view input) {
    using namespace pulp::inspect;

    ControlProtocolDiagnostics first_diagnostics;
    ControlProtocolDiagnostics second_diagnostics;
    const auto first = decode_control_envelope(input, &first_diagnostics);
    const auto second = decode_control_envelope(input, &second_diagnostics);
    if (first.has_value() != second.has_value() || (first && second && *first != *second) ||
        first_diagnostics.code != second_diagnostics.code ||
        first_diagnostics.explanation != second_diagnostics.explanation) {
        return {FindingKind::NondeterministicDecode, "identical bytes produced different outcomes"};
    }

    if (input.size() > kControlMaximumEnvelopeBytes) {
        if (first) {
            return {FindingKind::OversizeAccepted,
                    "decoder admitted more than the envelope byte ceiling"};
        }
        if (first_diagnostics.code != ControlProtocolError::EnvelopeTooLarge) {
            return {FindingKind::OversizeMisclassified,
                    "oversized input did not fail at the byte boundary"};
        }
        return {};
    }

    // Rejection is the expected result for arbitrary bytes. The remaining
    // properties apply only when the decoder admitted a closed envelope.
    if (!first)
        return {};

    const auto encoded = encode_control_envelope(*first);
    if (encoded.empty()) {
        return {FindingKind::AcceptedEnvelopeNotEncodable,
                "accepted value failed the encoder's own validation"};
    }
    if (encoded.size() > kControlMaximumEnvelopeBytes) {
        return {FindingKind::EncodedEnvelopeTooLarge,
                "canonical encoding exceeded the decoder ceiling"};
    }

    ControlProtocolDiagnostics canonical_diagnostics;
    const auto canonical = decode_control_envelope(encoded, &canonical_diagnostics);
    if (!canonical) {
        return {FindingKind::CanonicalEnvelopeRejected,
                std::string("canonical bytes rejected: ") + canonical_diagnostics.explanation};
    }
    if (*canonical != *first) {
        return {FindingKind::RoundTripChangedEnvelope,
                "decode-encode-decode changed the typed envelope"};
    }
    if (encode_control_envelope(*canonical) != encoded) {
        return {FindingKind::NoncanonicalEncoding,
                "re-encoding canonical bytes was not byte-identical"};
    }
    return {};
}

} // namespace pulp::test::control_protocol_fuzz

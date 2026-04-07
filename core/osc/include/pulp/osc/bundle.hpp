#pragma once

// OSC Bundle — timetag + nested messages/bundles per OSC 1.0 spec.

#include <pulp/osc/osc.hpp>
#include <vector>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace pulp::osc {

/// OSC timetag — NTP timestamp (seconds since 1900-01-01)
struct TimeTag {
    uint32_t seconds = 0;
    uint32_t fraction = 0;

    /// "Immediately" timetag (1, 0)
    static TimeTag immediate() { return {0, 1}; }

    /// From Unix epoch seconds
    static TimeTag from_unix(double unix_time);

    /// To Unix epoch seconds
    double to_unix() const;

    bool operator==(const TimeTag& o) const {
        return seconds == o.seconds && fraction == o.fraction;
    }
};

struct Bundle;  // forward declaration

/// Bundle element — either a message or a nested bundle.
/// Uses unique_ptr<Bundle> to break the recursive value type.
struct BundleElement {
    std::variant<Message, std::unique_ptr<Bundle>> content;

    BundleElement() = default;

    /// Construct from a message
    explicit BundleElement(Message msg) : content(std::move(msg)) {}

    /// Construct from a nested bundle (takes ownership)
    explicit BundleElement(std::unique_ptr<Bundle> b) : content(std::move(b)) {}

    /// Convenience: construct from a bundle value (heap-allocates a copy)
    explicit BundleElement(Bundle b);

    bool is_message() const { return std::holds_alternative<Message>(content); }
    bool is_bundle() const { return std::holds_alternative<std::unique_ptr<Bundle>>(content); }

    const Message& message() const { return std::get<Message>(content); }
    const Bundle& bundle() const { return *std::get<std::unique_ptr<Bundle>>(content); }
};

/// OSC Bundle — contains a timetag and a list of elements
struct Bundle {
    TimeTag timetag = TimeTag::immediate();
    std::vector<BundleElement> elements;

    /// Add a message to the bundle
    void add(Message msg) {
        elements.emplace_back(std::move(msg));
    }

    /// Add a nested bundle
    void add(Bundle nested) {
        elements.emplace_back(std::make_unique<Bundle>(std::move(nested)));
    }

    /// Serialize to OSC binary format
    std::vector<uint8_t> serialize() const;

    /// Deserialize from OSC binary
    static std::optional<Bundle> deserialize(const uint8_t* data, size_t size);
};

/// Deferred definition (Bundle is complete here)
inline BundleElement::BundleElement(Bundle b)
    : content(std::make_unique<Bundle>(std::move(b))) {}

/// OSC address pattern matching per OSC 1.0 spec.
/// Supports: * (any), ? (single char), [...] (character class), {...} (alternatives)
bool address_matches(std::string_view pattern, std::string_view address);

}  // namespace pulp::osc

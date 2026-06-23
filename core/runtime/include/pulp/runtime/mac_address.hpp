#pragma once

/// @file mac_address.hpp
/// MAC address parsing, formatting, and validation utility.
///
/// Pure parser + formatter — no platform calls, no allocation in the
/// common path. Headless-friendly, RT-safe to copy/compare.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::runtime {

/// 48-bit MAC address (IEEE 802 EUI-48). Stored as 6 octets in
/// transmission order — `octets()[0]` is the OUI's first byte.
///
/// Accepted text forms (case-insensitive):
///   - Colon-separated:    `aa:bb:cc:dd:ee:ff`
///   - Hyphen-separated:   `aa-bb-cc-dd-ee-ff`
///   - Dotted-quad-of-3:   `aabb.ccdd.eeff` (Cisco style)
///   - Plain hex:          `aabbccddeeff`
class MacAddress {
public:
    constexpr MacAddress() = default;

    /// Construct from 6 raw octets in transmission order.
    constexpr MacAddress(uint8_t a, uint8_t b, uint8_t c,
                         uint8_t d, uint8_t e, uint8_t f)
        : octets_{a, b, c, d, e, f} {}

    /// Try to parse a MAC address. Returns `std::nullopt` on malformed
    /// input. Whitespace around the value is ignored; embedded
    /// whitespace is rejected.
    static std::optional<MacAddress> parse(std::string_view text);

    /// True if `text` parses as a valid MAC address.
    static bool is_valid(std::string_view text) {
        return parse(text).has_value();
    }

    /// Canonical colon-separated lowercase form (e.g. `aa:bb:cc:dd:ee:ff`).
    std::string to_string() const;

    /// Same as `to_string()` but with hyphens (e.g. `aa-bb-cc-dd-ee-ff`).
    std::string to_string_hyphens() const;

    /// Raw octets in transmission order.
    const std::array<uint8_t, 6>& octets() const { return octets_; }

    /// All-zero address — typically used as a sentinel for "unknown".
    bool is_null() const;

    /// Group bit (LSB of first octet). 0 = unicast, 1 = multicast / broadcast.
    bool is_multicast() const { return (octets_[0] & 0x01) != 0; }

    /// Local-administration bit (bit 1 of first octet). 1 = locally
    /// administered, 0 = globally unique OUI-assigned.
    bool is_locally_administered() const { return (octets_[0] & 0x02) != 0; }

    /// `ff:ff:ff:ff:ff:ff` — the L2 broadcast address.
    bool is_broadcast() const;

    /// Organisationally Unique Identifier — first 3 octets packed
    /// big-endian into the low 24 bits of a uint32_t. Useful for vendor
    /// lookups.
    uint32_t oui() const {
        return (uint32_t(octets_[0]) << 16) |
               (uint32_t(octets_[1]) << 8) |
                uint32_t(octets_[2]);
    }

    bool operator==(const MacAddress& other) const {
        return octets_ == other.octets_;
    }
    bool operator!=(const MacAddress& other) const {
        return !(*this == other);
    }

private:
    std::array<uint8_t, 6> octets_{};
};

}  // namespace pulp::runtime

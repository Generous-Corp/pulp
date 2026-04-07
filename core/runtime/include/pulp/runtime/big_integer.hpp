#pragma once

// BigInteger — arbitrary-precision integer for cryptographic operations.
// Wraps Mbed TLS MPI (Multi-Precision Integer) for RSA key operations.

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <optional>

namespace pulp::runtime {

/// Arbitrary-precision unsigned integer backed by Mbed TLS MPI.
class BigInteger {
public:
    BigInteger();
    BigInteger(uint64_t value);
    BigInteger(const BigInteger& other);
    BigInteger(BigInteger&& other) noexcept;
    ~BigInteger();

    BigInteger& operator=(const BigInteger& other);
    BigInteger& operator=(BigInteger&& other) noexcept;

    /// Parse from decimal string
    static BigInteger from_string(std::string_view decimal);

    /// Parse from hex string
    static BigInteger from_hex(std::string_view hex);

    /// Convert to decimal string
    std::string to_string() const;

    /// Convert to hex string
    std::string to_hex() const;

    /// Whether this is zero
    bool is_zero() const;

    /// Number of bits needed to represent this value
    size_t bit_count() const;

    /// Arithmetic operations
    BigInteger operator+(const BigInteger& other) const;
    BigInteger operator*(const BigInteger& other) const;
    BigInteger operator%(const BigInteger& other) const;

    /// Modular exponentiation: (this^exp) mod modulus
    /// Used for RSA encrypt/decrypt.
    BigInteger mod_pow(const BigInteger& exp, const BigInteger& modulus) const;

    /// Comparison
    bool operator==(const BigInteger& other) const;
    bool operator!=(const BigInteger& other) const;
    bool operator<(const BigInteger& other) const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace pulp::runtime

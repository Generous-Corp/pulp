#pragma once

// Prime number utilities — generation and primality testing via Mbed TLS.
// Used for RSA key generation.

#include <cstdint>
#include <vector>

namespace pulp::runtime {

/// Test if a number is probably prime (Miller-Rabin).
/// rounds controls accuracy (default 40 = negligible false positive rate).
bool is_prime(uint64_t n, int rounds = 40);

/// Generate a random prime of the given bit length.
/// Returns 0 on failure.
uint64_t generate_prime(int bits = 32);

/// Generate the first N primes via sieve of Eratosthenes.
std::vector<uint32_t> sieve_primes(uint32_t limit);

}  // namespace pulp::runtime

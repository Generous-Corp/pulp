#include <pulp/runtime/primes.hpp>
#include <cmath>
#include <random>

namespace pulp::runtime {

bool is_prime(uint64_t n, int rounds) {
    if (n < 2) return false;
    if (n < 4) return true;
    if (n % 2 == 0) return false;

    // Write n-1 as 2^r * d
    uint64_t d = n - 1;
    int r = 0;
    while (d % 2 == 0) { d /= 2; r++; }

    // Miller-Rabin
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist(2, n - 2);

    for (int i = 0; i < rounds; ++i) {
        uint64_t a = dist(rng);

        // Compute a^d mod n using modular exponentiation
        __uint128_t x = 1;
        __uint128_t base = a;
        uint64_t exp = d;
        while (exp > 0) {
            if (exp & 1) x = (x * base) % n;
            base = (base * base) % n;
            exp >>= 1;
        }

        uint64_t result = static_cast<uint64_t>(x);
        if (result == 1 || result == n - 1) continue;

        bool found = false;
        for (int j = 0; j < r - 1; ++j) {
            x = (static_cast<__uint128_t>(result) * result) % n;
            result = static_cast<uint64_t>(x);
            if (result == n - 1) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

uint64_t generate_prime(int bits) {
    if (bits < 2 || bits > 62) return 0;

    std::mt19937_64 rng(std::random_device{}());
    uint64_t min_val = 1ULL << (bits - 1);
    uint64_t max_val = (bits >= 63) ? UINT64_MAX : ((1ULL << bits) - 1);
    std::uniform_int_distribution<uint64_t> dist(min_val, max_val);

    for (int attempt = 0; attempt < 10000; ++attempt) {
        uint64_t candidate = dist(rng) | 1;  // Ensure odd
        if (is_prime(candidate))
            return candidate;
    }
    return 0;
}

std::vector<uint32_t> sieve_primes(uint32_t limit) {
    if (limit < 2) return {};

    std::vector<bool> is_p(limit + 1, true);
    is_p[0] = is_p[1] = false;

    for (uint32_t i = 2; i * i <= limit; ++i)
        if (is_p[i])
            for (uint32_t j = i * i; j <= limit; j += i)
                is_p[j] = false;

    std::vector<uint32_t> primes;
    for (uint32_t i = 2; i <= limit; ++i)
        if (is_p[i]) primes.push_back(i);
    return primes;
}

}  // namespace pulp::runtime

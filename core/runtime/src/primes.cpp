#include <pulp/runtime/primes.hpp>
#include <cmath>
#include <random>

// MSVC doesn't support __uint128_t. Use a portable multiply-mod helper.
#ifdef _MSC_VER
static uint64_t addmod(uint64_t a, uint64_t b, uint64_t m) {
    // Guard against uint64_t overflow: if a + b would wrap, subtract m first.
    // Since a < m and b < m, (a - (m - b)) is the correct reduced result.
    if (a >= m - b)
        return a - (m - b);
    return a + b;
}

static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m) {
    uint64_t result = 0;
    a %= m;
    while (b > 0) {
        if (b & 1) result = addmod(result, a, m);
        a = addmod(a, a, m);
        b >>= 1;
    }
    return result;
}
#endif

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
#ifdef _MSC_VER
        uint64_t x = 1;
        uint64_t base_val = a % n;
        uint64_t exp = d;
        while (exp > 0) {
            if (exp & 1) x = mulmod(x, base_val, n);
            base_val = mulmod(base_val, base_val, n);
            exp >>= 1;
        }
        uint64_t result = x;
#else
        __uint128_t x = 1;
        __uint128_t base_val = a;
        uint64_t exp = d;
        while (exp > 0) {
            if (exp & 1) x = (x * base_val) % n;
            base_val = (base_val * base_val) % n;
            exp >>= 1;
        }
        uint64_t result = static_cast<uint64_t>(x);
#endif
        if (result == 1 || result == n - 1) continue;

        bool found = false;
        for (int j = 0; j < r - 1; ++j) {
#ifdef _MSC_VER
            result = mulmod(result, result, n);
#else
            __uint128_t xx = (static_cast<__uint128_t>(result) * result) % n;
            result = static_cast<uint64_t>(xx);
#endif
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

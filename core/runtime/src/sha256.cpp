#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace pulp::runtime {
namespace {

class Sha256 {
public:
    void update(const std::uint8_t* data, std::size_t size) noexcept {
        total_bits_ += static_cast<std::uint64_t>(size) * 8;
        while (size > 0) {
            const auto count = std::min(buffer_.size() - buffered_, size);
            std::memcpy(buffer_.data() + buffered_, data, count);
            buffered_ += count;
            data += count;
            size -= count;
            if (buffered_ == buffer_.size()) {
                compress(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finish() noexcept {
        const auto message_bits = total_bits_;
        buffer_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                      buffer_.end(), 0);
            compress(buffer_.data());
            buffered_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                  buffer_.begin() + 56, 0);
        for (int byte = 0; byte < 8; ++byte) {
            buffer_[56 + byte] =
                static_cast<std::uint8_t>(message_bits >> ((7 - byte) * 8));
        }
        compress(buffer_.data());

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (int byte = 0; byte < 4; ++byte) {
                digest[word * 4 + static_cast<std::size_t>(byte)] =
                    static_cast<std::uint8_t>(state_[word] >> ((3 - byte) * 8));
            }
        }
        return digest;
    }

private:
    static std::uint32_t rotate_right(std::uint32_t value, std::uint32_t bits) noexcept {
        return (value >> bits) | (value << (32 - bits));
    }

    void compress(const std::uint8_t* block) noexcept {
        static constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = index * 4;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24) |
                           (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
                           (static_cast<std::uint32_t>(block[offset + 2]) << 8) |
                           static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto s0 = rotate_right(words[index - 15], 7) ^
                            rotate_right(words[index - 15], 18) ^
                            (words[index - 15] >> 3);
            const auto s1 = rotate_right(words[index - 2], 17) ^
                            rotate_right(words[index - 2], 19) ^
                            (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 =
                rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temporary1 =
                h + sum1 + choose + constants[index] + words[index];
            const auto sum0 =
                rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint64_t total_bits_ = 0;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
};

std::string digest_hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = digits[digest[index] >> 4];
        result[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    return result;
}

} // namespace

std::vector<std::uint8_t> sha256(const std::uint8_t* data, std::size_t size) {
    Sha256 hash;
    hash.update(data, size);
    const auto digest = hash.finish();
    return {digest.begin(), digest.end()};
}

std::vector<std::uint8_t> sha256(std::string_view data) {
    return sha256(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
    Sha256 hash;
    hash.update(data, size);
    return digest_hex(hash.finish());
}

std::string sha256_hex(std::string_view data) {
    return sha256_hex(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

} // namespace pulp::runtime

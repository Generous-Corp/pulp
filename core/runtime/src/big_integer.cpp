#include <pulp/runtime/big_integer.hpp>
#include <mbedtls/bignum.h>
#include <cstring>

namespace pulp::runtime {

struct BigInteger::Impl {
    mbedtls_mpi mpi;

    Impl() { mbedtls_mpi_init(&mpi); }
    ~Impl() { mbedtls_mpi_free(&mpi); }

    Impl(const Impl& other) {
        mbedtls_mpi_init(&mpi);
        mbedtls_mpi_copy(&mpi, &other.mpi);
    }
};

BigInteger::BigInteger() : impl_(new Impl) {}
BigInteger::BigInteger(uint64_t value) : impl_(new Impl) {
    mbedtls_mpi_lset(&impl_->mpi, static_cast<mbedtls_mpi_sint>(value));
}
BigInteger::BigInteger(const BigInteger& other) : impl_(new Impl(*other.impl_)) {}
BigInteger::BigInteger(BigInteger&& other) noexcept : impl_(other.impl_) { other.impl_ = new Impl; }
BigInteger::~BigInteger() { delete impl_; }

BigInteger& BigInteger::operator=(const BigInteger& other) {
    if (this != &other) mbedtls_mpi_copy(&impl_->mpi, &other.impl_->mpi);
    return *this;
}

BigInteger& BigInteger::operator=(BigInteger&& other) noexcept {
    if (this != &other) { delete impl_; impl_ = other.impl_; other.impl_ = new Impl; }
    return *this;
}

BigInteger BigInteger::from_string(std::string_view decimal) {
    BigInteger result;
    std::string s(decimal);
    mbedtls_mpi_read_string(&result.impl_->mpi, 10, s.c_str());
    return result;
}

BigInteger BigInteger::from_hex(std::string_view hex) {
    BigInteger result;
    std::string s(hex);
    mbedtls_mpi_read_string(&result.impl_->mpi, 16, s.c_str());
    return result;
}

std::string BigInteger::to_string() const {
    size_t olen = 0;
    mbedtls_mpi_write_string(&impl_->mpi, 10, nullptr, 0, &olen);
    std::vector<char> buf(olen);
    mbedtls_mpi_write_string(&impl_->mpi, 10, buf.data(), olen, &olen);
    return std::string(buf.data());  // C-string constructor stops at null
}

std::string BigInteger::to_hex() const {
    size_t olen = 0;
    mbedtls_mpi_write_string(&impl_->mpi, 16, nullptr, 0, &olen);
    std::vector<char> buf(olen);
    mbedtls_mpi_write_string(&impl_->mpi, 16, buf.data(), olen, &olen);
    return std::string(buf.data());
}

bool BigInteger::is_zero() const {
    return mbedtls_mpi_cmp_int(&impl_->mpi, 0) == 0;
}

size_t BigInteger::bit_count() const {
    return mbedtls_mpi_bitlen(&impl_->mpi);
}

BigInteger BigInteger::operator+(const BigInteger& other) const {
    BigInteger result;
    mbedtls_mpi_add_mpi(&result.impl_->mpi, &impl_->mpi, &other.impl_->mpi);
    return result;
}

BigInteger BigInteger::operator*(const BigInteger& other) const {
    BigInteger result;
    mbedtls_mpi_mul_mpi(&result.impl_->mpi, &impl_->mpi, &other.impl_->mpi);
    return result;
}

BigInteger BigInteger::operator%(const BigInteger& other) const {
    BigInteger result;
    mbedtls_mpi_mod_mpi(&result.impl_->mpi, &impl_->mpi, &other.impl_->mpi);
    return result;
}

BigInteger BigInteger::mod_pow(const BigInteger& exp, const BigInteger& modulus) const {
    BigInteger result;
    mbedtls_mpi_exp_mod(&result.impl_->mpi, &impl_->mpi, &exp.impl_->mpi,
                         &modulus.impl_->mpi, nullptr);
    return result;
}

bool BigInteger::operator==(const BigInteger& other) const {
    return mbedtls_mpi_cmp_mpi(&impl_->mpi, &other.impl_->mpi) == 0;
}

bool BigInteger::operator!=(const BigInteger& other) const {
    return !(*this == other);
}

bool BigInteger::operator<(const BigInteger& other) const {
    return mbedtls_mpi_cmp_mpi(&impl_->mpi, &other.impl_->mpi) < 0;
}

}  // namespace pulp::runtime

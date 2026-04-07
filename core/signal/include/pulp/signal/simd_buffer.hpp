#pragma once

// SIMD-friendly aligned buffer for audio DSP
// Allocates memory aligned to SIMD register width for optimal vectorized access.

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <algorithm>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace pulp::signal {

/// Alignment in bytes for SIMD operations (covers AVX-512)
static constexpr size_t kSimdAlignment = 64;

/// Portable aligned allocation (MSVC uses _aligned_malloc, POSIX uses aligned_alloc)
inline void* aligned_alloc_portable(size_t alignment, size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, size);
#endif
}

inline void aligned_free_portable(void* ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

/// RAII aligned buffer with SIMD-friendly access
class AlignedBuffer {
public:
    AlignedBuffer() = default;

    explicit AlignedBuffer(size_t num_samples)
        : size_(num_samples) {
        if (num_samples > 0) {
            // Round up to alignment boundary
            size_t bytes = num_samples * sizeof(float);
            data_ = static_cast<float*>(aligned_alloc_portable(kSimdAlignment, align_up(bytes)));
            std::memset(data_, 0, align_up(bytes));
        }
    }

    ~AlignedBuffer() { aligned_free_portable(data_); }

    // Move only
    AlignedBuffer(AlignedBuffer&& other) noexcept
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) {
            aligned_free_portable(data_);
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    float* data() { return data_; }
    const float* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    float& operator[](size_t i) { return data_[i]; }
    const float& operator[](size_t i) const { return data_[i]; }

    float* begin() { return data_; }
    float* end() { return data_ + size_; }
    const float* begin() const { return data_; }
    const float* end() const { return data_ + size_; }

    /// Clear buffer to zero
    void clear() {
        if (data_)
            std::memset(data_, 0, size_ * sizeof(float));
    }

    /// Resize (reallocates, does not preserve data)
    void resize(size_t new_size) {
        if (new_size == size_) return;
        aligned_free_portable(data_);
        data_ = nullptr;
        size_ = new_size;
        if (new_size > 0) {
            size_t bytes = new_size * sizeof(float);
            data_ = static_cast<float*>(aligned_alloc_portable(kSimdAlignment, align_up(bytes)));
            std::memset(data_, 0, align_up(bytes));
        }
    }

    /// Copy from raw pointer
    void copy_from(const float* src, size_t count) {
        count = std::min(count, size_);
        std::memcpy(data_, src, count * sizeof(float));
    }

private:
    static size_t align_up(size_t bytes) {
        return (bytes + kSimdAlignment - 1) & ~(kSimdAlignment - 1);
    }

    float* data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace pulp::signal

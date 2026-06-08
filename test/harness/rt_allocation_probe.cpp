#include "rt_allocation_probe.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace pulp::test {
namespace {
thread_local RtAllocationProbe* g_current_probe = nullptr;
}

RtAllocationProbe::RtAllocationProbe() noexcept
    : previous_(g_current_probe) {
    g_current_probe = this;
}

RtAllocationProbe::~RtAllocationProbe() noexcept {
    g_current_probe = previous_;
}

void rt_allocation_probe_record(std::size_t bytes) noexcept {
    if (g_current_probe == nullptr) return;
    ++g_current_probe->allocation_count_;
    g_current_probe->allocated_bytes_ += bytes;
}

bool rt_allocation_probe_active() noexcept {
    return g_current_probe != nullptr;
}

}  // namespace pulp::test

namespace {

void record_allocation(std::size_t bytes) noexcept {
    pulp::test::rt_allocation_probe_record(bytes);
}

void* allocate_unaligned(std::size_t bytes) {
    record_allocation(bytes);
    if (bytes == 0) bytes = 1;
    void* ptr = std::malloc(bytes);
    if (ptr == nullptr) throw std::bad_alloc();
    return ptr;
}

void* allocate_unaligned_nothrow(std::size_t bytes) noexcept {
    record_allocation(bytes);
    if (bytes == 0) bytes = 1;
    return std::malloc(bytes);
}

void free_unaligned(void* ptr) noexcept {
    std::free(ptr);
}

void* allocate_aligned(std::size_t bytes, std::align_val_t alignment) {
    record_allocation(bytes);
    if (bytes == 0) bytes = 1;
    auto align = static_cast<std::size_t>(alignment);
    if (align < sizeof(void*)) align = sizeof(void*);
#if defined(_WIN32)
    void* ptr = _aligned_malloc(bytes, align);
    if (ptr == nullptr) throw std::bad_alloc();
    return ptr;
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, align, bytes) != 0) throw std::bad_alloc();
    return ptr;
#endif
}

void* allocate_aligned_nothrow(std::size_t bytes,
                               std::align_val_t alignment) noexcept {
    record_allocation(bytes);
    if (bytes == 0) bytes = 1;
    auto align = static_cast<std::size_t>(alignment);
    if (align < sizeof(void*)) align = sizeof(void*);
#if defined(_WIN32)
    return _aligned_malloc(bytes, align);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, align, bytes) != 0) return nullptr;
    return ptr;
#endif
}

void free_aligned(void* ptr) noexcept {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

}  // namespace

void* operator new(std::size_t bytes) {
    return allocate_unaligned(bytes);
}

void* operator new[](std::size_t bytes) {
    return allocate_unaligned(bytes);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    return allocate_unaligned_nothrow(bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    return allocate_unaligned_nothrow(bytes);
}

void operator delete(void* ptr) noexcept {
    free_unaligned(ptr);
}

void operator delete[](void* ptr) noexcept {
    free_unaligned(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    free_unaligned(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    free_unaligned(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    free_unaligned(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    free_unaligned(ptr);
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, alignment);
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, alignment);
}

void* operator new(std::size_t bytes,
                   std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    return allocate_aligned_nothrow(bytes, alignment);
}

void* operator new[](std::size_t bytes,
                     std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    return allocate_aligned_nothrow(bytes, alignment);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
    free_aligned(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
    free_aligned(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    free_aligned(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    free_aligned(ptr);
}

void operator delete(void* ptr,
                     std::align_val_t,
                     const std::nothrow_t&) noexcept {
    free_aligned(ptr);
}

void operator delete[](void* ptr,
                       std::align_val_t,
                       const std::nothrow_t&) noexcept {
    free_aligned(ptr);
}

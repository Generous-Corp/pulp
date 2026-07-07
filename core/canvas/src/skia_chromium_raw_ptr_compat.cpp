#include <cstdint>
#include <limits>

#if defined(__linux__) && UINTPTR_MAX > 0xffffffffULL

// Skia m151 Linux prebuilts can reference Chromium BackupRefPtr and
// PartitionAlloc support symbols even though the standalone bundle does not
// ship PartitionAlloc. Pulp does not use Chromium PartitionAlloc, so these
// definitions make Skia's raw_ptr use behave like ordinary pointer storage.
namespace {
constexpr auto kUninitializedPoolBaseAddress = static_cast<std::uintptr_t>(-1);
}

namespace partition_alloc::internal {

class PartitionAddressSpace {
public:
    struct alignas(64) PoolSetup {
        std::uintptr_t regular_pool_base_address_ = kUninitializedPoolBaseAddress;
        std::uintptr_t brp_pool_base_address_ = kUninitializedPoolBaseAddress;
        std::uintptr_t configurable_pool_base_address_ = kUninitializedPoolBaseAddress;
        std::uintptr_t thread_isolated_pool_base_address_ = kUninitializedPoolBaseAddress;
        std::uintptr_t core_pools_base_mask_ = 0;
        std::uintptr_t glued_pools_base_mask_ = 0;
        std::uintptr_t configurable_pool_base_mask_ = 0;
        std::uintptr_t reserved_ = 0;
    };

    static PoolSetup setup_;
};

PartitionAddressSpace::PoolSetup PartitionAddressSpace::setup_;

}  // namespace partition_alloc::internal

namespace base::internal {

template <bool AllowDangling>
struct RawPtrBackupRefImpl {
    static void AcquireInternal(unsigned long address);
    static void ReleaseInternal(unsigned long address);
};

static_assert(sizeof(unsigned long) == sizeof(std::uintptr_t));

template <>
void RawPtrBackupRefImpl<false>::AcquireInternal(unsigned long) {}

template <>
void RawPtrBackupRefImpl<false>::ReleaseInternal(unsigned long) {}

}  // namespace base::internal

#endif

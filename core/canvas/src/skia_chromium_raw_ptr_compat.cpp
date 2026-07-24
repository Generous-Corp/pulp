#include <cstdint>
#include <limits>

#if (defined(__linux__) || defined(_WIN32)) && UINTPTR_MAX > 0xffffffffULL

// Skia m151 Linux and Windows prebuilts can reference Chromium BackupRefPtr and
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

private:
    static PoolSetup setup_;
};

PartitionAddressSpace::PoolSetup PartitionAddressSpace::setup_;

}  // namespace partition_alloc::internal

namespace base::internal {

template <bool AllowDangling>
class RawPtrBackupRefImpl {
    static void AcquireInternal(std::uintptr_t address);
    static void ReleaseInternal(std::uintptr_t address);
};

template <>
void RawPtrBackupRefImpl<false>::AcquireInternal(std::uintptr_t) {}

template <>
void RawPtrBackupRefImpl<false>::ReleaseInternal(std::uintptr_t) {}

}  // namespace base::internal

#endif

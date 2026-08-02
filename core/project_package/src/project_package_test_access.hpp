#pragma once

#include <cstdint>

namespace pulp::project_package::detail {

enum class PackageFaultPoint : std::uint8_t {
    StagedFileWritten,
    StagedFileFenced,
    ExistingBlobVerified,
    BlobReferenceVerified,
    BlobPublished,
    BlobDirectoryFenced,
    GenerationWritten,
    GenerationFenced,
    GenerationPublished,
    GenerationDirectoryFenced,
    DirectoryTreeFenced,
    DestinationPublishedBeforePermissionAdoption,
    DirectoryPublished,
    BlobHashSnapshot,
    ReferenceSetVerified,
    PublicationSourceVerified,
};

using PackageFaultHook = void (*)(PackageFaultPoint) noexcept;

class ProjectPackageTestAccess {
  public:
    static void set_fault_hook(PackageFaultHook hook) noexcept;
    static void clear_fault_hook() noexcept;
#if defined(PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS)
    static void set_skip_reference_validation_for_test(bool skip) noexcept;
#endif
};

void invoke_fault_hook(PackageFaultPoint point) noexcept;
#if defined(PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS)
bool skip_reference_validation_for_test() noexcept;
#endif

} // namespace pulp::project_package::detail

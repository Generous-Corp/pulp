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
    ReaderRootAnchored,
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
#if defined(_WIN32)
/// Whether a private-staging parent owned by `owner` (a PSID) is acceptable to
/// the caller whose token user is `token_user`: the caller itself, SYSTEM, or
/// BUILTIN\Administrators.
bool staging_parent_owner_trusted(void* owner, void* token_user) noexcept;
#endif
#if defined(PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS)
bool skip_reference_validation_for_test() noexcept;
#endif

} // namespace pulp::project_package::detail

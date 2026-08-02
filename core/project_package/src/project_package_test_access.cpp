#include "project_package_test_access.hpp"

#include <atomic>

namespace pulp::project_package::detail {
namespace {
std::atomic<PackageFaultHook> g_fault_hook{nullptr};
#if defined(PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS)
std::atomic<bool> g_skip_reference_validation{false};
#endif
} // namespace

void ProjectPackageTestAccess::set_fault_hook(PackageFaultHook hook) noexcept {
    g_fault_hook.store(hook, std::memory_order_release);
}

void ProjectPackageTestAccess::clear_fault_hook() noexcept {
    g_fault_hook.store(nullptr, std::memory_order_release);
}

#if defined(PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS)
void ProjectPackageTestAccess::set_skip_reference_validation_for_test(bool skip) noexcept {
    g_skip_reference_validation.store(skip, std::memory_order_release);
}
#endif

void invoke_fault_hook(PackageFaultPoint point) noexcept {
    if (const auto hook = g_fault_hook.load(std::memory_order_acquire))
        hook(point);
}

#if defined(PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS)
bool skip_reference_validation_for_test() noexcept {
    return g_skip_reference_validation.load(std::memory_order_acquire);
}
#endif

} // namespace pulp::project_package::detail

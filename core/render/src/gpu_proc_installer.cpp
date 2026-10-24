#include <pulp/render/gpu_proc_installer.hpp>

#include "gpu_proc_installer_registry.hpp"

#include <pulp/runtime/log.hpp>

namespace pulp::render {

namespace detail {
#if defined(PULP_DAWN_PROCS_HOST_OWNED)
// The embedding host owns Dawn's proc table, so Pulp has no installer of its
// own: until the host registers one, every Dawn object creation fails.
bool refuse_unowned_dawn_procs(void* /*context*/, std::string* error) {
    if (error != nullptr)
        *error = "Dawn proc table is host-owned (PULP_DAWN_PROCS_HOST_OWNED) and no host "
                 "installer is registered; call set_dawn_proc_installer() before creating "
                 "any GpuSurface or GpuCompute";
    return false;
}
#else
// Defined in gpu_proc_installer_native.cpp, the one device source that calls
// Dawn's proc-table setter.
bool install_native_dawn_procs(void* context, std::string* error);
#endif
} // namespace detail

namespace {

detail::DawnProcInstallerRegistry& registry() {
#if defined(PULP_DAWN_PROCS_HOST_OWNED)
    static detail::DawnProcInstallerRegistry instance(
        DawnProcInstaller{&detail::refuse_unowned_dawn_procs, nullptr});
#else
    static detail::DawnProcInstallerRegistry instance(
        DawnProcInstaller{&detail::install_native_dawn_procs, nullptr});
#endif
    return instance;
}

} // namespace

bool set_dawn_proc_installer(const DawnProcInstaller& installer, std::string* error) {
    std::string reason;
    if (registry().set(installer, &reason))
        return true;
    runtime::log_error("set_dawn_proc_installer: {}", reason);
    if (error != nullptr)
        *error = std::move(reason);
    return false;
}

bool ensure_dawn_procs(std::string* error) {
    return registry().ensure(error);
}

} // namespace pulp::render

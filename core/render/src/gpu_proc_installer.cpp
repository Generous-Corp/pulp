#include <pulp/render/gpu_proc_installer.hpp>

#include "gpu_proc_installer_registry.hpp"

#include <pulp/runtime/log.hpp>

namespace pulp::render {

namespace detail {
// Defined in gpu_proc_installer_native.cpp, the one device source that calls
// Dawn's proc-table setter.
bool install_native_dawn_procs(void* context, std::string* error);
} // namespace detail

namespace {

detail::DawnProcInstallerRegistry& registry() {
    static detail::DawnProcInstallerRegistry instance(
        DawnProcInstaller{&detail::install_native_dawn_procs, nullptr});
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

// The state machine behind set_dawn_proc_installer() / ensure_dawn_procs().
//
// It is a class rather than file-local state so a test can drive a fresh
// instance with a counting default installer; the process has exactly one
// instance (gpu_proc_installer.cpp), constructed with the native default.
// Nothing here names Dawn: the only code that installs the table is the
// default installer the instance is handed.
#pragma once

#include <pulp/render/gpu_proc_installer.hpp>

#include <cstdint>
#include <mutex>
#include <string>

namespace pulp::render::detail {

class DawnProcInstallerRegistry {
public:
    explicit DawnProcInstallerRegistry(DawnProcInstaller default_installer)
        : default_(default_installer) {}

    DawnProcInstallerRegistry(const DawnProcInstallerRegistry&) = delete;
    DawnProcInstallerRegistry& operator=(const DawnProcInstallerRegistry&) = delete;

    bool set(const DawnProcInstaller& installer, std::string* error) {
        if (installer.install == nullptr)
            return fail(error, "Dawn proc installer has no install function");
        std::lock_guard lock(mutex_);
        if (registered_.install == installer.install && registered_.context == installer.context)
            return true;
        if (sealed_)
            return fail(error, "Dawn proc installer registered after a GpuSurface or GpuCompute "
                               "already resolved the proc table; register it before creating "
                               "any Dawn object");
        if (registered_.install != nullptr)
            return fail(error, "a different Dawn proc installer is already registered");
        registered_ = installer;
        return true;
    }

    bool ensure(std::string* error) {
        if (installing_ == this)
            return fail(error, "Dawn proc installer re-entered ensure_dawn_procs()");
        std::lock_guard lock(mutex_);
        sealed_ = true;
        if (ready_)
            return true;
        const DawnProcInstaller& installer =
            registered_.install != nullptr ? registered_ : default_;
        if (installer.install == nullptr)
            return fail(error, "no Dawn proc installer is available");
        std::string reason;
        struct InstallingScope {
            explicit InstallingScope(const DawnProcInstallerRegistry* self) { installing_ = self; }
            ~InstallingScope() { installing_ = nullptr; }
        } scope(this);
        ++install_calls_;
        const bool ok = installer.install(installer.context, &reason);
        if (!ok)
            return fail(error, reason.empty() ? "Dawn proc installer reported failure" : reason);
        ready_ = true;
        return true;
    }

    /// How many times an installer (registered or default) has run.
    std::uint64_t install_calls() const {
        std::lock_guard lock(mutex_);
        return install_calls_;
    }

private:
    static bool fail(std::string* error, std::string reason) {
        if (error != nullptr)
            *error = std::move(reason);
        return false;
    }

    mutable std::mutex mutex_;
    const DawnProcInstaller default_;
    DawnProcInstaller registered_{};
    bool sealed_ = false;
    bool ready_ = false;
    std::uint64_t install_calls_ = 0;
    // Set on the thread running an installer, so an installer that creates a
    // Dawn object fails instead of deadlocking on mutex_.
    static inline thread_local const DawnProcInstallerRegistry* installing_ = nullptr;
};

} // namespace pulp::render::detail

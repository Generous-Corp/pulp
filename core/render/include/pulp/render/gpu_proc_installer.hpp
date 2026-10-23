// gpu_proc_installer.hpp — who installs Dawn's process-global proc table.
//
// Dawn's C++ wrappers dispatch every call through one process-wide table of
// function pointers that must be installed before the first Dawn object is
// created. GpuSurface and GpuCompute never install it themselves: each calls
// ensure_dawn_procs() first, and that resolves the process's installer exactly
// once.
//
// By default the installer is Pulp's own: it installs Dawn's native procs once
// per process. An embedding host that owns the table instead (because it
// authenticates the Dawn build, shares the table with other code, or installs
// it itself) registers its installer with set_dawn_proc_installer() before any
// GpuSurface or GpuCompute is created. An installer may install the table, or
// only verify that the host already did and report failure otherwise; Pulp
// treats both the same way and creates no Dawn object until one returns true.
//
// The header names no Dawn type, so it stays usable from code that links the
// GPU device API alone.
#pragma once

#include <string>

namespace pulp::render {

/// A host-supplied installer for Dawn's process-global proc table.
///
/// `install` returns true when Dawn calls may proceed, either because it
/// installed the table or because it verified that it is already installed.
/// On false it may write a human-readable reason to `*error` (which may be
/// null). It is called with an internal lock held, at most once concurrently,
/// and must not create a GpuSurface or GpuCompute itself.
struct DawnProcInstaller {
    bool (*install)(void* context, std::string* error) = nullptr;
    void* context = nullptr;
};

/// Register the process's Dawn proc-table installer in place of Pulp's
/// native default.
///
/// Call it before the first GpuSurface or GpuCompute is created. Registering
/// the same installer and context again is a no-op that returns true. Returns
/// false, logs an error, and leaves the current policy in place when
/// `installer.install` is null, when a different installer is already
/// registered, or when a Dawn object creation has already consulted the
/// policy: a late registration is a host ordering bug, never silently ignored.
[[nodiscard]] bool set_dawn_proc_installer(const DawnProcInstaller& installer,
                                           std::string* error = nullptr);

/// Make Dawn's proc table usable before creating a Dawn object.
///
/// The first call fixes the policy (later set_dawn_proc_installer() calls are
/// rejected) and runs the registered installer, or the native default. Once
/// an installer reports success, every later call returns true without
/// running it again. A failed attempt is not latched: the next call retries,
/// so a verify-only host can finish its own installation first. Thread-safe.
[[nodiscard]] bool ensure_dawn_procs(std::string* error = nullptr);

} // namespace pulp::render

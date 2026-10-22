#pragma once

// Where the GPU fault-continuity plug-in finds its run configuration (WP-K).
//
// REAPER is launched by the harness, so the run is configured through
// PULP_GPU_FAULT_* environment variables. Other hosts cannot pass them on:
// Logic and GarageBand are opened through LaunchServices, an AU loaded out of
// process runs in AUHostingService (an XPC service started by launchd), and
// Bitwig may host plug-ins in a separate process. For those hosts the plug-in
// reads a rendezvous file at a fixed path that every one of those processes
// may read and write:
//
//   <account home>/Library/Application Support/Pulp/gpu-fault-continuity/active.json
//
// AUHostingService's sandbox profile allows ~/Library/Application Support and
// denies ~/Library/Containers and ~/Library/Group Containers, so a container
// or app-group path would not work there. Inside a sandboxed host HOME is the
// app's container, so the root comes from the account database, not HOME.
//
// The environment, when set, always wins. The rendezvous is accepted only if
// it is unexpired, carries a nonce, and names a run directory under
// <root>/runs/ that holds the request and the impulse response, so a stale or
// hand-edited file cannot point the plug-in at an arbitrary path.
//
// Test-only: compiled only into the fault-continuity plug-in, which is built
// only with -DPULP_GPU_AUDIO_FAULT_HOOKS=ON.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace pulp::examples::gpu_fault {

inline constexpr const char* kRendezvousSchema = "pulp.gpu-fault.rendezvous.v1";

struct RunConfig {
    std::filesystem::path run_dir;
    std::filesystem::path request;
    std::filesystem::path ir;
    std::string nonce;
    // Staged GPU path instead of shared I/O (the gpu_off path negative, and
    // the staged rows): env PULP_GPU_FAULT_PROVIDER=staged or rendezvous
    // "provider":"staged".
    bool provider_staged = false;
    std::string source = "none"; // "env", "rendezvous" or "none"
    std::string detail;          // why the rendezvous was refused, when it was
};

// Lookup seams, so tests can supply their own environment, root and clock.
struct RunConfigSources {
    std::function<const char*(const char*)> getenv;
    std::filesystem::path rendezvous_root;
    std::int64_t now_unix = 0;
};

// The rendezvous root for this account (see above). Honors
// PULP_GPU_FAULT_RENDEZVOUS_ROOT so tests never touch the real one.
std::filesystem::path default_rendezvous_root();

// The run configuration for this process: environment first, then the
// rendezvous file. A refused rendezvous yields source "none" and a detail.
RunConfig resolve_run_config(const RunConfigSources& sources);
RunConfig resolve_run_config();

// Which process the plug-in is in, so a receipt can bind an out-of-process
// instance (AUHostingService, a Bitwig plug-in host) to the host app that
// owns it.
struct HostProcess {
    int pid = 0;
    int ppid = 0;
    int responsible_pid = -1; // -1: unknown
    int sandboxed = -1;       // -1: unknown, 0: no, 1: yes
    std::string path;
};

HostProcess current_host_process();

// One JSON line: {"event":"host_process",...}.
std::string host_process_event(const HostProcess& process, const RunConfig& config);

} // namespace pulp::examples::gpu_fault

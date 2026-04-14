#pragma once

// Background plugin scanning (workstream 03 slice 3.2).
//
// Wraps any scan function in a worker-thread driver with cooperative
// cancellation and progress callbacks. Settings-panel UIs subscribe to
// progress events, run a scan without blocking the UI thread, and cancel
// mid-flight when the user closes the dialog or adds/removes a search
// path.
//
// Not tied to PluginScanner concretely — accepts any callable of shape
//   std::vector<PluginInfo>(const CancelToken&, ProgressSink).
// This keeps tests fast (fake scanners) and lets consumers swap in
// cached-scan paths later.

#include <pulp/host/scanner.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulp::host {

/// Shared cancel flag. Worker function polls `requested()` at loop
/// boundaries; callers invoke `request()` to stop the scan early.
class CancelToken {
public:
    void request() { flag_.store(true, std::memory_order_release); }
    bool requested() const { return flag_.load(std::memory_order_acquire); }
    /// Clear the flag for a fresh run. Call only from the thread that
    /// owns the token (BackgroundScanner does this under its start lock).
    void reset() { flag_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> flag_{false};
};

/// Progress callback invoked from the worker thread. Callers should
/// marshal to the UI thread before touching widgets.
using ScanProgressCallback = std::function<void(
    const std::string& current_path, int scanned, int total)>;

/// Completion callback invoked when the scan finishes (or cancel lands).
/// `cancelled == true` means the worker observed a cancel before finishing.
using ScanCompletionCallback = std::function<void(
    std::vector<PluginInfo> results, bool cancelled)>;

/// Worker signature: runs on the background thread. Must cooperatively
/// check `token.requested()` between plugin inspections and return early
/// when set. `progress` is optional.
using ScanWorkerFn = std::function<std::vector<PluginInfo>(
    const CancelToken& token, const ScanProgressCallback& progress)>;

class BackgroundScanner {
public:
    BackgroundScanner() = default;
    ~BackgroundScanner() { stop_and_join(); }

    BackgroundScanner(const BackgroundScanner&) = delete;
    BackgroundScanner& operator=(const BackgroundScanner&) = delete;

    /// Start a scan. Returns false if a scan is already running.
    bool start(ScanWorkerFn worker,
               ScanProgressCallback progress,
               ScanCompletionCallback completion);

    /// Request the running scan cancel. No-op if nothing is running.
    /// The completion callback still fires (with `cancelled = true`).
    void cancel();

    /// True while the worker thread is active.
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    /// Block until the worker thread finishes. Safe to call when idle.
    void join();

private:
    void stop_and_join();

    std::thread worker_;
    CancelToken token_;
    std::atomic<bool> running_{false};
    std::mutex start_mutex_;  // serialises start() vs stop_and_join()
};

} // namespace pulp::host

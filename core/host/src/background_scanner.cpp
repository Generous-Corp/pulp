#include <pulp/host/background_scanner.hpp>

namespace pulp::host {

bool BackgroundScanner::start(ScanWorkerFn worker,
                              ScanProgressCallback progress,
                              ScanCompletionCallback completion) {
    std::lock_guard<std::mutex> lock(start_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }
    // A completed-but-not-joined worker leaves worker_ joinable. Assigning
    // a fresh std::thread over a joinable one triggers std::terminate,
    // which a "scan finishes → start another scan" flow would otherwise
    // crash through. Join first so the sequence is always safe regardless
    // of whether the caller explicitly joined.
    if (worker_.joinable()) worker_.join();

    // Reset token for a fresh run (std::atomic isn't copy-assignable, so
    // clear the flag in place rather than reassigning).
    token_.reset();

    running_.store(true, std::memory_order_release);

    // std::thread will drop the last captured copies of `worker` etc. on
    // completion; capture by value so they outlive the caller.
    worker_ = std::thread([this,
                           worker = std::move(worker),
                           progress = std::move(progress),
                           completion = std::move(completion)]() mutable {
        std::vector<PluginInfo> results;
        if (worker) {
            results = worker(token_, progress);
        }
        bool cancelled = token_.requested();
        if (completion) {
            completion(std::move(results), cancelled);
        }
        running_.store(false, std::memory_order_release);
    });
    return true;
}

void BackgroundScanner::cancel() {
    token_.request();
}

void BackgroundScanner::join() {
    if (worker_.joinable()) worker_.join();
}

void BackgroundScanner::stop_and_join() {
    token_.request();
    if (worker_.joinable()) worker_.join();
}

} // namespace pulp::host

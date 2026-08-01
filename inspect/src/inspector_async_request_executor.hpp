#pragma once

#include <pulp/inspect/protocol.hpp>

#include "inspector_connected_client.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace pulp::inspect::detail {

class InspectorAsyncRequestExecutor final
    : public std::enable_shared_from_this<InspectorAsyncRequestExecutor> {
public:
    struct Request {
        std::shared_ptr<InspectorConnectedClient> client;
        InspectorMessage message;
        std::uint64_t generation = 0;
    };

    struct Callbacks {
        std::function<std::shared_ptr<void>()> acquire_iteration_lifetime;
        std::function<void(const Request&)> execute;
        std::function<void()> reserve_worker;
        std::function<void()> cancel_worker_reservation;
        std::function<void()> worker_started;
        std::function<void()> worker_exited;
    };

    static std::shared_ptr<InspectorAsyncRequestExecutor> create(
        Callbacks callbacks);

    void configure(std::size_t max_requests);
    bool submit(Request request);
    bool cancel_queued_for_client(
        const std::shared_ptr<InspectorConnectedClient>& client,
        std::function<void()> deferred_cleanup);
    void clear();
    void stop(bool detach_worker);
    bool on_worker_thread() const noexcept;

private:
    explicit InspectorAsyncRequestExecutor(Callbacks callbacks)
        : callbacks_(std::move(callbacks)) {}

    void start();
    void run();

    Callbacks callbacks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::shared_ptr<InspectorConnectedClient> active_client_;
    std::unordered_map<InspectorConnectedClient*, std::function<void()>>
        deferred_cleanups_;
    std::thread worker_;
    std::atomic<bool> stop_claimed_{false};
    bool stopping_ = false;
    bool request_active_ = false;
    std::size_t max_requests_ = 64;
};

} // namespace pulp::inspect::detail

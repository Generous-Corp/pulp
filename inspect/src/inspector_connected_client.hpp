#pragma once

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/authentication.hpp>

#include "bounded_event_queue.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pulp::inspect::detail {

class InspectorOutboundClient
    : public std::enable_shared_from_this<InspectorOutboundClient> {
public:
    static std::shared_ptr<InspectorOutboundClient> create(
        std::shared_ptr<events::InterprocessConnection> connection) {
        auto result = std::shared_ptr<InspectorOutboundClient>(
            new InspectorOutboundClient(std::move(connection)));
        result->worker_ = std::thread([result] { result->run(); });
        return result;
    }

    EventQueuePushResult enqueue(std::string message, bool lossy) {
        EventQueuePushResult result;
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
                return EventQueuePushResult::DroppedLossy;
            result = messages_.push(std::move(message), lossy);
        }
        if (result == EventQueuePushResult::Queued)
            cv_.notify_one();
        return result;
    }

    void request_stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            messages_.clear();
        }
        cv_.notify_all();
    }

    void shutdown() {
        request_stop();
        if (worker_.joinable() &&
            worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
        } else if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    explicit InspectorOutboundClient(
        std::shared_ptr<events::InterprocessConnection> connection)
        : connection_(std::move(connection)) {}

    void run() {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            cv_.wait(lock, [this] {
                return stopping_ || !messages_.empty();
            });
            if (stopping_)
                break;
            auto message = messages_.take_front();
            if (!message)
                continue;
            auto connection = connection_;
            lock.unlock();
            if (connection && !connection->send_message(*message)) {
                connection->disconnect();
                lock.lock();
                stopping_ = true;
                messages_.clear();
                break;
            }
            lock.lock();
        }
    }

    std::shared_ptr<events::InterprocessConnection> connection_;
    std::mutex mutex_;
    std::condition_variable cv_;
    BoundedEventQueue<std::string> messages_{32};
    std::thread worker_;
    bool stopping_ = false;
};

struct InspectorConnectedClient {
    static std::shared_ptr<InspectorConnectedClient> create(
        std::shared_ptr<events::InterprocessConnection> connection,
        std::unique_ptr<InspectorAuthVerifier> verifier,
        std::string client_id,
        std::chrono::steady_clock::time_point deadline,
        std::uint64_t generation) {
        auto result = std::make_shared<InspectorConnectedClient>();
        result->connection = std::move(connection);
        result->verifier = std::move(verifier);
        result->client_id = std::move(client_id);
        result->deadline = deadline;
        result->generation = generation;
        result->outbound =
            InspectorOutboundClient::create(result->connection);
        return result;
    }

    std::shared_ptr<events::InterprocessConnection> connection;
    std::unique_ptr<InspectorAuthVerifier> verifier;
    std::shared_ptr<InspectorOutboundClient> outbound;
    std::string client_id;
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t generation = 0;
    bool authenticated = false;
};

} // namespace pulp::inspect::detail

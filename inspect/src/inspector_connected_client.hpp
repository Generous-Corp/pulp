#pragma once

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/authentication.hpp>

#include "bounded_event_queue.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace pulp::inspect::detail {

class InspectorOutboundClient
    : public std::enable_shared_from_this<InspectorOutboundClient> {
public:
    using SendMessage = std::function<bool(std::string_view)>;

    static std::shared_ptr<InspectorOutboundClient> create(
        std::shared_ptr<events::InterprocessConnection> connection) {
        auto result = std::shared_ptr<InspectorOutboundClient>(
            new InspectorOutboundClient(std::move(connection), {}, 32));
        result->worker_ = std::thread([result] { result->run(); });
        return result;
    }

    // Internal deterministic seam for queue/backpressure tests. Production
    // clients always use create() and the real framed connection.
    static std::shared_ptr<InspectorOutboundClient> create_for_testing(
        SendMessage send_message, std::size_t capacity) {
        auto result = std::shared_ptr<InspectorOutboundClient>(
            new InspectorOutboundClient({}, std::move(send_message), capacity));
        result->worker_ = std::thread([result] { result->run(); });
        return result;
    }

    EventQueuePushResult enqueue(std::string message, bool lossy,
                                 bool* evicted_lossy = nullptr) {
        if (evicted_lossy)
            *evicted_lossy = false;
        EventQueuePushResult result;
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
                return EventQueuePushResult::DroppedLossy;
            result = messages_.push(
                std::move(message), lossy, evicted_lossy);
        }
        if (result == EventQueuePushResult::Queued)
            cv_.notify_one();
        return result;
    }

    EventQueuePushResult enqueue_targeted(
        std::string message, bool lossy, std::string owner,
        bool* evicted_same_owner = nullptr) {
        if (evicted_same_owner)
            *evicted_same_owner = false;
        EventQueuePushResult result;
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
                return EventQueuePushResult::DroppedLossy;
            result = targeted_messages_.push_isolated(
                std::move(message), lossy, std::move(owner),
                evicted_same_owner);
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
            targeted_messages_.clear();
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
        std::shared_ptr<events::InterprocessConnection> connection,
        SendMessage send_message,
        std::size_t capacity)
        : connection_(std::move(connection)),
          send_message_(std::move(send_message)),
          messages_(capacity),
          targeted_messages_(capacity) {}

    void run() {
        std::unique_lock lock(mutex_);
        bool prefer_targeted = false;
        while (!stopping_) {
            cv_.wait(lock, [this] {
                return stopping_ || !messages_.empty() ||
                       !targeted_messages_.empty();
            });
            if (stopping_)
                break;
            std::optional<std::string> message;
            if (!messages_.empty() && !targeted_messages_.empty()) {
                message = prefer_targeted
                    ? targeted_messages_.take_front()
                    : messages_.take_front();
                prefer_targeted = !prefer_targeted;
            } else if (!messages_.empty()) {
                message = messages_.take_front();
                prefer_targeted = true;
            } else {
                message = targeted_messages_.take_front();
                prefer_targeted = false;
            }
            if (!message)
                continue;
            auto connection = connection_;
            lock.unlock();
            const auto sent = send_message_
                ? send_message_(*message)
                : connection && connection->send_message(*message);
            if (!sent) {
                if (connection)
                    connection->disconnect();
                lock.lock();
                stopping_ = true;
                messages_.clear();
                targeted_messages_.clear();
                break;
            }
            lock.lock();
        }
    }

    std::shared_ptr<events::InterprocessConnection> connection_;
    SendMessage send_message_;
    std::mutex mutex_;
    std::condition_variable cv_;
    BoundedEventQueue<std::string> messages_;
    BoundedEventQueue<std::string> targeted_messages_;
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

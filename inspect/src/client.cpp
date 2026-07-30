#include <pulp/inspect/client.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/authentication.hpp>

#include <choc/text/choc_JSON.h>

#include "bounded_event_queue.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace pulp::inspect {
namespace {

InspectorMessage connection_error(std::int64_t request_id,
                                  std::string message,
                                  std::string code,
                                  std::string data_json = "{}") {
    return make_error(request_id,
                      std::move(message),
                      std::move(code),
                      std::move(data_json));
}

} // namespace

class InspectorClient::Impl {
public:
    struct QueuedEvent {
        InspectorMessage message;
        std::uint64_t generation = 0;
    };

    struct EventState {
        std::mutex mutex;
        std::condition_variable cv;
        detail::BoundedEventQueue<QueuedEvent> events{256};
        EventHandler handler;
        std::thread::id callback_thread;
        std::uint64_t connection_generation = 0;
        std::uint64_t callback_generation = 0;
        bool callback_active = false;
        bool stopping = false;
    };

    events::InterprocessConnection connection;
    std::mutex send_mutex;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::int64_t, InspectorMessage> responses;
    std::set<std::int64_t> in_flight;
    std::optional<InspectorAuthChallenge> challenge;
    std::shared_ptr<EventState> event_state = std::make_shared<EventState>();
    std::thread event_thread;
    std::atomic<std::int64_t> next_request_id{2};
    std::uint64_t connection_generation = 0;
    bool mutually_authenticated = false;
    bool disconnected = true;

    Impl() {
        const auto state = event_state;
        event_thread = std::thread([state] {
            std::unique_lock lock(state->mutex);
            while (!state->stopping) {
                state->cv.wait(lock, [&] {
                    return state->stopping || !state->events.empty();
                });
                if (state->stopping)
                    break;
                auto event = state->events.take_front();
                if (!event)
                    continue;
                auto handler = state->handler;
                state->callback_thread = std::this_thread::get_id();
                state->callback_generation = event->generation;
                state->callback_active = true;
                lock.unlock();
                if (handler) {
                    try {
                        handler(event->message);
                    } catch (...) {
                    }
                }
                lock.lock();
                state->callback_active = false;
                state->callback_thread = {};
            }
        });
    }

    ~Impl() {
        disconnect_current(true);
        {
            std::lock_guard lock(event_state->mutex);
            event_state->stopping = true;
            event_state->events.clear();
        }
        event_state->cv.notify_all();
        if (event_thread.joinable() &&
            event_thread.get_id() == std::this_thread::get_id())
            event_thread.detach();
        else if (event_thread.joinable())
            event_thread.join();
    }

    void receive(std::string_view text, std::uint64_t generation) {
        InspectorMessage message;
        if (!decode_message(std::string(text), message))
            return;

        {
            std::lock_guard lock(mutex);
            if (disconnected || generation != connection_generation)
                return;
            if (message.method == "Session.authChallenge") {
                if (mutually_authenticated || challenge)
                    return;
                try {
                    const auto value = choc::json::parse(message.params_json);
                    InspectorAuthChallenge parsed;
                    parsed.scheme = std::string(value["scheme"].getString());
                    parsed.nonce_hex =
                        std::string(value["nonce"].getString());
                    parsed.session_id =
                        std::string(value["sessionId"].getString());
                    parsed.instance_id =
                        std::string(value["instanceId"].getString());
                    parsed.protocol_version =
                        std::string(value["protocolVersion"].getString());
                    challenge = std::move(parsed);
                } catch (...) {
                }
                cv.notify_all();
                return;
            }
            if (message.id != 0 && message.method.empty()) {
                if (in_flight.contains(message.id)) {
                    responses.insert_or_assign(message.id, std::move(message));
                    cv.notify_all();
                }
                return;
            }
        }
        {
            std::lock_guard lock(mutex);
            if (!mutually_authenticated)
                return;
        }
        const bool lossy = inspector_event_is_lossy(message.method);
        detail::EventQueuePushResult result;
        {
            std::lock_guard lock(event_state->mutex);
            if (generation != event_state->connection_generation)
                return;
            result = event_state->events.push(
                QueuedEvent{std::move(message), generation}, lossy);
        }
        if (result == detail::EventQueuePushResult::Queued) {
            event_state->cv.notify_one();
        } else if (result ==
                   detail::EventQueuePushResult::ReliableOverflow) {
            connection.disconnect();
        }
    }

    void mark_disconnected(std::uint64_t generation) {
        {
            std::lock_guard lock(mutex);
            if (generation != connection_generation)
                return;
            disconnected = true;
            mutually_authenticated = false;
        }
        cv.notify_all();
    }

    bool request_from_stale_callback() const {
        std::lock_guard lock(event_state->mutex);
        return event_state->callback_active &&
               event_state->callback_thread == std::this_thread::get_id() &&
               event_state->callback_generation !=
                   event_state->connection_generation;
    }

    void disconnect_current(bool clear_callbacks) {
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex);
            generation = ++connection_generation;
            disconnected = true;
            mutually_authenticated = false;
            responses.clear();
            in_flight.clear();
            challenge.reset();
        }
        {
            std::lock_guard lock(event_state->mutex);
            event_state->connection_generation = generation;
            event_state->events.clear();
        }
        if (clear_callbacks) {
            connection.set_on_text_message(
                std::function<void(std::string_view)>{});
            connection.set_on_disconnected(std::function<void()>{});
        }
        connection.disconnect();
        cv.notify_all();
    }

    InspectorMessage wait_for_response(std::int64_t id,
                                       std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, timeout, [&] {
                return responses.contains(id) || disconnected;
            })) {
            in_flight.erase(id);
            lock.unlock();
            // The complete request frame was sent, so a response timeout
            // cannot prove that its operation did not run. Fence this
            // connection before returning the explicit ambiguity marker:
            // late responses are no longer correlated and callers must
            // reconnect instead of retrying on the same authority.
            connection.disconnect();
            return connection_error(id,
                                    "Inspector request timed out; it may have applied",
                                    "request_timeout",
                                    R"({"mayHaveApplied":true})");
        }
        const auto found = responses.find(id);
        if (found == responses.end()) {
            in_flight.erase(id);
            return connection_error(id,
                                    "Inspector connection closed; request may have applied",
                                    "connection_closed",
                                    R"({"mayHaveApplied":true})");
        }
        auto response = std::move(found->second);
        responses.erase(found);
        in_flight.erase(id);
        return response;
    }
};

InspectorClient::InspectorClient() : impl_(std::make_unique<Impl>()) {
    impl_->connection.set_max_message_bytes(1024u * 1024u);
}

InspectorClient::~InspectorClient() = default;

bool InspectorClient::connect(const InspectorDiscoveryRecord& record,
                              const InspectorDiscoveryReader& discovery,
                              std::chrono::milliseconds timeout) {
    if (impl_->request_from_stale_callback())
        return false;
    disconnect();
    const auto token = discovery.read_credential(record);
    if (!token)
        return false;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(impl_->mutex);
        generation = ++impl_->connection_generation;
        impl_->disconnected = false;
        impl_->mutually_authenticated = false;
        impl_->challenge.reset();
        impl_->responses.clear();
        impl_->in_flight.clear();
    }
    {
        std::lock_guard lock(impl_->event_state->mutex);
        impl_->event_state->connection_generation = generation;
    }
    impl_->connection.set_on_text_message(
        [this, generation](std::string_view message) {
            impl_->receive(message, generation);
        });
    impl_->connection.set_on_disconnected([this, generation] {
        impl_->mark_disconnected(generation);
    });
    impl_->connection.set_write_timeout(
        std::max(timeout, std::chrono::milliseconds(1)));
    if (!impl_->connection.connect(record.endpoint,
                                   events::IpcTransport::Socket)) {
        std::lock_guard lock(impl_->mutex);
        impl_->disconnected = true;
        return false;
    }

    InspectorAuthChallenge challenge;
    {
        std::unique_lock lock(impl_->mutex);
        if (!impl_->cv.wait_for(lock, timeout, [&] {
                return impl_->challenge.has_value() || impl_->disconnected;
            }) ||
            !impl_->challenge) {
            lock.unlock();
            impl_->connection.disconnect();
            return false;
        }
        challenge = *impl_->challenge;
    }
    if (challenge.session_id != record.session_id ||
        challenge.instance_id != record.instance_id ||
        challenge.protocol_version != record.protocol_version) {
        impl_->connection.disconnect();
        return false;
    }
    const auto proof =
        make_inspector_auth_proof(token->bytes(), challenge);
    if (!proof) {
        impl_->connection.disconnect();
        return false;
    }
    auto params = choc::value::createObject("");
    params.addMember("proof", choc::value::createString(*proof));
    const auto authentication =
        make_request(1,
                     "Session.authenticate",
                     choc::json::toString(params, false));
    {
        std::lock_guard lock(impl_->mutex);
        impl_->in_flight.insert(authentication.id);
    }
    if (!impl_->connection.send_message(encode_message(authentication))) {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->in_flight.erase(authentication.id);
        }
        impl_->connection.disconnect();
        return false;
    }
    const auto response = impl_->wait_for_response(1, timeout);
    if (response.is_error) {
        impl_->connection.disconnect();
        return false;
    }
    std::string server_proof;
    try {
        const auto result = choc::json::parse(response.params_json);
        server_proof =
            std::string(result["serverProof"].getString());
    } catch (...) {
    }
    if (!verify_inspector_server_auth_proof(
            token->bytes(), challenge, *proof, server_proof)) {
        impl_->connection.disconnect();
        return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->disconnected ||
            impl_->connection_generation != generation) {
            return false;
        }
        impl_->mutually_authenticated = true;
    }
    return true;
}

void InspectorClient::disconnect() {
    if (impl_->request_from_stale_callback())
        return;
    impl_->disconnect_current(false);
}

bool InspectorClient::is_connected() const {
    return impl_->connection.is_connected();
}

InspectorMessage InspectorClient::request(
    std::string method,
    std::string params_json,
    std::chrono::milliseconds timeout) {
    const auto id =
        impl_->next_request_id.fetch_add(1, std::memory_order_relaxed);
    if (impl_->request_from_stale_callback()) {
        return connection_error(
            id,
            "Inspector event callback belongs to a prior connection",
            "stale_event_callback");
    }
    if (!is_connected()) {
        return connection_error(id,
                                "Inspector client is not connected",
                                "not_connected");
    }
    const auto message =
        make_request(id, std::move(method), std::move(params_json));
    bool sent = false;
    {
        std::lock_guard send_lock(impl_->send_mutex);
        impl_->connection.set_write_timeout(
            std::max(timeout, std::chrono::milliseconds(1)));
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->disconnected) {
                return connection_error(id,
                                        "Inspector connection closed",
                                        "connection_closed");
            }
            impl_->in_flight.insert(id);
        }
        sent = impl_->connection.send_message(encode_message(message));
    }
    if (!sent) {
        std::lock_guard lock(impl_->mutex);
        impl_->in_flight.erase(id);
        return connection_error(id,
                                "Inspector request could not be sent",
                                "send_failed");
    }
    return impl_->wait_for_response(id, timeout);
}

void InspectorClient::set_event_handler(EventHandler handler) {
    std::lock_guard lock(impl_->event_state->mutex);
    impl_->event_state->handler = std::move(handler);
}

} // namespace pulp::inspect

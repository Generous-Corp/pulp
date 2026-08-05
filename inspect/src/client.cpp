#include <pulp/inspect/client.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/authentication.hpp>

#include <choc/text/choc_JSON.h>

#include "bounded_event_queue.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
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

InspectorMessage connection_error(std::int64_t request_id, std::string message, std::string code,
                                  std::string data_json = "{}") {
    return make_error(request_id, std::move(message), std::move(code), std::move(data_json));
}

} // namespace

class InspectorClient::Impl {
  public:
    struct QueuedEvent {
        InspectorMessage message;
        std::uint64_t generation = 0;
        std::size_t wire_bytes = 0;
        bool lossy = false;
    };

    struct EventState {
        static constexpr std::size_t max_pre_auth_events = 16;
        static constexpr std::size_t max_pre_auth_bytes = 64u * 1024u;

        std::mutex mutex;
        std::condition_variable cv;
        detail::BoundedEventQueue<QueuedEvent> events{256};
        std::deque<QueuedEvent> pre_auth_events;
        std::size_t pre_auth_bytes = 0;
        EventHandler handler;
        std::thread::id callback_thread;
        std::uint64_t connection_generation = 0;
        std::uint64_t callback_generation = 0;
        bool callback_active = false;
        bool authenticated = false;
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
                    return state->stopping || (state->authenticated && !state->events.empty());
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
        if (event_thread.joinable() && event_thread.get_id() == std::this_thread::get_id())
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
                    parsed.nonce_hex = std::string(value["nonce"].getString());
                    parsed.session_id = std::string(value["sessionId"].getString());
                    parsed.instance_id = std::string(value["instanceId"].getString());
                    parsed.publication_id = std::string(value["publicationId"].getString());
                    parsed.protocol_version = std::string(value["protocolVersion"].getString());
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
        const bool lossy = inspector_event_is_lossy(message.method);
        detail::EventQueuePushResult result;
        bool notify_event = false;
        {
            std::lock_guard lock(event_state->mutex);
            if (generation != event_state->connection_generation)
                return;
            QueuedEvent event{std::move(message), generation, text.size(), lossy};
            if (!event_state->authenticated) {
                if (event_state->pre_auth_events.size() >= EventState::max_pre_auth_events ||
                    event.wire_bytes >
                        EventState::max_pre_auth_bytes - event_state->pre_auth_bytes) {
                    result = lossy ? detail::EventQueuePushResult::DroppedLossy
                                   : detail::EventQueuePushResult::ReliableOverflow;
                } else {
                    event_state->pre_auth_bytes += event.wire_bytes;
                    event_state->pre_auth_events.push_back(std::move(event));
                    result = detail::EventQueuePushResult::Queued;
                }
            } else {
                result = event_state->events.push(std::move(event), lossy);
                notify_event = result == detail::EventQueuePushResult::Queued;
            }
        }
        if (notify_event) {
            event_state->cv.notify_one();
        } else if (result == detail::EventQueuePushResult::ReliableOverflow) {
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
        {
            std::lock_guard lock(event_state->mutex);
            if (generation == event_state->connection_generation) {
                event_state->authenticated = false;
                event_state->events.clear();
                event_state->pre_auth_events.clear();
                event_state->pre_auth_bytes = 0;
            }
        }
        cv.notify_all();
    }

    bool request_from_stale_callback() const {
        std::lock_guard lock(event_state->mutex);
        return event_state->callback_active &&
               event_state->callback_thread == std::this_thread::get_id() &&
               event_state->callback_generation != event_state->connection_generation;
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
            event_state->authenticated = false;
            event_state->events.clear();
            event_state->pre_auth_events.clear();
            event_state->pre_auth_bytes = 0;
        }
        if (clear_callbacks) {
            connection.set_on_text_message(std::function<void(std::string_view)>{});
            connection.set_on_disconnected(std::function<void()>{});
        }
        connection.disconnect();
        cv.notify_all();
    }

    InspectorMessage wait_for_response(std::int64_t id, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, timeout, [&] { return responses.contains(id) || disconnected; })) {
            in_flight.erase(id);
            lock.unlock();
            // The complete request frame was sent, so a response timeout
            // cannot prove that its operation did not run. Fence this
            // connection before returning the explicit ambiguity marker:
            // late responses are no longer correlated and callers must
            // reconnect instead of retrying on the same authority.
            connection.disconnect();
            return connection_error(id, "Inspector request timed out; it may have applied",
                                    "request_timeout", R"({"mayHaveApplied":true})");
        }
        const auto found = responses.find(id);
        if (found == responses.end()) {
            in_flight.erase(id);
            return connection_error(id, "Inspector connection closed; request may have applied",
                                    "connection_closed", R"({"mayHaveApplied":true})");
        }
        auto response = std::move(found->second);
        responses.erase(found);
        in_flight.erase(id);
        return response;
    }
};

InspectorClient::InspectorClient() : impl_(std::make_unique<Impl>()) {
    impl_->connection.set_max_message_bytes(kInspectorExtendedMessageBytes);
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
        impl_->event_state->authenticated = false;
        impl_->event_state->pre_auth_events.clear();
        impl_->event_state->pre_auth_bytes = 0;
    }
    impl_->connection.set_on_text_message(
        [this, generation](std::string_view message) { impl_->receive(message, generation); });
    impl_->connection.set_on_disconnected(
        [this, generation] { impl_->mark_disconnected(generation); });
    const auto bounded_timeout = std::max(timeout, std::chrono::milliseconds(1));
    const auto connect_started = std::chrono::steady_clock::now();
    const auto remaining = [&] {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - connect_started);
        return elapsed >= bounded_timeout ? std::chrono::milliseconds(0)
                                          : bounded_timeout - elapsed;
    };
    impl_->connection.set_write_timeout(bounded_timeout);
    if (!impl_->connection.connect(record.endpoint, events::IpcTransport::Socket,
                                   bounded_timeout)) {
        impl_->disconnect_current(false);
        return false;
    }

    InspectorAuthChallenge challenge;
    {
        const auto challenge_timeout = remaining();
        if (challenge_timeout <= std::chrono::milliseconds(0)) {
            impl_->disconnect_current(false);
            return false;
        }
        std::unique_lock lock(impl_->mutex);
        if (!impl_->cv.wait_for(
                lock, challenge_timeout,
                [&] { return impl_->challenge.has_value() || impl_->disconnected; }) ||
            !impl_->challenge) {
            lock.unlock();
            impl_->disconnect_current(false);
            return false;
        }
        challenge = *impl_->challenge;
    }
    if (challenge.session_id != record.session_id || challenge.instance_id != record.instance_id ||
        challenge.publication_id != record.publication_id ||
        challenge.protocol_version != record.protocol_version) {
        impl_->disconnect_current(false);
        return false;
    }
    const auto proof = make_inspector_auth_proof(token->bytes(), challenge);
    if (!proof) {
        impl_->disconnect_current(false);
        return false;
    }
    auto params = choc::value::createObject("");
    params.addMember("proof", choc::value::createString(*proof));
    const auto authentication =
        make_request(1, "Session.authenticate", choc::json::toString(params, false));
    {
        std::lock_guard lock(impl_->mutex);
        impl_->in_flight.insert(authentication.id);
    }
    const auto authentication_timeout = remaining();
    if (authentication_timeout <= std::chrono::milliseconds(0)) {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->in_flight.erase(authentication.id);
        }
        impl_->disconnect_current(false);
        return false;
    }
    impl_->connection.set_write_timeout(authentication_timeout);
    if (!impl_->connection.send_message(encode_message(authentication))) {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->in_flight.erase(authentication.id);
        }
        impl_->disconnect_current(false);
        return false;
    }
    const auto response = impl_->wait_for_response(1, remaining());
    if (response.is_error) {
        impl_->disconnect_current(false);
        return false;
    }
    std::string server_proof;
    try {
        const auto result = choc::json::parse(response.params_json);
        server_proof = std::string(result["serverProof"].getString());
    } catch (...) {
    }
    if (!verify_inspector_server_auth_proof(token->bytes(), challenge, *proof, server_proof)) {
        impl_->disconnect_current(false);
        return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->disconnected || impl_->connection_generation != generation) {
            return false;
        }
        impl_->mutually_authenticated = true;
    }
    {
        std::lock_guard lock(impl_->event_state->mutex);
        if (impl_->event_state->connection_generation != generation)
            return false;
        for (auto& event : impl_->event_state->pre_auth_events) {
            const bool lossy = event.lossy;
            (void)impl_->event_state->events.push(std::move(event), lossy);
        }
        impl_->event_state->pre_auth_events.clear();
        impl_->event_state->pre_auth_bytes = 0;
        impl_->event_state->authenticated = true;
    }
    impl_->event_state->cv.notify_all();
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

InspectorMessage InspectorClient::request(std::string method, std::string params_json,
                                          std::chrono::milliseconds timeout) {
    const auto id = impl_->next_request_id.fetch_add(1, std::memory_order_relaxed);
    if (impl_->request_from_stale_callback()) {
        return connection_error(id, "Inspector event callback belongs to a prior connection",
                                "stale_event_callback");
    }
    if (!is_connected()) {
        return connection_error(id, "Inspector client is not connected", "not_connected");
    }
    const auto message = make_request(id, std::move(method), std::move(params_json));
    bool sent = false;
    {
        std::lock_guard send_lock(impl_->send_mutex);
        impl_->connection.set_write_timeout(std::max(timeout, std::chrono::milliseconds(1)));
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->disconnected) {
                return connection_error(id, "Inspector connection closed", "connection_closed");
            }
            impl_->in_flight.insert(id);
        }
        sent = impl_->connection.send_message(encode_message(message));
    }
    if (!sent) {
        std::lock_guard lock(impl_->mutex);
        impl_->in_flight.erase(id);
        return connection_error(id, "Inspector request could not be sent", "send_failed");
    }
    return impl_->wait_for_response(id, timeout);
}

void InspectorClient::set_event_handler(EventHandler handler) {
    std::lock_guard lock(impl_->event_state->mutex);
    impl_->event_state->handler = std::move(handler);
}

namespace {

template <typename Operation>
InspectorClientResult run_inspector_operation(InspectorClientTarget target,
                                              std::chrono::milliseconds timeout,
                                              const InspectorDiscoveryReader& discovery,
                                              bool needs_controller, Operation&& operation) {
    InspectorClientResult result;
    const auto deadline =
        std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
    const auto remaining = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return std::chrono::milliseconds(0);
        return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                        std::chrono::milliseconds(1));
    };

    std::string discovery_error;
    const auto records = discovery.list(&discovery_error);
    if (!discovery_error.empty()) {
        auto data = choc::value::createObject("");
        data.addMember("runtimeDirectory",
                       choc::value::createString(discovery.runtime_directory().string()));
        result.response = make_error(0, std::move(discovery_error), "discovery_unavailable",
                                     choc::json::toString(data, false));
        return result;
    }
    std::string selection_error;
    result.publication = select_inspector_session(records, target.session_id, target.instance_id,
                                                  target.publication_id, &selection_error);
    if (!result.publication) {
        auto data = choc::value::createObject("");
        data.addMember("sessionId", choc::value::createString(target.session_id));
        data.addMember("instanceId", choc::value::createString(target.instance_id));
        data.addMember("publicationId", choc::value::createString(target.publication_id));
        result.response = make_error(0,
                                     selection_error.empty() ? "No inspector publication selected"
                                                             : std::move(selection_error),
                                     "selection_failed", choc::json::toString(data, false));
        return result;
    }

    const auto connect_timeout = remaining();
    if (connect_timeout <= std::chrono::milliseconds(0)) {
        result.response = make_error(0, "Inspector operation timed out", "request_timeout",
                                     R"({"mayHaveApplied":false})");
        return result;
    }

    InspectorClient client;
    if (!client.connect(*result.publication, discovery, connect_timeout)) {
        auto data = choc::value::createObject("");
        data.addMember("sessionId", choc::value::createString(result.publication->session_id));
        data.addMember("instanceId", choc::value::createString(result.publication->instance_id));
        data.addMember("publicationId",
                       choc::value::createString(result.publication->publication_id));
        result.response = make_error(0, "Inspector authentication or connection failed",
                                     "connection_failed", choc::json::toString(data, false));
        return result;
    }

    if (needs_controller) {
        const auto lease_timeout = remaining();
        if (lease_timeout <= std::chrono::milliseconds(0)) {
            result.response = make_error(0, "Inspector operation timed out", "request_timeout",
                                         R"({"mayHaveApplied":false})");
            return result;
        }
        const auto lease =
            client.request(std::string(methods::kSessionAcquireController), "{}", lease_timeout);
        if (lease.is_error) {
            result.response = lease;
            return result;
        }
    }

    result.response = operation(client, remaining);
    if (needs_controller) {
        const auto release_timeout = remaining();
        if (release_timeout > std::chrono::milliseconds(0))
            (void)client.request(std::string(methods::kSessionReleaseController), "{}",
                                 release_timeout);
    }
    return result;
}

choc::value::Value midi_params(const MidiTestInput& input) {
    auto params = choc::value::createObject("");
    params.addMember("kind", choc::value::createString(
                                 input.kind == MidiTestInputKind::NoteOn ? "note_on" : "note_off"));
    params.addMember("channel",
                     choc::value::createInt32(static_cast<std::int32_t>(input.channel) + 1));
    params.addMember("note", choc::value::createInt32(input.note));
    params.addMember("velocity", choc::value::createInt32(input.velocity));
    return params;
}

} // namespace

InspectorClientResult request_inspector(std::string method, std::string params_json,
                                        InspectorClientTarget target,
                                        std::chrono::milliseconds timeout,
                                        const InspectorDiscoveryReader& discovery) {
    const auto* descriptor = find_inspector_method(method);
    const bool needs_controller = descriptor && descriptor->kind == InspectorMethodKind::Request &&
                                  capability_requires_controller_lease(descriptor->capability) &&
                                  method != methods::kSessionAcquireController &&
                                  method != methods::kSessionRenewController &&
                                  method != methods::kSessionReleaseController;
    return run_inspector_operation(
        std::move(target), timeout, discovery, needs_controller,
        [method = std::move(method), params_json = std::move(params_json)](
            InspectorClient& client, const auto& remaining) mutable {
            const auto request_timeout = remaining();
            if (request_timeout <= std::chrono::milliseconds(0))
                return make_error(0, "Inspector operation timed out", "request_timeout",
                                  R"({"mayHaveApplied":false})");
            return client.request(std::move(method), std::move(params_json), request_timeout);
        });
}

InspectorClientResult inject_inspector_midi(const MidiTestInput& input,
                                            std::chrono::milliseconds hold_duration,
                                            InspectorClientTarget target,
                                            std::chrono::milliseconds timeout,
                                            const InspectorDiscoveryReader& discovery) {
    if ((input.kind != MidiTestInputKind::NoteOn && input.kind != MidiTestInputKind::NoteOff) ||
        input.channel > 15 || input.note > 127 || input.velocity > 127 ||
        (input.kind == MidiTestInputKind::NoteOn && (hold_duration < std::chrono::milliseconds(1) ||
                                                     hold_duration > std::chrono::seconds(2))) ||
        (input.kind == MidiTestInputKind::NoteOff &&
         hold_duration != std::chrono::milliseconds(0))) {
        InspectorClientResult result;
        result.response = make_error(0, "Invalid bounded MIDI note input", "invalid_params");
        return result;
    }
    return run_inspector_operation(
        std::move(target), timeout, discovery, true,
        [input, hold_duration](InspectorClient& client, const auto& remaining) {
            auto request_timeout = remaining();
            if (request_timeout <= std::chrono::milliseconds(0))
                return make_error(0, "Inspector operation timed out", "request_timeout",
                                  R"({"mayHaveApplied":false})");
            auto response =
                client.request(std::string(methods::kTestInjectMidi),
                               choc::json::toString(midi_params(input), false), request_timeout);
            if (response.is_error || input.kind == MidiTestInputKind::NoteOff)
                return response;

            if (remaining() <= hold_duration)
                return make_error(0, "Inspector MIDI hold exceeded the operation timeout",
                                  "request_timeout", R"({"mayHaveApplied":true})");
            std::this_thread::sleep_for(hold_duration);

            MidiTestInput note_off = input;
            note_off.kind = MidiTestInputKind::NoteOff;
            note_off.velocity = 0;
            request_timeout = remaining();
            if (request_timeout <= std::chrono::milliseconds(0))
                return make_error(0, "Inspector MIDI hold exceeded the operation timeout",
                                  "request_timeout", R"({"mayHaveApplied":true})");
            return client.request(std::string(methods::kTestInjectMidi),
                                  choc::json::toString(midi_params(note_off), false),
                                  request_timeout);
        });
}

InspectorClientResult set_inspector_transport(const StandaloneTransportTestInput& input,
                                              InspectorClientTarget target,
                                              std::chrono::milliseconds timeout,
                                              const InspectorDiscoveryReader& discovery) {
    if ((!input.playing && !input.position_samples && !input.tempo_bpm) ||
        (input.position_samples && *input.position_samples < 0) ||
        (input.tempo_bpm && (!std::isfinite(*input.tempo_bpm) || *input.tempo_bpm < 20.0 ||
                             *input.tempo_bpm > 400.0))) {
        InspectorClientResult result;
        result.response = make_error(0, "Invalid standalone transport input", "invalid_params");
        return result;
    }
    auto params = choc::value::createObject("");
    if (input.playing)
        params.addMember("playing", choc::value::createBool(*input.playing));
    if (input.position_samples)
        params.addMember("position_samples", choc::value::createInt64(*input.position_samples));
    if (input.tempo_bpm)
        params.addMember("tempo_bpm", choc::value::createFloat64(*input.tempo_bpm));
    return request_inspector(std::string(methods::kTestSetTransport),
                             choc::json::toString(params, false), std::move(target), timeout,
                             discovery);
}

} // namespace pulp::inspect

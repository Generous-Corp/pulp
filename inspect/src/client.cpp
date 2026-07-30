#include <pulp/inspect/client.hpp>

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/authentication.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace pulp::inspect {
namespace {

InspectorMessage connection_error(std::int64_t request_id,
                                  std::string message,
                                  std::string code) {
    return make_error(request_id, std::move(message), std::move(code));
}

} // namespace

class InspectorClient::Impl {
public:
    events::InterprocessConnection connection;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::int64_t, InspectorMessage> responses;
    std::optional<InspectorAuthChallenge> challenge;
    EventHandler event_handler;
    std::atomic<std::int64_t> next_request_id{2};
    bool disconnected = true;

    void receive(std::string_view text) {
        InspectorMessage message;
        if (!decode_message(std::string(text), message))
            return;

        EventHandler event;
        {
            std::lock_guard lock(mutex);
            if (message.method == "Session.authChallenge") {
                try {
                    const auto value = choc::json::parse(message.params_json);
                    InspectorAuthChallenge parsed;
                    parsed.scheme = std::string(value["scheme"].getString());
                    parsed.nonce_hex =
                        std::string(value["nonce"].getString());
                    parsed.session_id =
                        std::string(value["sessionId"].getString());
                    parsed.protocol_version =
                        std::string(value["protocolVersion"].getString());
                    challenge = std::move(parsed);
                } catch (...) {
                }
                cv.notify_all();
                return;
            }
            if (message.id != 0 && message.method.empty()) {
                responses.insert_or_assign(message.id, std::move(message));
                cv.notify_all();
                return;
            }
            event = event_handler;
        }
        if (event)
            event(message);
    }

    InspectorMessage wait_for_response(std::int64_t id,
                                       std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, timeout, [&] {
                return responses.contains(id) || disconnected;
            })) {
            return connection_error(id,
                                    "Inspector request timed out",
                                    "request_timeout");
        }
        const auto found = responses.find(id);
        if (found == responses.end()) {
            return connection_error(id,
                                    "Inspector connection closed",
                                    "connection_closed");
        }
        auto response = std::move(found->second);
        responses.erase(found);
        return response;
    }
};

InspectorClient::InspectorClient() : impl_(std::make_unique<Impl>()) {
    impl_->connection.set_max_message_bytes(1024u * 1024u);
    impl_->connection.set_on_text_message(
        [this](std::string_view message) { impl_->receive(message); });
    impl_->connection.set_on_disconnected([this] {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->disconnected = true;
        }
        impl_->cv.notify_all();
    });
}

InspectorClient::~InspectorClient() {
    disconnect();
}

bool InspectorClient::connect(const InspectorDiscoveryRecord& record,
                              const InspectorDiscoveryReader& discovery,
                              std::chrono::milliseconds timeout) {
    disconnect();
    const auto token = discovery.read_credential(record);
    if (!token)
        return false;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->disconnected = false;
        impl_->challenge.reset();
        impl_->responses.clear();
    }
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
        challenge.protocol_version != record.protocol_version) {
        impl_->connection.disconnect();
        return false;
    }
    const auto proof = make_inspector_auth_proof(*token, challenge);
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
    if (!impl_->connection.send_message(encode_message(authentication))) {
        impl_->connection.disconnect();
        return false;
    }
    const auto response = impl_->wait_for_response(1, timeout);
    if (response.is_error) {
        impl_->connection.disconnect();
        return false;
    }
    return true;
}

void InspectorClient::disconnect() {
    impl_->connection.disconnect();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->disconnected = true;
        impl_->responses.clear();
        impl_->challenge.reset();
    }
    impl_->cv.notify_all();
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
    if (!is_connected()) {
        return connection_error(id,
                                "Inspector client is not connected",
                                "not_connected");
    }
    const auto message =
        make_request(id, std::move(method), std::move(params_json));
    if (!impl_->connection.send_message(encode_message(message))) {
        return connection_error(id,
                                "Inspector request could not be sent",
                                "send_failed");
    }
    return impl_->wait_for_response(id, timeout);
}

void InspectorClient::set_event_handler(EventHandler handler) {
    std::lock_guard lock(impl_->mutex);
    impl_->event_handler = std::move(handler);
}

} // namespace pulp::inspect

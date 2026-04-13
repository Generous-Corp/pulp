#include <pulp/format/remote_view_session.hpp>
#include <pulp/runtime/log.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <sstream>
#include <thread>

namespace pulp::format {

namespace {

// Serialize a single `{"id":N,"normalized":F}` payload.
std::string encode_param_payload(uint32_t id, float normalized) {
    std::ostringstream os;
    os << R"({"id":)" << id << R"(,"normalized":)" << normalized << "}";
    return os.str();
}

// Extract an integer field from a flat JSON object.
std::optional<uint32_t> parse_u32(std::string_view json, std::string_view key) {
    try {
        auto v = choc::json::parse(std::string{json});
        if (v.isObject() && v.hasObjectMember(std::string{key})) {
            auto m = v[std::string{key}];
            if (m.isInt32() || m.isInt64()) return static_cast<uint32_t>(m.getInt64());
            if (m.isFloat32() || m.isFloat64()) return static_cast<uint32_t>(m.getFloat64());
        }
    } catch (...) {}
    return std::nullopt;
}

std::optional<float> parse_f32(std::string_view json, std::string_view key) {
    try {
        auto v = choc::json::parse(std::string{json});
        if (v.isObject() && v.hasObjectMember(std::string{key})) {
            auto m = v[std::string{key}];
            if (m.isFloat32() || m.isFloat64()) return static_cast<float>(m.getFloat64());
            if (m.isInt32() || m.isInt64()) return static_cast<float>(m.getInt64());
        }
    } catch (...) {}
    return std::nullopt;
}

} // namespace

RemoteViewSession::RemoteViewSession(std::string url,
                                     state::StateStore& store,
                                     std::unique_ptr<runtime::MessageChannel> channel)
    : url_(std::move(url)),
      store_(store),
      channel_(std::move(channel)),
      peer_(std::make_unique<runtime::JsonRpcPeer>(*channel_)) {
}

RemoteViewSession::~RemoteViewSession() {
    close();
}

void RemoteViewSession::install_handlers_(Processor& processor) {
    // view.param_set: remote wants to change a parameter.
    peer_->on_notification("view.param_set",
        [this](std::string_view params_json) {
            auto id = parse_u32(params_json, "id");
            auto v  = parse_f32(params_json, "normalized");
            if (id && v) {
                store_.set_normalized(*id, *v);
            }
        });

    // view.param_get: remote asks for the current value.
    peer_->register_method("view.param_get",
        [this](std::string_view params_json) -> runtime::JsonRpcResult {
            auto id = parse_u32(params_json, "id");
            if (!id) return runtime::JsonRpcResult::fail(runtime::JsonRpcError::invalid_params());
            float v = store_.get_normalized(*id);
            std::ostringstream os;
            os << R"({"normalized":)" << v << "}";
            return runtime::JsonRpcResult::ok(os.str());
        });

    // view.input: forwarded input event. Current MVP just logs;
    // wiring into the bridge's primary view input dispatch is a
    // follow-up (requires synthetic input API on View).
    peer_->on_notification("view.input",
        [](std::string_view payload) {
            runtime::log_debug("RemoteViewSession: view.input {}", std::string{payload});
        });

    // view.close: remote is detaching.
    peer_->on_notification("view.close",
        [this](std::string_view) { close(); });

    (void)processor; // primary-view event hooks land with paint-op streaming.
}

bool RemoteViewSession::handshake_(Processor& processor) {
    install_handlers_(processor);

    // Send view.hello. We don't block on the reply — the protocol is
    // symmetric: both sides hello each other and no further state
    // gating is required.
    std::ostringstream hello;
    hello << R"({"protocol_version":1,"role":"remote"})";
    if (!peer_->notify("view.hello", hello.str())) {
        last_error_ = "send view.hello failed";
        return false;
    }

    // Send view.metadata describing the Processor's parameter list.
    std::ostringstream meta;
    meta << R"({"title":")" << processor.descriptor().name << R"(",)";
    auto hints = processor.view_size();
    meta << R"("size_hints":{"preferred_width":)" << hints.preferred_width
         << R"(,"preferred_height":)" << hints.preferred_height
         << R"(,"min_width":)" << hints.min_width
         << R"(,"min_height":)" << hints.min_height
         << R"(,"max_width":)" << hints.max_width
         << R"(,"max_height":)" << hints.max_height << "},";
    meta << R"("params":[)";
    const auto& params = store_.all_params();
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) meta << ",";
        meta << R"({"id":)" << params[i].id
             << R"(,"name":")" << params[i].name << R"("})";
    }
    meta << "]}";
    peer_->notify("view.metadata", meta.str());
    return true;
}

bool RemoteViewSession::set_parameter(uint32_t id, float normalized) {
    if (closed_ || !channel_->is_open()) return false;
    store_.set_normalized(id, normalized);
    return peer_->notify("view.param_changed", encode_param_payload(id, normalized));
}

std::optional<float> RemoteViewSession::get_parameter(uint32_t id) {
    if (closed_ || !channel_->is_open()) return std::nullopt;
    std::ostringstream req;
    req << R"({"id":)" << id << "}";
    std::optional<float> result;
    std::atomic<bool> done{false};
    peer_->send_request("view.param_get", req.str(),
        [&](const runtime::JsonRpcResult& resp) {
            if (!resp.error) {
                result = parse_f32(resp.result_json, "normalized");
            }
            done = true;
        });
    // Spin briefly — the test harness uses synchronous message loopback
    // so the reply is usually immediate. Real WebSocket use should
    // switch to an async accessor; kept synchronous for MVP.
    for (int i = 0; i < 100 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return result;
}

bool RemoteViewSession::send_input(std::string_view kind_and_payload) {
    if (closed_ || !channel_->is_open()) return false;
    return peer_->notify("view.input", kind_and_payload);
}

void RemoteViewSession::close() {
    if (closed_) return;
    closed_ = true;
    if (peer_ && channel_ && channel_->is_open()) {
        peer_->notify("view.close", R"({"reason":"local_detach"})");
    }
    peer_.reset();
    if (channel_) channel_->close();
}

bool RemoteViewSession::is_open() const {
    return !closed_ && channel_ && channel_->is_open();
}

} // namespace pulp::format

#include <pulp/runtime/json_rpc.hpp>

#include <choc/text/choc_JSON.h>
#include <choc/containers/choc_Value.h>

#include <sstream>
#include <utility>

namespace pulp::runtime {

namespace {

std::string value_to_json(const choc::value::ValueView& v) {
    return choc::json::toString(v);
}

// Build a JSON-RPC response envelope. `id_json` is already JSON (null,
// a number, or a quoted string). `result_json` is already JSON.
std::string make_response(std::string_view id_json, std::string_view result_json) {
    std::ostringstream ss;
    ss << R"({"jsonrpc":"2.0","id":)" << id_json
       << R"(,"result":)" << result_json << "}";
    return ss.str();
}

std::string make_error(std::string_view id_json, const JsonRpcError& err) {
    std::ostringstream ss;
    ss << R"({"jsonrpc":"2.0","id":)" << id_json
       << R"(,"error":{"code":)" << err.code
       << R"(,"message":)" << choc::json::getEscapedQuotedString(err.message);
    if (!err.data_json.empty()) {
        ss << R"(,"data":)" << err.data_json;
    }
    ss << "}}";
    return ss.str();
}

std::string make_request(std::uint64_t id, std::string_view method,
                         std::string_view params_json) {
    std::ostringstream ss;
    ss << R"({"jsonrpc":"2.0","id":)" << id
       << R"(,"method":)" << choc::json::getEscapedQuotedString(std::string(method));
    if (!params_json.empty()) {
        ss << R"(,"params":)" << params_json;
    }
    ss << "}";
    return ss.str();
}

std::string make_notification(std::string_view method, std::string_view params_json) {
    std::ostringstream ss;
    ss << R"({"jsonrpc":"2.0","method":)"
       << choc::json::getEscapedQuotedString(std::string(method));
    if (!params_json.empty()) {
        ss << R"(,"params":)" << params_json;
    }
    ss << "}";
    return ss.str();
}

}  // namespace

JsonRpcPeer::JsonRpcPeer(MessageChannel& channel) : channel_(&channel) {
    channel_->on_message([this](const Message& m) { handle_message(m); });
}

JsonRpcPeer::~JsonRpcPeer() {
    if (channel_) channel_->on_message({});
}

void JsonRpcPeer::register_method(std::string_view name, JsonRpcMethodHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!handler) methods_.erase(std::string(name));
    else methods_[std::string(name)] = std::move(handler);
}

void JsonRpcPeer::on_notification(std::string_view name,
                                  JsonRpcNotificationHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!handler) notifications_.erase(std::string(name));
    else notifications_[std::string(name)] = std::move(handler);
}

bool JsonRpcPeer::send_request(std::string_view method,
                               std::string_view params_json,
                               JsonRpcResponseCallback callback) {
    if (!channel_ || !channel_->is_open()) return false;
    const std::uint64_t id = next_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[id] = std::move(callback);
    }
    if (!channel_->send_text(make_request(id, method, params_json))) {
        // Write failed — reclaim the pending slot so the callback is not
        // leaked indefinitely. A retry from the caller will allocate a
        // fresh id rather than piling on stale entries.
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);
        return false;
    }
    return true;
}

bool JsonRpcPeer::notify(std::string_view method, std::string_view params_json) {
    if (!channel_ || !channel_->is_open()) return false;
    return channel_->send_text(make_notification(method, params_json));
}

void JsonRpcPeer::handle_message(const Message& msg) {
    std::string_view text(
        reinterpret_cast<const char*>(msg.payload.data()),
        msg.payload.size());
    // JSON-RPC supports batching (top-level array) but we accept only
    // single objects for Phase 4. A simple sniff: find first non-space.
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size()) return;
    if (text[i] == '[') {
        // Batching deferred; a well-behaved peer can still send a single
        // object. Ignore silently.
        return;
    }
    handle_object(text);
}

void JsonRpcPeer::handle_object(std::string_view obj_json) {
    choc::value::Value root;
    try {
        root = choc::json::parse(std::string(obj_json));
    } catch (...) {
        // RFC 4.2: malformed JSON → respond with a -32700 Parse error and
        // id: null so senders don't hang waiting for a reply.
        if (channel_) {
            channel_->send_text(make_error("null", JsonRpcError::parse_error()));
        }
        return;
    }
    if (!root.isObject()) {
        if (channel_) {
            channel_->send_text(make_error("null", JsonRpcError::invalid_request()));
        }
        return;
    }

    const bool has_method = root.hasObjectMember("method");
    const bool has_id = root.hasObjectMember("id");
    const bool has_result = root.hasObjectMember("result");
    const bool has_error = root.hasObjectMember("error");

    if (has_method && has_id) {
        dispatch_request(obj_json);
    } else if (has_method && !has_id) {
        // Notification
        auto method = root["method"].getWithDefault<std::string>("");
        JsonRpcNotificationHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = notifications_.find(method);
            if (it != notifications_.end()) handler = it->second;
        }
        if (handler) {
            std::string params_json;
            if (root.hasObjectMember("params")) {
                params_json = choc::json::toString(root["params"]);
            }
            handler(params_json);
        }
    } else if (has_result || has_error) {
        dispatch_response(obj_json);
    }
}

void JsonRpcPeer::dispatch_request(std::string_view request_json) {
    choc::value::Value root;
    try {
        root = choc::json::parse(std::string(request_json));
    } catch (...) {
        return;
    }

    const auto method = root["method"].getWithDefault<std::string>("");
    std::string params_json;
    if (root.hasObjectMember("params")) {
        params_json = choc::json::toString(root["params"]);
    }

    // id can be a number, a string, or null. Carry it through as raw JSON.
    std::string id_json = "null";
    if (root.hasObjectMember("id")) {
        id_json = choc::json::toString(root["id"]);
    }

    JsonRpcMethodHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = methods_.find(method);
        if (it != methods_.end()) handler = it->second;
    }

    if (!handler) {
        channel_->send_text(make_error(id_json, JsonRpcError::method_not_found()));
        return;
    }

    JsonRpcResult result;
    try {
        result = handler(params_json);
    } catch (...) {
        result = JsonRpcResult::fail(JsonRpcError::internal_error());
    }

    if (result.error) {
        channel_->send_text(make_error(id_json, *result.error));
    } else {
        const auto payload = result.result_json.empty() ? std::string("null")
                                                        : result.result_json;
        channel_->send_text(make_response(id_json, payload));
    }
}

void JsonRpcPeer::dispatch_response(std::string_view response_json) {
    choc::value::Value root;
    try {
        root = choc::json::parse(std::string(response_json));
    } catch (...) {
        return;
    }
    if (!root.hasObjectMember("id")) return;
    auto id_view = root["id"];
    if (!id_view.isInt()) return;
    const std::uint64_t id = static_cast<std::uint64_t>(id_view.getInt64());

    JsonRpcResponseCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        cb = std::move(it->second);
        pending_.erase(it);
    }

    JsonRpcResult result;
    if (root.hasObjectMember("error")) {
        auto err = root["error"];
        JsonRpcError e;
        if (err.hasObjectMember("code")) e.code = static_cast<int>(err["code"].getInt64());
        if (err.hasObjectMember("message")) e.message = err["message"].getWithDefault<std::string>("");
        if (err.hasObjectMember("data")) e.data_json = choc::json::toString(err["data"]);
        result = JsonRpcResult::fail(std::move(e));
    } else if (root.hasObjectMember("result")) {
        result = JsonRpcResult::ok(choc::json::toString(root["result"]));
    }
    if (cb) cb(result);
}

}  // namespace pulp::runtime

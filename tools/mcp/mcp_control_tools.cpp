// mcp_control_tools.cpp -- typed MCP projection over the shared control client.

#include "mcp_control_tools.hpp"

#include "mcp_json.hpp"

#include <pulp/inspect/control_client_connection.hpp>
#include <pulp/inspect/control_inspector_client.hpp>

#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/control_carrier.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace pulp_mcp {
namespace {

using namespace pulp::inspect;
using Value = choc::value::Value;

constexpr std::string_view kInstancesTool = "pulp_control_instances";
constexpr std::string_view kStatusTool = "pulp_control_status";
constexpr std::string_view kGrantTool = "pulp_control_grant_request";
constexpr std::string_view kRevokeTool = "pulp_control_revoke";
constexpr std::string_view kCancelTool = "pulp_control_cancel";

std::string operation_tool_name(std::string_view id) {
    constexpr std::string_view prefix = "dev.pulp.";
    constexpr std::string_view suffix = "@1";
    if (!id.starts_with(prefix) || !id.ends_with(suffix))
        return {};
    id.remove_prefix(prefix.size());
    id.remove_suffix(suffix.size());
    std::string name = "pulp_control_";
    for (const auto character : id) {
        if (std::isalnum(static_cast<unsigned char>(character)))
            name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        else if (name.back() != '_')
            name.push_back('_');
    }
    return name;
}

const ControlOperationDescriptor* operation_for_tool(std::string_view name) {
    for (const auto& operation : control_operation_registry())
        if ((capability_is_grantable(operation.capability) ||
             operation.capability == InspectorCapability::ArtifactRead) &&
            operation_tool_name(operation.id) == name)
            return &operation;
    return nullptr;
}

std::string quote(std::string_view value) {
    return json_string(std::string(value));
}

std::string success_payload(std::string_view result_json) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" +
           quote(result_json) + "}],\"structuredContent\":" + std::string(result_json) + "}";
}

std::string error_payload(std::string_view code, std::string_view explanation,
                          std::string_view data_json = "{}") {
    std::string data(data_json);
    try {
        if (!choc::json::parse(data).isObject())
            data = "{}";
    } catch (...) {
        data = "{}";
    }
    auto structured = "{\"ok\":false,\"error\":{\"code\":" + quote(code) +
                      ",\"message\":" + quote(explanation) + ",\"data\":" + data + "}}";
    return "{\"content\":[{\"type\":\"text\",\"text\":" + quote(explanation) +
           "}],\"isError\":true,\"structuredContent\":" + structured + "}";
}

std::optional<Value> parse_object(std::string_view json) {
    try {
        auto value = choc::json::parse(json);
        if (value.isObject())
            return value;
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<std::string> required_string(const Value& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.hasObjectMember(key) || !object[key].isString() ||
        object[key].getString().empty())
        return std::nullopt;
    return std::string(object[key].getString());
}

std::string progress_token_json(std::string_view token) {
    try {
        const auto value = choc::json::parse(token);
        if (value.isString() || value.isInt32() || value.isInt64())
            return std::string(token);
    } catch (...) {
    }
    return quote(token);
}

std::string random_token(std::string_view prefix) {
    const auto bytes = pulp::runtime::secure_random_bytes(16);
    return bytes ? std::string(prefix) + pulp::runtime::hex_encode(*bytes) : std::string{};
}

class OperationDeadline {
  public:
    OperationDeadline(std::chrono::milliseconds budget, ControlMcpSteadyNow steady_now)
        : steady_now_(steady_now ? std::move(steady_now)
                                 : ControlMcpSteadyNow([] { return std::chrono::steady_clock::now(); })),
          deadline_(steady_now_() + budget) {}

    std::chrono::milliseconds remaining() const {
        const auto now = steady_now_();
        if (now >= deadline_)
            return std::chrono::milliseconds::zero();
        return std::max(std::chrono::ceil<std::chrono::milliseconds>(deadline_ - now),
                        std::chrono::milliseconds{1});
    }

    std::int64_t unix_deadline_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count() +
               remaining().count();
    }

  private:
    ControlMcpSteadyNow steady_now_;
    std::chrono::steady_clock::time_point deadline_;
};

bool is_terminal_session_status(std::string_view status) {
    return status == "session-required" || status == "session-superseded" ||
           status == "client-unavailable" || status == "unavailable" ||
           status == "not-connected" || status == "connection-lost" ||
           status == "send-failed" || status == "timeout";
}

std::string percent_encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            out.push_back(character);
        } else {
            out.push_back('%');
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0f]);
        }
    }
    return out;
}

std::optional<std::string> percent_decode(std::string_view value) {
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            out.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size())
            return std::nullopt;
        const auto high = digit(value[index + 1]);
        const auto low = digit(value[index + 2]);
        if (high < 0 || low < 0)
            return std::nullopt;
        out.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return out;
}

class InstalledControlMcpSession final : public ControlMcpSession {
  public:
    InstalledControlMcpSession(std::unique_ptr<ControlClientConnection> connection,
                               std::string client_id)
        : connection_(std::move(connection)), client_id_(std::move(client_id)) {}

    ControlClientTransport& transport() override { return *connection_; }
    ControlManagementResult manage(std::string_view command,
                                   std::string_view params_json,
                                   std::chrono::milliseconds timeout) override {
        return connection_->manage(command, params_json, timeout);
    }
    std::string_view client_id() const override { return client_id_; }
    void set_progress_sink(ProgressSink sink) override {
        connection_->set_progress_sink(std::move(sink));
    }

  private:
    std::unique_ptr<ControlClientConnection> connection_;
    std::string client_id_;
};

std::mutex g_adapter_mutex;
std::mutex g_adapter_call_mutex;
std::string g_executable_path;
ControlMcpSessionFactory g_test_factory;
ControlMcpNotificationSink g_notification_sink;
std::shared_ptr<ControlMcpAdapter> g_adapter;

ControlMcpOpenResult open_installed_session(std::chrono::milliseconds timeout) {
    OperationDeadline deadline(timeout, {});
    if (g_executable_path.empty())
        return {.error_code = "broker-unavailable",
                .explanation = "pulp-mcp executable identity is unavailable"};
    const auto broker = installed_control_broker_executable(g_executable_path);
    if (broker.empty())
        return {.error_code = "broker-unavailable",
                .explanation = "the installed pulp-control-broker was not found"};

    const auto connect_timeout = deadline.remaining();
    if (connect_timeout <= std::chrono::milliseconds::zero())
        return {.error_code = "timeout", .explanation = "the control operation timed out"};
    auto connection = std::make_unique<ControlClientConnection>(ControlClientConnectionConfig{
        .endpoint_path = default_control_endpoint_path(),
        .expected_broker_executable = broker,
        .connect_timeout = connect_timeout,
        .write_timeout = connect_timeout,
        .frame_read_timeout = connect_timeout,
    });
    if (!connection->connect()) {
        if (deadline.remaining() <= std::chrono::milliseconds::zero())
            return {.error_code = "timeout", .explanation = "the control operation timed out"};
        return {.error_code = connection->last_error_code(),
                .explanation = connection->last_error_explanation()};
    }
    const auto enrollment_timeout = deadline.remaining();
    if (enrollment_timeout <= std::chrono::milliseconds::zero())
        return {.error_code = "timeout", .explanation = "the control operation timed out"};
    const auto enrolled = connection->manage("enroll", "{}", enrollment_timeout);
    if (enrolled.status_id != "accepted")
        return {.error_code = enrolled.status_id, .explanation = enrolled.explanation};
    const auto data = parse_object(enrolled.data_json);
    const auto client_id = data ? required_string(*data, "client_id") : std::nullopt;
    if (!client_id)
        return {.error_code = "malformed-response",
                .explanation = "broker enrollment omitted client_id"};
    return {.session =
                std::make_unique<InstalledControlMcpSession>(std::move(connection), *client_id)};
}

std::shared_ptr<ControlMcpAdapter> global_adapter() {
    std::lock_guard lock(g_adapter_mutex);
    if (!g_adapter) {
        auto factory = g_test_factory ? g_test_factory : ControlMcpSessionFactory(open_installed_session);
        g_adapter = std::make_shared<ControlMcpAdapter>(std::move(factory), g_notification_sink);
    }
    return g_adapter;
}

std::string management_tool_json() {
    return R"JSON({"name":"pulp_control_instances","description":"List broker-owned live capability-control instances.","inputSchema":{"type":"object","additionalProperties":false,"properties":{}},"annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},{"name":"pulp_control_status","description":"Read one exact broker-owned instance by instance_id.","inputSchema":{"type":"object","additionalProperties":false,"required":["instance_id"],"properties":{"instance_id":{"type":"string","minLength":1}}},"annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},{"name":"pulp_control_grant_request","description":"Ask the broker for a grant. Any consent challenge is broker-owned; MCP metadata or client UI approval is never authority.","inputSchema":{"type":"object","additionalProperties":false,"required":["instance_id","profile"],"properties":{"instance_id":{"type":"string","minLength":1},"profile":{"type":"string","enum":["inspect-readonly","observe","develop"]}}},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},{"name":"pulp_control_revoke","description":"Revoke a broker-issued grant.","inputSchema":{"type":"object","additionalProperties":false,"required":["grant_id"],"properties":{"grant_id":{"type":"string","minLength":1}}},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":true,"openWorldHint":false}},{"name":"pulp_control_cancel","description":"Request cooperative cancellation for an operation on one exact instance.","inputSchema":{"type":"object","additionalProperties":false,"required":["instance_id","request_id"],"properties":{"instance_id":{"type":"string","minLength":1},"request_id":{"type":"string","minLength":1},"reason":{"type":"string","maxLength":1024}}},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}})JSON";
}

std::string operation_output_schema(const ControlOperationDescriptor& operation) {
    if (operation.capability == InspectorCapability::ArtifactRead)
        return std::string(operation.output_schema_json);
    return "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"ok\",\"schema\",\"request_id\",\"receipt_id\",\"operation_id\",\"operation_version\",\"state\",\"explanation\",\"result\",\"artifacts\",\"progress\"],\"properties\":{\"ok\":{\"const\":true},\"schema\":{\"const\":\"dev.pulp.control/mcp-receipt@1\"},\"request_id\":{\"type\":\"string\"},\"receipt_id\":{\"type\":\"string\"},\"operation_id\":{\"const\":" +
           quote(operation.id) +
           "},\"operation_version\":{\"const\":" + std::to_string(operation.version) +
           "},\"state\":{\"const\":\"completed\"},\"explanation\":{\"type\":\"string\"},\"result\":" +
           std::string(operation.output_schema_json) +
           ",\"artifacts\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"artifact_id\",\"media_type\",\"byte_size\"],\"properties\":{\"artifact_id\":{\"type\":\"string\"},\"media_type\":{\"type\":\"string\"},\"byte_size\":{\"type\":\"integer\",\"minimum\":0}}}},\"progress\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"sequence\",\"current\",\"total\",\"detail\"],\"properties\":{\"sequence\":{\"type\":\"integer\",\"minimum\":1},\"current\":{\"type\":\"integer\",\"minimum\":0},\"total\":{\"type\":\"integer\",\"minimum\":1},\"detail\":{\"type\":\"object\"}}}}}}";
}

} // namespace

class ControlMcpAdapter::Impl {
  public:
    Impl(ControlMcpSessionFactory factory_in, ControlMcpNotificationSink notifications_in,
         ControlMcpSteadyNow steady_now_in)
        : factory(std::move(factory_in)), notifications(std::move(notifications_in)),
          steady_now(steady_now_in ? std::move(steady_now_in)
                                   : ControlMcpSteadyNow([] { return std::chrono::steady_clock::now(); })) {}

    std::optional<std::string> ensure_session(std::chrono::milliseconds timeout =
                                                 std::chrono::seconds(3)) {
        const auto now = steady_now();
        const auto refresh_requested = session_refresh_requested.exchange(false);
        if (session && !refresh_requested &&
            now - session_opened_at < std::chrono::minutes(4))
            return std::nullopt;
        session.reset();
        auto opened = factory ? factory(timeout) : ControlMcpOpenResult{};
        if (!opened.session) {
            last_error_code = opened.error_code.empty() ? "control-session-unavailable"
                                                        : std::move(opened.error_code);
            last_explanation = opened.explanation.empty() ? "capability-control session is unavailable"
                                                          : std::move(opened.explanation);
            return error_payload(last_error_code, last_explanation);
        }
        session = std::move(opened.session);
        session_opened_at = now;
        session->set_progress_sink([this](const ControlProgressEnvelope& progress) {
            progress_events.push_back(progress);
            if (notifications && !active_progress_token.empty()) {
                auto params = "{\"progressToken\":" + progress_token_json(active_progress_token) +
                              ",\"progress\":" + std::to_string(progress.current) +
                              ",\"total\":" + std::to_string(progress.total) +
                              ",\"message\":" + quote(progress.detail_json) + "}";
                notifications("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":" +
                              params + "}");
            }
        });
        return std::nullopt;
    }

    std::optional<Value> inventory(std::string& error,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(3),
                                   const OperationDeadline* deadline = nullptr) {
        if (auto unavailable = ensure_session(timeout)) {
            error = *unavailable;
            return std::nullopt;
        }
        const auto inventory_timeout = deadline ? deadline->remaining() : timeout;
        if (inventory_timeout <= std::chrono::milliseconds::zero()) {
            error = error_payload("timeout", "the control operation timed out");
            return std::nullopt;
        }
        const auto managed = session->manage("instances", "{}", inventory_timeout);
        if (managed.status_id != "completed") {
            if (is_terminal_session_status(managed.status_id))
                mark_session_unhealthy();
            error = error_payload(managed.status_id, managed.explanation);
            return std::nullopt;
        }
        auto data = parse_object(managed.data_json);
        if (!data || !data->hasObjectMember("instances") || !(*data)["instances"].isArray()) {
            error = error_payload("malformed-response", "broker inventory omitted instances");
            return std::nullopt;
        }
        return data;
    }

    std::optional<Value> exact_instance(std::string_view instance_id, std::string& error,
                                        std::chrono::milliseconds timeout =
                                            std::chrono::seconds(3),
                                        const OperationDeadline* deadline = nullptr) {
        auto data = inventory(error, timeout, deadline);
        if (!data)
            return std::nullopt;
        std::optional<Value> match;
        const auto values = (*data)["instances"];
        for (std::uint32_t index = 0; index < values.size(); ++index) {
            if (!values[index].isObject() || !values[index].hasObjectMember("instance_id") ||
                !values[index]["instance_id"].isString())
                continue;
            if (values[index]["instance_id"].getString() == instance_id) {
                if (match) {
                    error = error_payload("ambiguous-instance", "instance_id is ambiguous");
                    return std::nullopt;
                }
                match = Value(values[index]);
            }
        }
        if (!match)
            error = error_payload("instance-not-found", "the exact instance is not live");
        return match;
    }

    std::string grant(std::string_view instance_id, std::string_view instance_generation,
                      std::string_view authority, bool operation_specific,
                      std::string& grant_id, std::chrono::milliseconds timeout) {
        const auto cache_key = std::string(instance_id) + "\n" +
                               std::string(instance_generation) + "\n" + std::string(authority);
        if (const auto found = automatic_grants.find(cache_key); found != automatic_grants.end()) {
            if (std::chrono::steady_clock::now() < found->second.second) {
                grant_id = found->second.first;
                return {};
            }
            automatic_grants.erase(found);
        }
        auto params = choc::value::createObject("");
        params.addMember("instance_id", choc::value::createString(instance_id));
        params.addMember(operation_specific ? "operation_id" : "profile",
                         choc::value::createString(authority));
        const auto managed = session->manage("grant-request", choc::json::toString(params, false),
                                             timeout);
        if (managed.status_id != "granted") {
            if (is_terminal_session_status(managed.status_id))
                mark_session_unhealthy();
            auto data = "{\"broker_owned\":true,\"single_use\":true,\"status\":" +
                        quote(managed.status_id) + "}";
            return error_payload(managed.status_id, managed.explanation.empty()
                                                        ? "broker-owned consent is required"
                                                        : managed.explanation,
                                 data);
        }
        const auto result = parse_object(managed.data_json);
        const auto id = result ? required_string(*result, "grant_id") : std::nullopt;
        if (!id)
            return error_payload("malformed-response", "broker grant response omitted grant_id");
        grant_id = *id;
        // Broker grants default to fifteen minutes. Refresh conservatively so
        // the adapter never presents a locally cached grant past broker expiry.
        automatic_grants[cache_key] = {
            grant_id, std::chrono::steady_clock::now() + std::chrono::minutes(14)};
        return {};
    }

    void mark_session_unhealthy() {
        session.reset();
    }

    void request_session_refresh() { session_refresh_requested.store(true); }

    void register_active_request(std::string request_id, std::string instance_id,
                                 std::string publication_id) {
        std::lock_guard lock(active_requests_mutex);
        active_requests.emplace(std::move(request_id),
                                std::pair{std::move(instance_id), std::move(publication_id)});
    }

    std::optional<std::string> active_request_publication(std::string_view request_id,
                                                          std::string_view instance_id) const {
        std::lock_guard lock(active_requests_mutex);
        const auto found = active_requests.find(std::string(request_id));
        if (found == active_requests.end() || found->second.first != instance_id)
            return std::nullopt;
        return found->second.second;
    }

    void finish_active_request(std::string_view request_id) {
        std::lock_guard lock(active_requests_mutex);
        active_requests.erase(std::string(request_id));
    }

    void forget_grant(std::string_view grant_id) {
        std::erase_if(automatic_grants,
                      [&](const auto& entry) { return entry.second.first == grant_id; });
    }

    std::string receipt_payload(const ControlReceiptEnvelope& receipt,
                                const ControlOperationDescriptor* operation = nullptr,
                                bool include_progress = true) {
        auto root = choc::value::createObject("");
        root.addMember("ok", choc::value::createBool(receipt.state == ControlReceiptState::Completed));
        root.addMember("schema", choc::value::createString("dev.pulp.control/mcp-receipt@1"));
        root.addMember("request_id", choc::value::createString(receipt.request_id));
        root.addMember("receipt_id", choc::value::createString(receipt.receipt_id));
        root.addMember("operation_id", choc::value::createString(receipt.operation_id));
        root.addMember("operation_version", choc::value::createInt32(static_cast<std::int32_t>(receipt.operation_version)));
        root.addMember("state", choc::value::createString(control_receipt_state_id(receipt.state)));
        root.addMember("explanation", choc::value::createString(receipt.explanation));
        try {
            root.addMember("result", choc::json::parse(receipt.detail_json));
        } catch (...) {
            return error_payload("invalid-control-response", "receipt detail is invalid JSON");
        }
        auto artifacts = choc::value::createEmptyArray();
        for (const auto& artifact : receipt.artifacts) {
            auto item = choc::value::createObject("");
            item.addMember("artifact_id", choc::value::createString(artifact.artifact_id));
            item.addMember("media_type", choc::value::createString(artifact.media_type));
            item.addMember("byte_size", choc::value::createInt64(static_cast<std::int64_t>(artifact.byte_size)));
            artifacts.addArrayElement(item);
        }
        root.addMember("artifacts", artifacts);
        auto progress = choc::value::createEmptyArray();
        if (include_progress) {
            for (const auto& event : progress_events) {
                auto item = choc::value::createObject("");
                item.addMember("sequence", choc::value::createInt64(static_cast<std::int64_t>(event.sequence)));
                item.addMember("current", choc::value::createInt64(static_cast<std::int64_t>(event.current)));
                item.addMember("total", choc::value::createInt64(static_cast<std::int64_t>(event.total)));
                item.addMember("detail", choc::json::parse(event.detail_json));
                progress.addArrayElement(item);
            }
        }
        root.addMember("progress", progress);

        if (receipt.state == ControlReceiptState::Completed && operation) {
            ControlJsonSchemaDiagnostics diagnostics;
            if (!validate_control_output_json_schema(receipt.detail_json,
                                                     operation->output_schema_json, &diagnostics))
                return error_payload("invalid-control-response", diagnostics.explanation);
        }
        const auto json = choc::json::toString(root, false);
        if (receipt.state == ControlReceiptState::Completed)
            return success_payload(json);
        const auto code = receipt.result_code
                              ? std::string(control_result_code_id(*receipt.result_code))
                              : std::string(control_receipt_state_id(receipt.state));
        return error_payload(code, receipt.explanation.empty() ? "control operation did not complete"
                                                               : receipt.explanation,
                             json);
    }

    ControlMcpSessionFactory factory;
    ControlMcpNotificationSink notifications;
    ControlMcpSteadyNow steady_now;
    std::unique_ptr<ControlMcpSession> session;
    std::chrono::steady_clock::time_point session_opened_at{};
    std::atomic_bool session_refresh_requested{false};
    std::string last_error_code;
    std::string last_explanation;
    std::string active_progress_token;
    std::vector<ControlProgressEnvelope> progress_events;
    std::map<std::string,
             std::pair<std::string, std::chrono::steady_clock::time_point>> automatic_grants;
    mutable std::mutex active_requests_mutex;
    std::map<std::string, std::pair<std::string, std::string>> active_requests;
};

ControlMcpAdapter::ControlMcpAdapter(ControlMcpSessionFactory factory,
                                     ControlMcpNotificationSink notifications,
                                     ControlMcpSteadyNow steady_now)
    : impl_(std::make_unique<Impl>(std::move(factory), std::move(notifications),
                                   std::move(steady_now))) {}
ControlMcpAdapter::~ControlMcpAdapter() = default;

std::string ControlMcpAdapter::tools_json_fragment() const {
    std::string out = management_tool_json();
    for (const auto& operation : control_operation_registry()) {
        if (!capability_is_grantable(operation.capability) &&
            operation.capability != InspectorCapability::ArtifactRead)
            continue;
        const auto name = operation_tool_name(operation.id);
        const auto risk = capability_risk(operation.capability);
        const bool read_only = risk == InspectorCapabilityRisk::Observe ||
                               risk == InspectorCapabilityRisk::Sensitive;
        out += ",{\"name\":" + quote(name) + ",\"description\":" +
               quote("Typed capability-control operation " + std::string(operation.id) +
                     ". instance_id selects exactly one broker-owned live instance. Grants and consent are broker authority; MCP annotations are advisory only.") +
               ",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"instance_id\",\"input\"],\"properties\":{\"instance_id\":{\"type\":\"string\",\"minLength\":1},\"request_id\":{\"type\":\"string\",\"minLength\":1},\"grant_id\":{\"type\":\"string\",\"minLength\":1},\"profile\":{\"type\":\"string\",\"enum\":[\"inspect-readonly\",\"observe\",\"develop\"]},\"expected_state_generation\":{\"type\":\"integer\",\"minimum\":0},\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":300000},\"input\":" +
               std::string(operation.input_schema_json) + "}},\"outputSchema\":" +
               operation_output_schema(operation) +
               ",\"annotations\":{\"readOnlyHint\":" + (read_only ? "true" : "false") +
               ",\"destructiveHint\":" +
               (risk == InspectorCapabilityRisk::Critical || risk == InspectorCapabilityRisk::HighRisk
                    ? "true"
                    : "false") +
               ",\"idempotentHint\":" + (read_only ? "true" : "false") +
               ",\"openWorldHint\":false}}";
    }
    return out;
}

bool ControlMcpAdapter::owns_tool(std::string_view name) const {
    return name == kInstancesTool || name == kStatusTool || name == kGrantTool ||
           name == kRevokeTool || name == kCancelTool || operation_for_tool(name) != nullptr;
}

std::string ControlMcpAdapter::call_tool(std::string_view name, std::string_view arguments_json,
                                         std::string_view progress_token) {
    const auto arguments = parse_object(arguments_json);
    if (!arguments)
        return error_payload("invalid-arguments", "control tool arguments must be an object");

    if (name == kCancelTool) {
        const auto instance_id = required_string(*arguments, "instance_id");
        const auto request_id = required_string(*arguments, "request_id");
        if (!instance_id || !request_id)
            return error_payload("invalid-arguments", "instance_id and request_id are required");
        if (arguments->hasObjectMember("reason") && !(*arguments)["reason"].isString())
            return error_payload("invalid-arguments", "reason must be a string");
        const auto active_publication =
            impl_->active_request_publication(*request_id, *instance_id);
        if (!active_publication)
            return error_payload("request-instance-mismatch",
                                 "request_id is not active on the exact instance");
        auto opened = impl_->factory ? impl_->factory(std::chrono::seconds(3))
                                     : ControlMcpOpenResult{};
        if (!opened.session)
            return error_payload(opened.error_code.empty() ? "control-session-unavailable"
                                                            : opened.error_code,
                                 opened.explanation);
        // The installed factory resumes the durable broker identity on this
        // connection, superseding the primary transport even on later errors.
        impl_->request_session_refresh();
        const auto inventory =
            opened.session->manage("instances", "{}", std::chrono::seconds(3));
        const auto inventory_data = parse_object(inventory.data_json);
        std::optional<std::string> exact_publication;
        if (inventory.status_id == "completed" && inventory_data &&
            inventory_data->hasObjectMember("instances") &&
            (*inventory_data)["instances"].isArray()) {
            const auto instances = (*inventory_data)["instances"];
            for (std::uint32_t index = 0; index < instances.size(); ++index) {
                if (instances[index]["instance_id"].isString() &&
                    instances[index]["instance_id"].getString() == *instance_id &&
                    instances[index]["publication_id"].isString()) {
                    if (exact_publication)
                        return error_payload("ambiguous-instance", "instance_id is ambiguous");
                    exact_publication = std::string(instances[index]["publication_id"].getString());
                }
            }
        }
        if (!exact_publication)
            return error_payload("instance-not-found", "the exact instance is not live");
        if (*exact_publication != *active_publication)
            return error_payload("request-instance-mismatch",
                                 "request_id is not active on the exact instance");
        ControlClient cancellation_client(opened.session->transport());
        const auto negotiation = cancellation_client.negotiate({
            .versions = {kControlProtocolVersion, kControlProtocolVersion},
            .mandatory_features = {"cancellation", "receipts"},
        });
        if (!negotiation.response ||
            negotiation.response->status != ControlNegotiationStatus::Accepted)
            return error_payload(negotiation.error_code.empty() ? "negotiation-failed"
                                                                : negotiation.error_code,
                                 negotiation.explanation);
        const auto reason = required_string(*arguments, "reason").value_or("mcp-client-cancelled");
        const auto result = cancellation_client.cancel({.request_id = *request_id, .reason = reason});
        if (!result.response)
            return error_payload(result.error_code, result.explanation);
        auto acknowledgement = choc::value::createObject("");
        acknowledgement.addMember("ok", choc::value::createBool(true));
        acknowledgement.addMember(
            "schema", choc::value::createString("dev.pulp.control/mcp-cancellation@1"));
        acknowledgement.addMember("request_id",
                                  choc::value::createString(result.response->request_id));
        acknowledgement.addMember("receipt_id",
                                  choc::value::createString(result.response->receipt_id));
        acknowledgement.addMember(
            "state", choc::value::createString(control_receipt_state_id(result.response->state)));
        acknowledgement.addMember("cancellation_accepted", choc::value::createBool(true));
        return success_payload(choc::json::toString(acknowledgement, false));
    }
    const auto* operation = operation_for_tool(name);
    std::optional<OperationDeadline> operation_deadline;
    if (operation) {
        static constexpr std::array<std::string_view, 7> allowed_fields{
            "instance_id", "request_id", "grant_id", "profile",
            "expected_state_generation", "timeout_ms", "input"};
        for (std::uint32_t index = 0; index < arguments->size(); ++index) {
            const auto member = arguments->getObjectMemberAt(index);
            if (std::ranges::find(allowed_fields, member.name) == allowed_fields.end())
                return error_payload("invalid-arguments",
                                     "unexpected control argument: " + std::string(member.name));
        }
        for (const auto field : {"request_id", "grant_id", "profile"}) {
            if (arguments->hasObjectMember(field) &&
                (!(*arguments)[field].isString() || (*arguments)[field].getString().empty()))
                return error_payload("invalid-arguments", std::string(field) +
                                                              " must be a non-empty string");
        }
        for (const auto field : {"timeout_ms", "expected_state_generation"}) {
            if (arguments->hasObjectMember(field) &&
                !((*arguments)[field].isInt32() || (*arguments)[field].isInt64()))
                return error_payload("invalid-arguments", std::string(field) + " must be an integer");
        }
        if (!arguments->hasObjectMember("input") || !(*arguments)["input"].isObject())
            return error_payload("invalid-arguments", "input is required and must be an object");
        const auto input_json = choc::json::toString((*arguments)["input"], false);
        ControlJsonSchemaDiagnostics diagnostics;
        if (!validate_control_json_schema(input_json, operation->input_schema_json, &diagnostics))
            return error_payload("invalid-arguments", diagnostics.explanation);
        const auto timeout_value = arguments->hasObjectMember("timeout_ms")
                                       ? (*arguments)["timeout_ms"].getInt64()
                                       : 3000;
        if (timeout_value < 1 || timeout_value > 300000)
            return error_payload("invalid-arguments", "timeout_ms must be between 1 and 300000");
        operation_deadline.emplace(std::chrono::milliseconds(timeout_value), impl_->steady_now);
    }
    const auto remaining = [&]() {
        return operation_deadline ? operation_deadline->remaining() : std::chrono::seconds(3);
    };
    if (operation_deadline && remaining() <= std::chrono::milliseconds::zero())
        return error_payload("timeout", "the control operation timed out");
    if (!operation)
        if (auto unavailable = impl_->ensure_session(remaining()))
            return *unavailable;

    if (name == kInstancesTool) {
        std::string error;
        auto data = impl_->inventory(error);
        if (!data)
            return error;
        auto root = choc::value::createObject("");
        root.addMember("ok", choc::value::createBool(true));
        root.addMember("schema", choc::value::createString("dev.pulp.control/mcp-instances@1"));
        root.addMember("instances", (*data)["instances"]);
        return success_payload(choc::json::toString(root, false));
    }
    if (name == kStatusTool) {
        const auto instance_id = required_string(*arguments, "instance_id");
        if (!instance_id)
            return error_payload("invalid-arguments", "instance_id is required");
        std::string error;
        auto item = impl_->exact_instance(*instance_id, error);
        if (!item)
            return error;
        auto root = choc::value::createObject("");
        root.addMember("ok", choc::value::createBool(true));
        root.addMember("schema", choc::value::createString("dev.pulp.control/mcp-status@1"));
        root.addMember("instance", *item);
        return success_payload(choc::json::toString(root, false));
    }
    if (name == kGrantTool || name == kRevokeTool) {
        auto command = name == kGrantTool ? "grant-request" : "revoke";
        Value params = choc::value::createObject("");
        if (name == kGrantTool) {
            const auto instance_id = required_string(*arguments, "instance_id");
            const auto profile = required_string(*arguments, "profile");
            if (!instance_id || !profile)
                return error_payload("invalid-arguments", "instance_id and profile are required");
            params.addMember("instance_id", choc::value::createString(*instance_id));
            params.addMember("profile", choc::value::createString(*profile));
        } else {
            const auto grant_id = required_string(*arguments, "grant_id");
            if (!grant_id)
                return error_payload("invalid-arguments", "grant_id is required");
            params.addMember("grant_id", choc::value::createString(*grant_id));
        }
        const auto managed = impl_->session->manage(
            command, choc::json::toString(params, false), std::chrono::seconds(3));
        auto root = choc::value::createObject("");
        root.addMember("ok", choc::value::createBool(managed.status_id == "granted" ||
                                               managed.status_id == "revoked"));
        root.addMember("status", choc::value::createString(managed.status_id));
        root.addMember("explanation", choc::value::createString(managed.explanation));
        try { root.addMember("data", choc::json::parse(managed.data_json)); }
        catch (...) { return error_payload("malformed-response", "broker management response is invalid"); }
        const auto json = choc::json::toString(root, false);
        if (name == kRevokeTool && managed.status_id == "revoked")
            impl_->forget_grant(std::string(params["grant_id"].getString()));
        if (is_terminal_session_status(managed.status_id))
            impl_->mark_session_unhealthy();
        return (managed.status_id == "granted" || managed.status_id == "revoked")
                   ? success_payload(json)
                   : error_payload(managed.status_id, managed.explanation, json);
    }

    const auto instance_id = required_string(*arguments, "instance_id");
    if (!instance_id)
        return error_payload("invalid-arguments", "instance_id is required");
    std::string error;
    if (operation_deadline && remaining() <= std::chrono::milliseconds::zero())
        return error_payload("timeout", "the control operation timed out");
    auto item = impl_->exact_instance(*instance_id, error, remaining(),
                                      operation_deadline ? &*operation_deadline : nullptr);
    if (!item)
        return error;

    ControlClient client(impl_->session->transport());
    if (!operation)
        return error_payload("unknown-tool", "unknown control tool");
    const auto input_json = choc::json::toString((*arguments)["input"], false);

    if (operation->capability == InspectorCapability::ArtifactRead) {
        if (remaining() <= std::chrono::milliseconds::zero())
            return error_payload("timeout", "the control operation timed out");
        const auto negotiation = client.negotiate({
            .versions = {kControlProtocolVersion, kControlProtocolVersion},
            .mandatory_features = {"artifacts", "receipts"},
        }, remaining());
        if (!negotiation.response ||
            negotiation.response->status != ControlNegotiationStatus::Accepted) {
            if (!negotiation.response)
                impl_->mark_session_unhealthy();
            return error_payload(negotiation.error_code.empty() ? "negotiation-failed"
                                                                : negotiation.error_code,
                                 negotiation.explanation);
        }
        const auto input = Value((*arguments)["input"]);
        const auto artifact_id = required_string(input, "artifact_id");
        if (!artifact_id || !input["offset"].isInt64() || !input["max_bytes"].isInt64())
            return error_payload("invalid-arguments", "artifact read input is incomplete");
        const auto offset = static_cast<std::uint64_t>(input["offset"].getInt64());
        const auto max_bytes = static_cast<std::size_t>(input["max_bytes"].getInt64());
        if (remaining() <= std::chrono::milliseconds::zero())
            return error_payload("timeout", "the control operation timed out");
        const auto result = client.read_artifact(*artifact_id, offset, max_bytes, remaining());
        if (result.status != ControlArtifactStatus::Read) {
            if (result.status == ControlArtifactStatus::IoError ||
                result.status == ControlArtifactStatus::Corrupt)
                impl_->mark_session_unhealthy();
            if (remaining() <= std::chrono::milliseconds::zero())
                return error_payload("timeout", "the control operation timed out");
            return error_payload(control_artifact_status_id(result.status), result.explanation);
        }
        if (!result.metadata ||
            result.metadata->lineage.producer_registration_id !=
                (*item)["registration_id"].getString() ||
            result.metadata->lineage.instance_id != *instance_id ||
            result.metadata->lineage.publication_id != (*item)["publication_id"].getString())
            return error_payload("artifact-instance-mismatch",
                                 "artifact lineage does not belong to the exact instance");
        auto root = choc::value::createObject("");
        root.addMember("artifact_id", choc::value::createString(*artifact_id));
        root.addMember("chunk_base64", choc::value::createString(pulp::runtime::base64_encode(result.bytes.data(), result.bytes.size())));
        root.addMember("eof", choc::value::createBool(result.eof));
        root.addMember("sha256", choc::value::createString(result.metadata->sha256));
        return success_payload(choc::json::toString(root, false));
    }

    std::string grant_id = required_string(*arguments, "grant_id").value_or("");
    const auto risk = capability_risk(operation->capability);
    if (grant_id.empty()) {
        const auto profile = required_string(*arguments, "profile").value_or(
            risk == InspectorCapabilityRisk::Observe || risk == InspectorCapabilityRisk::Sensitive
                ? "inspect-readonly" : "develop");
        const bool operation_specific = risk == InspectorCapabilityRisk::Critical ||
            std::ranges::find(profile_capabilities(profile == "inspect-readonly"
                                                       ? InspectorProfile::Observe
                                                       : InspectorProfile::Develop),
                              operation->capability) ==
                profile_capabilities(profile == "inspect-readonly"
                                         ? InspectorProfile::Observe
                                         : InspectorProfile::Develop).end();
        const auto authority = operation_specific ? operation->id : std::string_view(profile);
        if (remaining() <= std::chrono::milliseconds::zero())
            return error_payload("timeout", "the control operation timed out");
        if (auto grant_error = impl_->grant(*instance_id,
                                            (*item)["publication_id"].getString(),
                                            authority, operation_specific, grant_id, remaining());
            !grant_error.empty())
            return grant_error;
    }
    if (remaining() <= std::chrono::milliseconds::zero())
        return error_payload("timeout", "the control operation timed out");
    const auto negotiation = client.negotiate({
        .versions = {kControlProtocolVersion, kControlProtocolVersion},
        .mandatory_features = {"receipts"},
        .optional_features = {"artifacts", "cancellation", "progress"},
    }, remaining());
    if (!negotiation.response || negotiation.response->status != ControlNegotiationStatus::Accepted)
    {
        if (!negotiation.response)
            impl_->mark_session_unhealthy();
        return error_payload(negotiation.error_code.empty() ? "negotiation-failed"
                                                            : negotiation.error_code,
                             negotiation.explanation);
    }
    if (std::ranges::find(negotiation.response->features, "receipts") ==
        negotiation.response->features.end())
        return error_payload("capability-negotiation-failed", "broker omitted mandatory receipts");

    const auto request_id = required_string(*arguments, "request_id").value_or(
        random_token("mcp-request-"));
    const auto idempotency = random_token("mcp-idempotency-");
    if (request_id.empty() || idempotency.empty())
        return error_payload("entropy-unavailable", "request entropy is unavailable");
    const auto expected_generation_value = arguments->hasObjectMember("expected_state_generation") &&
                                             ((*arguments)["expected_state_generation"].isInt32() ||
                                              (*arguments)["expected_state_generation"].isInt64())
                                         ? (*arguments)["expected_state_generation"].getInt64()
                                         : 0;
    if (expected_generation_value < 0)
        return error_payload("invalid-arguments",
                             "expected_state_generation must not be negative");
    const auto expected_generation = static_cast<std::uint64_t>(expected_generation_value);
    ControlRequestEnvelope request{
        .request_id = request_id,
        .client_id = std::string(impl_->session->client_id()),
        .registration_id = std::string((*item)["registration_id"].getString()),
        .grant_id = grant_id,
        .instance_generation = std::string((*item)["publication_id"].getString()),
        .operation_id = std::string(operation->id),
        .operation_version = operation->version,
        .idempotency_key = idempotency,
        .deadline_unix_ms = operation_deadline->unix_deadline_ms(),
        .expected_state_generation = expected_generation,
        .params_json = input_json,
    };
    request.request_hash = control_request_hash(request).value_or("");
    if (request.request_hash.empty())
        return error_payload("invalid-control-request", "canonical control request is invalid");

    impl_->progress_events.clear();
    impl_->active_progress_token = std::string(progress_token);
    impl_->register_active_request(request_id, *instance_id,
                                   std::string((*item)["publication_id"].getString()));
    if (remaining() <= std::chrono::milliseconds::zero()) {
        impl_->finish_active_request(request_id);
        impl_->active_progress_token.clear();
        return error_payload("timeout", "the control operation timed out");
    }
    const auto result = client.request(request, remaining());
    impl_->finish_active_request(request_id);
    impl_->active_progress_token.clear();
    if (!result.response)
    {
        impl_->forget_grant(grant_id);
        impl_->mark_session_unhealthy();
        return error_payload(result.error_code, result.explanation,
                             "{\"may_have_applied\":true,\"request_id\":" +
                                 quote(request_id) + "}");
    }
    if (result.response->state != ControlReceiptState::Completed)
        impl_->forget_grant(grant_id);
    return impl_->receipt_payload(*result.response, operation);
}

std::string ControlMcpAdapter::resource_templates_payload() const {
    return R"JSON({"resourceTemplates":[{"uriTemplate":"pulp-control://instances/{instance_id}","name":"Pulp control instance","description":"Broker-owned exact-instance metadata","mimeType":"application/vnd.pulp.control.instance+json"},{"uriTemplate":"pulp-control://artifacts/{instance_id}/{artifact_id}","name":"Pulp control artifact","description":"Broker ACL-checked artifact bytes","mimeType":"application/octet-stream"}]})JSON";
}

std::string ControlMcpAdapter::resources_list_payload() {
    std::string error;
    auto data = impl_->inventory(error);
    if (!data)
        return error;
    auto resources = choc::value::createEmptyArray();
    const auto instances = (*data)["instances"];
    for (std::uint32_t index = 0; index < instances.size(); ++index) {
        const auto id = std::string(instances[index]["instance_id"].getString());
        auto item = choc::value::createObject("");
        item.addMember("uri", choc::value::createString("pulp-control://instances/" + percent_encode(id)));
        item.addMember("name", choc::value::createString(id));
        item.addMember("mimeType", choc::value::createString("application/vnd.pulp.control.instance+json"));
        resources.addArrayElement(item);
    }
    auto root = choc::value::createObject("");
    root.addMember("resources", resources);
    return choc::json::toString(root, false);
}

std::string ControlMcpAdapter::resource_read_payload(std::string_view uri) {
    constexpr std::string_view instance_prefix = "pulp-control://instances/";
    constexpr std::string_view artifact_prefix = "pulp-control://artifacts/";
    if (uri.starts_with(instance_prefix)) {
        const auto id = percent_decode(uri.substr(instance_prefix.size()));
        if (!id)
            return error_payload("invalid-resource-uri", "instance resource URI is invalid");
        std::string error;
        auto item = impl_->exact_instance(*id, error);
        if (!item)
            return error;
        const auto json = choc::json::toString(*item, false);
        return "{\"contents\":[{\"uri\":" + quote(uri) +
               ",\"mimeType\":\"application/vnd.pulp.control.instance+json\",\"text\":" +
               quote(json) + "}]}";
    }
    if (uri.starts_with(artifact_prefix)) {
        constexpr std::size_t maximum_resource_bytes = 64u * 1024u * 1024u;
        auto path = uri.substr(artifact_prefix.size());
        const auto slash = path.find('/');
        if (slash == std::string_view::npos)
            return error_payload("invalid-resource-uri", "artifact resource URI is invalid");
        const auto instance_id = percent_decode(path.substr(0, slash));
        const auto artifact_id = percent_decode(path.substr(slash + 1));
        if (!instance_id || !artifact_id)
            return error_payload("invalid-resource-uri", "artifact resource URI is invalid");
        std::vector<std::uint8_t> bytes;
        bool eof = false;
        while (!eof) {
            auto arguments = choc::value::createObject("");
            arguments.addMember("instance_id", choc::value::createString(*instance_id));
            auto input = choc::value::createObject("");
            input.addMember("artifact_id", choc::value::createString(*artifact_id));
            input.addMember("offset", choc::value::createInt64(static_cast<std::int64_t>(bytes.size())));
            input.addMember("max_bytes", choc::value::createInt64(kControlMaximumArtifactReadBytes));
            arguments.addMember("input", input);
            const auto payload = call_tool("pulp_control_artifact_read",
                                           choc::json::toString(arguments, false));
            const auto parsed = parse_object(payload);
            if (!parsed || parsed->hasObjectMember("isError"))
                return payload;
            const auto structured = (*parsed)["structuredContent"];
            if (!structured.isObject() || !structured.hasObjectMember("chunk_base64") ||
                !structured["chunk_base64"].isString() ||
                !structured.hasObjectMember("eof") || !structured["eof"].isBool())
                return error_payload("invalid-control-response",
                                     "artifact chunk response is malformed");
            const auto chunk = pulp::runtime::base64_decode(structured["chunk_base64"].getString());
            if (!chunk || (chunk->empty() && !structured["eof"].getBool()))
                return error_payload("invalid-control-response",
                                     "artifact chunk did not advance the resource cursor");
            if (chunk->size() > maximum_resource_bytes - bytes.size())
                return error_payload("resource-too-large",
                                     "artifact exceeds the 64 MiB MCP resource limit");
            bytes.insert(bytes.end(), chunk->begin(), chunk->end());
            eof = structured["eof"].getBool();
        }
        return "{\"contents\":[{\"uri\":" + quote(uri) +
               ",\"mimeType\":\"application/octet-stream\",\"blob\":" +
               quote(pulp::runtime::base64_encode(bytes.data(), bytes.size())) + "}]}";
    }
    return error_payload("resource-not-found", "unknown control resource URI");
}

void configure_control_mcp_executable(std::string executable_path) {
    std::lock_guard lock(g_adapter_mutex);
    g_executable_path = std::move(executable_path);
    g_adapter.reset();
}
void set_control_mcp_session_factory_for_test(ControlMcpSessionFactory factory) {
    std::lock_guard lock(g_adapter_mutex);
    g_test_factory = std::move(factory);
    g_adapter.reset();
}
void reset_control_mcp_session_factory_for_test() {
    std::lock_guard lock(g_adapter_mutex);
    g_test_factory = {};
    g_adapter.reset();
}
void set_control_mcp_notification_sink(ControlMcpNotificationSink sink) {
    std::lock_guard lock(g_adapter_mutex);
    g_notification_sink = std::move(sink);
    g_adapter.reset();
}
std::string control_mcp_tools_json_fragment() {
    return global_adapter()->tools_json_fragment();
}
bool is_control_mcp_tool(std::string_view name) {
    return global_adapter()->owns_tool(name);
}
std::string handle_control_mcp_tool(std::string_view name, std::string_view arguments_json,
                                    std::string_view progress_token) {
    auto adapter = global_adapter();
    if (name == kCancelTool)
        return adapter->call_tool(name, arguments_json, progress_token);
    std::lock_guard lock(g_adapter_call_mutex);
    return adapter->call_tool(name, arguments_json, progress_token);
}
std::string control_mcp_resource_templates_payload() {
    return global_adapter()->resource_templates_payload();
}
std::string control_mcp_resources_list_payload() {
    auto adapter = global_adapter();
    std::lock_guard lock(g_adapter_call_mutex);
    return adapter->resources_list_payload();
}
std::string control_mcp_resource_read_payload(std::string_view uri) {
    auto adapter = global_adapter();
    std::lock_guard lock(g_adapter_call_mutex);
    return adapter->resource_read_payload(uri);
}
} // namespace pulp_mcp

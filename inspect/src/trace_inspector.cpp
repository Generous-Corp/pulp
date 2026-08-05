#include <pulp/inspect/trace_inspector.hpp>

#include <pulp/runtime/trace.hpp>          // kTracingEnabled
#include <pulp/runtime/trace_session.hpp>  // Tracing, TraceStopResult

#include <choc/text/choc_JSON.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pulp::inspect {

namespace {
using pulp::runtime::Tracing;
using pulp::runtime::kTracingEnabled;
constexpr std::int64_t kMinTraceRingMb = 1;
constexpr std::int64_t kMaxTraceRingMb = 512;
constexpr std::size_t kMaxTraceCategories = 128;
constexpr std::size_t kMaxTraceCategoryBytes = 128;

std::optional<std::int64_t> schema_integer(
    choc::value::ValueView value, std::int64_t minimum,
    std::int64_t maximum) {
    if (value.isInt32() || value.isInt64()) {
        const auto integer = value.isInt32()
            ? static_cast<std::int64_t>(value.getInt32())
            : value.getInt64();
        if (integer >= minimum && integer <= maximum) return integer;
        return std::nullopt;
    }
    if (!value.isFloat32() && !value.isFloat64()) return std::nullopt;
    const auto number = value.isFloat64()
        ? value.getFloat64()
        : static_cast<double>(value.getFloat32());
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum))
        return std::nullopt;
    return static_cast<std::int64_t>(number);
}
}  // namespace

class TraceInspector::PublicationLease final
    : public InspectorPublicationLease {
public:
    PublicationLease(
        TraceInspector& inspector,
        TracePublicationOwner owner)
        : inspector_(&inspector), owner_(std::move(owner)) {}

    ~PublicationLease() override {
        inspector_->release_publication(owner_);
    }

private:
    TraceInspector* inspector_;
    TracePublicationOwner owner_;
};

TraceInspector::~TraceInspector() {
    std::lock_guard lock(mutex_);
    if (ownership_)
        (void)Tracing::stop_owned(*ownership_);
}

std::unique_ptr<InspectorPublicationLease>
TraceInspector::bind_publication(
    const InspectorDiscoveryRecord& record) {
    if (record.session_id.empty() || record.instance_id.empty() ||
        record.publication_id.empty()) {
        return nullptr;
    }
    TracePublicationOwner owner{
        record.session_id,
        record.instance_id,
        record.publication_id,
    };
    std::lock_guard lock(mutex_);
    if (owner_.complete())
        return nullptr;
    auto lease = std::make_unique<PublicationLease>(*this, owner);
    owner_ = std::move(owner);
    last_trace_path_.clear();
    return lease;
}

void TraceInspector::release_publication(
    const TracePublicationOwner& owner) noexcept {
    std::lock_guard lock(mutex_);
    if (owner_ != owner)
        return;
    if (ownership_)
        (void)Tracing::stop_owned(*ownership_);
    ownership_.reset();
    owner_ = {};
}

bool TraceInspector::owns_method(const std::string& method) {
    return method == methods::kTraceStartSession
        || method == methods::kTraceStopSession
        || method == methods::kTraceSnapshot
        || method == methods::kTraceQuery
        || method == methods::kTraceExplain;
}

InspectorMessage TraceInspector::handle(const InspectorMessage& req) {
    if (req.method == methods::kTraceStartSession) return start_session(req);
    if (req.method == methods::kTraceStopSession)  return stop_session(req);
    if (req.method == methods::kTraceSnapshot)     return snapshot(req);
    if (req.method == methods::kTraceQuery)        return query(req);
    if (req.method == methods::kTraceExplain)      return explain(req);
    return make_error(req.id, "Unknown Trace method: " + req.method);
}

InspectorMessage TraceInspector::start_session(const InspectorMessage& req) {
    choc::value::Value params;
    try {
        params = choc::json::parse(req.params_json);
    } catch (...) {
        return make_error(req.id, "Trace.startSession: invalid params JSON");
    }

    if (!params.isObject()) {
        return make_error(req.id,
                          "Trace.startSession: params must be an object",
                          "invalid_params");
    }

    // Keep the live adapter closed exactly like trace/session-control@1.
    // out_path retains its more specific host-authority error below.
    if (params.hasObjectMember("out_path")) {
        return make_error(
            req.id,
            "Trace.startSession: out_path is unavailable over the inspector; "
            "the host owns the trace destination",
            "invalid_params");
    }
    bool has_unknown_field = false;
    params.getView().visitObjectMembers(
        [&](std::string_view name, const choc::value::ValueView&) {
            if (name != "categories" && name != "ring_mb")
                has_unknown_field = true;
        });
    if (has_unknown_field) {
        return make_error(
            req.id,
            "Trace.startSession: params contain unsupported fields",
            "invalid_params");
    }

    std::vector<std::string> categories;
    std::set<std::string> unique_categories;
    if (params.hasObjectMember("categories")) {
        const auto& arr = params["categories"];
        if (!arr.isArray() || arr.size() > kMaxTraceCategories) {
            return make_error(
                req.id,
                "Trace.startSession: categories must be an array of at most 128 strings",
                "invalid_params");
        }
        for (uint32_t i = 0; i < arr.size(); ++i) {
            const auto& category = arr[i];
            if (!category.isString()) {
                return make_error(
                    req.id,
                    "Trace.startSession: every category must be a string",
                    "invalid_params");
            }
            std::string value(category.getString());
            if (value.empty() || value.size() > kMaxTraceCategoryBytes) {
                return make_error(
                    req.id,
                    "Trace.startSession: categories must contain 1 to 128 UTF-8 bytes",
                    "invalid_params");
            }
            if (!unique_categories.insert(value).second) {
                return make_error(
                    req.id,
                    "Trace.startSession: categories must be unique",
                    "invalid_params");
            }
            categories.push_back(std::move(value));
        }
    }

    // The CLI sizes the ring in megabytes; Tracing takes kilobytes. Absent →
    // Tracing's own 80 MB default.
    std::uint32_t ring_kb = 80u * 1024u;
    if (params.isObject() && params.hasObjectMember("ring_mb")) {
        const auto ring_mb = schema_integer(
            params["ring_mb"], kMinTraceRingMb, kMaxTraceRingMb);
        if (!ring_mb) {
            return make_error(
                req.id,
                "Trace.startSession: ring_mb must be an integer between 1 and 512",
                "invalid_params");
        }
        ring_kb = static_cast<std::uint32_t>(*ring_mb) * 1024u;
    }

    if (!kTracingEnabled) {
        return make_error(
            req.id,
            "Trace.startSession: tracing is not compiled into this build; "
            "rebuild with -DPULP_TRACING=ON",
            "tracing_unavailable");
    }

    // An authenticated peer can control capture, not host filesystem paths.
    // Empty delegates the destination to host-owned trace configuration.
    std::lock_guard lock(mutex_);
    if (!owner_.complete()) {
        return make_error(
            req.id,
            "Trace.startSession: trace control is not bound to an active "
            "inspector publication",
            "trace_owner_unbound");
    }
    auto started = Tracing::start_exclusive(categories, {}, ring_kb);
    if (started.status == pulp::runtime::TraceStartStatus::Unavailable) {
        return make_error(
            req.id,
            "Trace.startSession: tracing could not be started",
            "trace_start_failed");
    }
    if (started.status == pulp::runtime::TraceStartStatus::AlreadyActive) {
        if (!Tracing::ownership_status(
                 ownership_ ? &*ownership_ : nullptr).owned) {
            return make_error(
                req.id,
                "Trace.startSession: another controller owns the "
                "active capture",
                "trace_owned_by_another_controller");
        }
        return make_error(
            req.id, "Trace.startSession: a tracing session is already active",
            "trace_already_active");
    }
    if (!started.ownership) {
        return make_error(
            req.id,
            "Trace.startSession: tracing started without an ownership "
            "capability",
            "trace_start_failed");
    }
    ownership_ = std::move(started.ownership);

    auto out = choc::value::createObject("");
    out.addMember("compiled_in", choc::value::createBool(kTracingEnabled));
    out.addMember("active", choc::value::createBool(Tracing::active()));
    out.addMember("ok", choc::value::createBool(true));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage TraceInspector::stop_session(const InspectorMessage& req) {
    choc::value::Value params;
    try {
        params = choc::json::parse(
            req.params_json.empty() ? std::string_view("{}")
                                    : std::string_view(req.params_json));
    } catch (...) {
        return make_error(req.id, "Trace.stopSession: invalid params JSON",
                          "invalid_params");
    }
    bool has_fields = false;
    if (params.isObject()) {
        params.getView().visitObjectMembers(
            [&](std::string_view, const choc::value::ValueView&) {
                has_fields = true;
            });
    }
    if (!params.isObject() || has_fields) {
        return make_error(req.id,
                          "Trace.stopSession: params must be an empty object",
                          "invalid_params");
    }
    if (!kTracingEnabled) {
        return make_error(
            req.id,
            "Trace.stopSession: tracing is not compiled into this build; "
            "rebuild with -DPULP_TRACING=ON",
            "tracing_unavailable");
    }
    std::lock_guard lock(mutex_);
    if (!Tracing::active()) {
        ownership_.reset();
        return make_error(
            req.id, "Trace.stopSession: no active tracing session",
            "no_active_trace");
    }
    if (!owner_.complete()) {
        return make_error(
            req.id,
            "Trace.stopSession: trace control is not bound to an active "
            "inspector publication",
            "trace_owner_unbound");
    }
    if (!Tracing::ownership_status(
             ownership_ ? &*ownership_ : nullptr).owned) {
        ownership_.reset();
        return make_error(
            req.id,
            "Trace.stopSession: another controller owns the active capture",
            "trace_owned_by_another_controller");
    }

    const auto result = Tracing::stop_owned(*ownership_);
    ownership_.reset();
    if (!result.ok) {
        return make_error(
            req.id,
            "Trace.stopSession: the active trace could not be written",
            "trace_write_failed");
    }
    last_trace_path_ = result.path;

    auto out = choc::value::createObject("");
    out.addMember("ok", choc::value::createBool(true));
    out.addMember("out_path", choc::value::createString(result.path));
    out.addMember("trace_bytes",
                  choc::value::createInt64(static_cast<int64_t>(result.trace_bytes)));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage TraceInspector::snapshot(const InspectorMessage& req) {
    std::lock_guard lock(mutex_);
    const auto ownership = Tracing::ownership_status(
        ownership_ ? &*ownership_ : nullptr);
    auto out = choc::value::createObject("");
    out.addMember("compiled_in", choc::value::createBool(kTracingEnabled));
    out.addMember("active", choc::value::createBool(ownership.active));
    out.addMember(
        "trace_control_available",
        choc::value::createBool(
            owner_.complete() && (!ownership.active || ownership.owned)));
    if (!last_trace_path_.empty())
        out.addMember("last_trace_path", choc::value::createString(last_trace_path_));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage TraceInspector::query(const InspectorMessage& req) {
    return make_error(
        req.id,
        "Trace.query is unavailable in the live inspector; query a flushed "
        ".pftrace with `pulp trace query \"<sql>\" --trace <file>`",
        "capability_unavailable");
}

InspectorMessage TraceInspector::explain(const InspectorMessage& req) {
    return make_error(
        req.id,
        "Trace.explain is not implemented; capture a .pftrace and run the "
        "trace-analysis workflow over offline trace queries",
        "capability_unavailable");
}

} // namespace pulp::inspect

#include <pulp/inspect/motion_inspector.hpp>

#include <pulp/view/inspector.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/motion_cost.hpp>
#include <pulp/view/ui_components.hpp>  // ScrollView (Motion.startTrace scroll-geometry)
#include <pulp/view/view.hpp>

#include <choc/text/choc_JSON.h>
#include <choc/text/choc_UTF8.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pulp::inspect {

namespace {

using pulp::view::motion::Coordinator;
using pulp::view::motion::CostAttributor;
using pulp::view::motion::CostSample;
using pulp::view::motion::GeometryProperty;
using pulp::view::motion::GeometrySource;
using pulp::view::motion::GeometrySpace;
using pulp::view::motion::Provenance;
using pulp::view::motion::SampleEvent;
using pulp::view::motion::TraceBuilder;
using pulp::view::motion::TraceHandle;
using pulp::view::motion::TraceOptions;

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

bool schema_string(choc::value::ValueView value, std::size_t minimum,
                   std::size_t maximum) {
    if (!value.isString()) return false;
    const std::string_view text = value.getString();
    if (text.empty()) return minimum == 0;
    if (choc::text::findInvalidUTF8Data(text.data(), text.size()) != nullptr)
        return false;
    const auto codepoints = static_cast<std::size_t>(std::count_if(
        text.begin(), text.end(), [](unsigned char byte) {
            return (byte & 0xc0u) != 0x80u;
        }));
    return codepoints >= minimum && codepoints <= maximum;
}

template <std::size_t N>
bool has_only_members(choc::value::ValueView value,
                      const std::array<std::string_view, N>& allowed) {
    if (!value.isObject()) return false;
    bool valid = true;
    value.visitObjectMembers(
        [&](std::string_view name, const choc::value::ValueView&) {
            if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
                valid = false;
        });
    return valid;
}

template <std::size_t N>
bool schema_string_array(choc::value::ValueView value,
                         const std::array<std::string_view, N>& allowed,
                         std::size_t maximum) {
    if (!value.isArray() || value.size() > maximum) return false;
    std::set<std::string_view> seen;
    for (std::uint32_t index = 0; index < value.size(); ++index) {
        const auto item = value[index];
        if (!item.isString()) return false;
        const std::string_view text = item.getString();
        if (std::find(allowed.begin(), allowed.end(), text) == allowed.end() ||
            !seen.insert(text).second)
            return false;
    }
    return true;
}

constexpr std::array<std::string_view, 3> kStartTraceFields = {
    "view_name", "fps", "metrics"};
constexpr std::array<std::string_view, 6> kGeometryFields = {
    "kind", "name", "node_id", "properties", "space", "source"};
constexpr std::array<std::string_view, 4> kScrollFields = {
    "kind", "name", "node_id", "properties"};
constexpr std::array<std::string_view, 8> kGeometryProperties = {
    "minX", "minY", "maxX", "maxY", "midX", "midY", "width", "height"};
constexpr std::array<std::string_view, 14> kScrollProperties = {
    "contentOffsetX", "contentOffsetY", "visibleRectMinX", "visibleRectMinY",
    "visibleRectWidth", "visibleRectHeight", "contentSizeWidth",
    "contentSizeHeight", "insetTop", "insetBottom", "insetLeft", "insetRight",
    "scrollableMaxX", "scrollableMaxY"};
constexpr std::array<std::string_view, 4> kGeometrySpaces = {
    "view-local", "view-global", "window", "screen"};
constexpr std::array<std::string_view, 2> kGeometrySources = {
    "layout", "presentation"};

template <std::size_t N>
bool schema_enum(choc::value::ValueView value,
                 const std::array<std::string_view, N>& allowed) {
    if (!value.isString()) return false;
    const std::string_view text = value.getString();
    return std::find(allowed.begin(), allowed.end(), text) != allowed.end();
}

GeometryProperty parse_geometry_property(std::string_view s) {
    if (s == "minX")   return GeometryProperty::MinX;
    if (s == "minY")   return GeometryProperty::MinY;
    if (s == "maxX")   return GeometryProperty::MaxX;
    if (s == "maxY")   return GeometryProperty::MaxY;
    if (s == "midX")   return GeometryProperty::MidX;
    if (s == "midY")   return GeometryProperty::MidY;
    if (s == "width")  return GeometryProperty::Width;
    if (s == "height") return GeometryProperty::Height;
    return GeometryProperty::MinX;
}

GeometrySpace parse_geometry_space(std::string_view s) {
    if (s == "view-local")  return GeometrySpace::ViewLocal;
    if (s == "view-global") return GeometrySpace::ViewGlobal;
    if (s == "window")      return GeometrySpace::Window;
    if (s == "screen")      return GeometrySpace::Screen;
    return GeometrySpace::Window;
}

// CamelCase property names mirror what TraceBuilder emits into fixtures,
// so callers can pass the same names back through the wire. Callers validate
// against the frozen enum before invoking this conversion.
pulp::view::motion::ScrollProperty parse_scroll_property(std::string_view s) {
    using SP = pulp::view::motion::ScrollProperty;
    if (s == "contentOffsetX")   return SP::ContentOffsetX;
    if (s == "contentOffsetY")   return SP::ContentOffsetY;
    if (s == "visibleRectMinX")  return SP::VisibleRectMinX;
    if (s == "visibleRectMinY")  return SP::VisibleRectMinY;
    if (s == "visibleRectWidth") return SP::VisibleRectWidth;
    if (s == "visibleRectHeight")return SP::VisibleRectHeight;
    if (s == "contentSizeWidth") return SP::ContentSizeWidth;
    if (s == "contentSizeHeight")return SP::ContentSizeHeight;
    if (s == "insetTop")         return SP::InsetTop;
    if (s == "insetBottom")      return SP::InsetBottom;
    if (s == "insetLeft")        return SP::InsetLeft;
    if (s == "insetRight")       return SP::InsetRight;
    if (s == "scrollableMaxX")   return SP::ScrollableMaxX;
    if (s == "scrollableMaxY")   return SP::ScrollableMaxY;
    return SP::ContentOffsetX;
}

GeometrySource parse_geometry_source(std::string_view s) {
    if (s == "layout")       return GeometrySource::Layout;
    if (s == "presentation") return GeometrySource::Presentation;
    return GeometrySource::Layout;
}

const char* sample_kind_to_string(SampleEvent::Kind k) {
    switch (k) {
        case SampleEvent::Kind::TraceStarted: return "trace-started";
        case SampleEvent::Kind::Baseline:     return "baseline";
        case SampleEvent::Kind::Sample:       return "sample";
        case SampleEvent::Kind::Start:        return "start";
        case SampleEvent::Kind::End:          return "end";
        case SampleEvent::Kind::Input:        return "input";
    }
    return "?";
}

const char* event_method_for_kind(SampleEvent::Kind k) {
    switch (k) {
        case SampleEvent::Kind::TraceStarted: return methods::kMotionStart;
        case SampleEvent::Kind::Baseline:     return methods::kMotionSample;
        case SampleEvent::Kind::Sample:       return methods::kMotionSample;
        case SampleEvent::Kind::Start:        return methods::kMotionStart;
        case SampleEvent::Kind::End:          return methods::kMotionEnd;
        case SampleEvent::Kind::Input:        return methods::kMotionSample;
    }
    return methods::kMotionSample;
}

// Match motion.cpp::format_number — NaN/Inf travel as quoted sentinels
// on the wire so a fixture round-trip and a live inspector broadcast see
// the same values. choc::value::createFloat64 would serialize non-finite
// as JSON `null`, silently dropping the misbehavior signal.
choc::value::Value wire_number(double v) {
    if (std::isnan(v))          return choc::value::createString("NaN");
    if (std::isinf(v) && v > 0) return choc::value::createString("Infinity");
    if (std::isinf(v) && v < 0) return choc::value::createString("-Infinity");
    return choc::value::createFloat64(v);
}

choc::value::Value components_to_object(
    const std::vector<std::pair<std::string, double>>& comps
) {
    auto obj = choc::value::createObject("");
    for (const auto& [k, v] : comps) {
        obj.addMember(k, wire_number(v));
    }
    return obj;
}

}  // namespace

// ── MotionInspector ──────────────────────────────────────────────────

MotionInspector::MotionInspector(pulp::view::View& root,
                                 InspectorEventSink event_sink)
    : root_(&root), event_sink_(std::move(event_sink)) {
    sink_id_ = Coordinator::instance().add_sink(
        [this](const SampleEvent& e) { broadcast_event(e); });
    cost_sink_id_ = CostAttributor::instance().add_sink(
        [this](const CostSample& s) { broadcast_cost(s); });
}

MotionInspector::~MotionInspector() {
    if (sink_id_) {
        Coordinator::instance().remove_sink(sink_id_);
        sink_id_ = 0;
    }
    if (cost_sink_id_) {
        CostAttributor::instance().remove_sink(cost_sink_id_);
        cost_sink_id_ = 0;
    }
    bool restore_cost = false;
    bool cost_enabled = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        restore_cost = !cost_owner_.empty();
        cost_enabled = cost_was_enabled_;
        cost_owner_.clear();
        cost_authority_live_ = {};
        recent_cost_samples_.clear();
        cost_trace_owners_.clear();
        traces_.clear();
        canonical_traces_.clear();
        restore_tracing_if_unused_locked();
    }
    if (restore_cost)
        CostAttributor::instance().set_enabled(cost_enabled);
}

std::size_t MotionInspector::active_trace_count() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return traces_.size() + canonical_traces_.size();
}

MotionTraceAttachResult MotionInspector::attach_trace(const MotionTraceSpec& spec,
                                                      std::string owner,
                                                      std::function<bool()> authority_live,
                                                      std::function<std::shared_ptr<void>(
                                                          std::function<void()>)>
                                                          subscribe_authority_end) {
    if (!root_)
        return {.error = "motion observation has no root view"};
    if (spec.view_name.empty() || spec.view_name.size() > 128 || spec.fps < 1 ||
        spec.fps > 240 || spec.geometry.size() + spec.scroll_geometry.size() < 1 ||
        spec.geometry.size() + spec.scroll_geometry.size() > 32)
        return {.error = "motion observation exceeded its bounded trace contract"};

    TraceBuilder builder = Coordinator::instance().trace(spec.view_name, {spec.fps});
    for (const auto& metric : spec.geometry) {
        auto* target = pulp::view::ViewInspector::find_by_id(*root_, metric.node_id);
        if (!target)
            return {.error = "geometry observation node is unavailable"};
        auto properties = metric.properties;
        if (properties.empty()) {
            properties = {GeometryProperty::MinX, GeometryProperty::MinY,
                          GeometryProperty::Width, GeometryProperty::Height};
        }
        builder.geometry(metric.name, *target, std::move(properties), metric.space,
                         metric.source);
    }
    for (const auto& metric : spec.scroll_geometry) {
        auto* target = pulp::view::ViewInspector::find_by_id(*root_, metric.node_id);
        auto* scroll = dynamic_cast<pulp::view::ScrollView*>(target);
        if (!scroll)
            return {.error = "scroll observation node is unavailable"};
        auto properties = metric.properties;
        if (properties.empty())
            builder.scroll_geometry(metric.name, *scroll);
        else
            builder.scroll_geometry(metric.name, *scroll, std::move(properties));
    }

    prune_inactive_traces();
    {
        std::lock_guard lock(mtx_);
        constexpr std::size_t kMaximumCanonicalTraces = 32;
        if (canonical_traces_.size() == kMaximumCanonicalTraces)
            return {.error = "motion observation trace quota is exhausted",
                    .resource_exhausted = true};
    }
    auto handle = builder.attach();
    if (!handle.is_attached())
        return {.error = "motion observation trace could not attach"};
    const auto trace_id = static_cast<std::int64_t>(handle.id());
    {
        std::lock_guard lock(mtx_);
        if (canonical_traces_.empty()) {
            tracing_was_enabled_ = Coordinator::instance().tracing_enabled();
            canonical_tracing_epoch_active_ = true;
        }
        Coordinator::instance().set_tracing_enabled(true);
        canonical_traces_.emplace(
            trace_id,
            CanonicalTrace{std::move(handle), owner, std::move(authority_live), {}});
        if (cost_owner_ == owner)
            cost_trace_owners_[trace_id] = owner;
    }
    auto subscription = subscribe_authority_end(
        [this, trace_id, owner = std::move(owner)] { (void)detach_trace(trace_id, owner); });
    std::lock_guard subscription_lock(mtx_);
    const auto found = canonical_traces_.find(trace_id);
    if (!subscription) {
        if (found != canonical_traces_.end())
            canonical_traces_.erase(found);
        restore_tracing_if_unused_locked();
        return {.error = "motion authority-end subscription is unavailable"};
    }
    if (found == canonical_traces_.end())
        return {.error = "motion authority ended during trace attachment"};
    found->second.authority_end_subscription = std::move(subscription);
    return {.trace_id = trace_id};
}

bool MotionInspector::begin_cost_observation(std::string owner,
                                             std::function<bool()> authority_live) {
    std::lock_guard lock(mtx_);
    if (!cost_owner_.empty() && cost_owner_ != owner && cost_authority_live_ &&
        cost_authority_live_())
        return false;
    if (cost_owner_.empty())
        cost_was_enabled_ = CostAttributor::instance().enabled();
    cost_owner_ = std::move(owner);
    cost_authority_live_ = std::move(authority_live);
    recent_cost_samples_.clear();
    cost_trace_owners_.clear();
    for (const auto& [trace_id, trace] : canonical_traces_) {
        if (trace.owner == cost_owner_)
            cost_trace_owners_[trace_id] = trace.owner;
    }
    return true;
}

bool MotionInspector::end_cost_observation(std::string_view owner) {
    bool restore_enabled = false;
    {
        std::lock_guard lock(mtx_);
        if (cost_owner_ != owner)
            return false;
        cost_owner_.clear();
        cost_authority_live_ = {};
        recent_cost_samples_.clear();
        cost_trace_owners_.clear();
        restore_enabled = cost_was_enabled_;
    }
    CostAttributor::instance().set_enabled(restore_enabled);
    return true;
}

bool MotionInspector::detach_trace(std::int64_t trace_id, std::string_view owner) {
    std::lock_guard lock(mtx_);
    const auto found = canonical_traces_.find(trace_id);
    if (found == canonical_traces_.end() || found->second.owner != owner)
        return false;
    canonical_traces_.erase(found);
    restore_tracing_if_unused_locked();
    return true;
}

void MotionInspector::prune_inactive_traces() {
    std::lock_guard lock(mtx_);
    std::erase_if(canonical_traces_, [](const auto& entry) {
        return !entry.second.authority_live || !entry.second.authority_live();
    });
    restore_tracing_if_unused_locked();
}

void MotionInspector::restore_tracing_if_unused_locked() {
    if (!canonical_traces_.empty() || !canonical_tracing_epoch_active_)
        return;
    if (!traces_.empty()) {
        canonical_tracing_epoch_active_ = false;
        return;
    }
    Coordinator::instance().set_tracing_enabled(tracing_was_enabled_);
    canonical_tracing_epoch_active_ = false;
}

std::vector<CostSample>
MotionInspector::recent_cost_samples(std::size_t maximum_samples,
                                     std::string_view owner) const {
    std::lock_guard lock(mtx_);
    if (cost_owner_ != owner)
        return {};
    maximum_samples = std::min(maximum_samples, recent_cost_samples_.size());
    std::vector<CostSample> samples{
        recent_cost_samples_.end() - static_cast<std::ptrdiff_t>(maximum_samples),
        recent_cost_samples_.end()};
    for (auto& sample : samples) {
        std::erase_if(sample.active_trace_ids, [&](int trace_id) {
            const auto found = cost_trace_owners_.find(trace_id);
            return found == cost_trace_owners_.end() || found->second != owner;
        });
        sample.active_provenance.clear();
    }
    return samples;
}

InspectorMessage MotionInspector::handle(const InspectorMessage& req) {
    if (req.method == methods::kMotionStartTrace)  return start_trace(req);
    if (req.method == methods::kMotionStopTrace)   return stop_trace(req);
    if (req.method == methods::kMotionSnapshot)    return snapshot(req);
    if (req.method == methods::kMotionListTraces)  return list_traces(req);
    if (req.method == methods::kMotionEnableCost)  return enable_cost(req);
    if (req.method == methods::kMotionDisableCost) return disable_cost(req);
    return make_error(req.id, "Unknown Motion method: " + req.method);
}

InspectorMessage MotionInspector::start_trace(const InspectorMessage& req) {
    if (!root_) return make_error(req.id, "MotionInspector has no root view");

    choc::value::Value params;
    try {
        params = choc::json::parse(req.params_json);
    } catch (...) {
        return make_error(req.id, "Motion.startTrace: invalid params JSON");
    }

    if (!has_only_members(params, kStartTraceFields)) {
        return make_error(req.id,
                          "Motion.startTrace: params contain unsupported fields",
                          "invalid_params");
    }

    std::string view_name = "Trace";
    if (params.hasObjectMember("view_name")) {
        if (!schema_string(params["view_name"], 1, 128))
            return make_error(
                req.id,
                "Motion.startTrace: 'view_name' must contain 1 to 128 Unicode characters",
                "invalid_params");
        view_name = std::string(params["view_name"].getString());
    }
    int fps = 15;
    if (params.hasObjectMember("fps")) {
        const auto parsed = schema_integer(params["fps"], 1, 240);
        if (!parsed)
            return make_error(req.id,
                              "Motion.startTrace: 'fps' must be an integer from 1 to 240",
                              "invalid_params");
        fps = static_cast<int>(*parsed);
    }

    if (!params.hasObjectMember("metrics") ||
        !params["metrics"].isArray()) {
        return make_error(req.id, "Motion.startTrace: 'metrics' array required",
                          "invalid_params");
    }

    const auto& metrics = params["metrics"];
    if (metrics.size() < 1 || metrics.size() > 32) {
        return make_error(
            req.id,
            "Motion.startTrace: 'metrics' must contain 1 to 32 entries",
            "invalid_params");
    }

    // Validate the entire frozen request shape before creating a TraceBuilder.
    // A malformed later metric must not partially configure or attach a trace.
    for (uint32_t i = 0; i < metrics.size(); ++i) {
        const auto& m = metrics[i];
        if (!m.isObject() || !m.hasObjectMember("kind") || !m["kind"].isString()) {
            return make_error(req.id, "Motion.startTrace: metric requires string 'kind'",
                              "invalid_params");
        }
        const std::string_view kind = m["kind"].getString();
        const auto valid_common_fields = [&]() {
            return m.hasObjectMember("node_id") &&
                   schema_string(m["node_id"], 1, 256) &&
                   (!m.hasObjectMember("name") ||
                    schema_string(m["name"], 1, 128));
        };

        if (kind == "geometry") {
            const bool valid = has_only_members(m, kGeometryFields) &&
                               valid_common_fields() &&
                               (!m.hasObjectMember("properties") ||
                                schema_string_array(m["properties"],
                                                    kGeometryProperties, 8)) &&
                               (!m.hasObjectMember("space") ||
                                schema_enum(m["space"], kGeometrySpaces)) &&
                               (!m.hasObjectMember("source") ||
                                schema_enum(m["source"], kGeometrySources));
            if (!valid)
                return make_error(req.id,
                                  "Motion.startTrace: invalid geometry metric shape",
                                  "invalid_params");
        } else if (kind == "scroll-geometry" || kind == "scrollGeometry") {
            const bool valid = has_only_members(m, kScrollFields) &&
                               valid_common_fields() &&
                               (!m.hasObjectMember("properties") ||
                                schema_string_array(m["properties"],
                                                    kScrollProperties, 14));
            if (!valid)
                return make_error(req.id,
                                  "Motion.startTrace: invalid scroll metric shape",
                                  "invalid_params");
        } else {
            return make_error(req.id,
                              "Motion.startTrace: unsupported metric kind: " +
                                  std::string(kind),
                              "invalid_params");
        }
    }

    TraceBuilder builder = Coordinator::instance().trace(view_name, {fps});
    for (uint32_t i = 0; i < metrics.size(); ++i) {
        const auto& m = metrics[i];
        const std::string kind(m["kind"].getString());
        const std::string name = m.hasObjectMember("name")
                                     ? std::string(m["name"].getString())
                                     : kind;

        if (kind == "geometry") {
            const std::string node_id(m["node_id"].getString());
            auto* target = pulp::view::ViewInspector::find_by_id(*root_, node_id);
            if (!target) {
                return make_error(req.id,
                                  "Motion.startTrace: node not found: " + node_id);
            }
            std::vector<GeometryProperty> props;
            if (m.hasObjectMember("properties")) {
                const auto& parr = m["properties"];
                for (uint32_t j = 0; j < parr.size(); ++j) {
                    props.push_back(parse_geometry_property(parr[j].getString()));
                }
            }
            if (props.empty()) {
                props = {GeometryProperty::MinX, GeometryProperty::MinY,
                         GeometryProperty::Width, GeometryProperty::Height};
            }
            const auto space = m.hasObjectMember("space")
                                   ? parse_geometry_space(m["space"].getString())
                                   : GeometrySpace::Window;
            const auto source = m.hasObjectMember("source")
                                    ? parse_geometry_source(m["source"].getString())
                                    : GeometrySource::Layout;
            builder.geometry(name, *target, std::move(props), space, source);
        } else if (kind == "scroll-geometry" || kind == "scrollGeometry") {
            // Scroll geometry tracing over a ScrollView. Accept both
            // "scroll-geometry" (kebab-case, matches our other
            // inspector spellings) and the camelCase form Swift /
            // Kotlin facades pass through verbatim.
            const std::string node_id(m["node_id"].getString());
            auto* view_target = pulp::view::ViewInspector::find_by_id(*root_, node_id);
            if (!view_target) {
                return make_error(req.id,
                                  "Motion.startTrace: node not found: " + node_id);
            }
            auto* scroll_target = dynamic_cast<pulp::view::ScrollView*>(view_target);
            if (!scroll_target) {
                return make_error(req.id,
                    "Motion.startTrace: scroll-geometry node is not a ScrollView: "
                    + node_id);
            }
            std::vector<pulp::view::motion::ScrollProperty> props;
            if (m.hasObjectMember("properties")) {
                const auto& parr = m["properties"];
                for (uint32_t j = 0; j < parr.size(); ++j) {
                    props.push_back(parse_scroll_property(parr[j].getString()));
                }
            }
            // Empty props → builder's default 4-property set.
            if (props.empty()) {
                builder.scroll_geometry(name, *scroll_target);
            } else {
                builder.scroll_geometry(name, *scroll_target, std::move(props));
            }
        }
    }

    auto handle = builder.attach();
    if (!handle.is_attached()) {
        return make_error(req.id, "Motion.startTrace: attach failed");
    }
    Coordinator::instance().set_tracing_enabled(true);

    std::int64_t inspector_id = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        inspector_id = next_inspector_id_++;
        traces_.emplace(inspector_id, std::move(handle));
    }

    auto out = choc::value::createObject("");
    out.addMember("trace_id", choc::value::createInt64(inspector_id));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::stop_trace(const InspectorMessage& req) {
    choc::value::Value params;
    try {
        params = choc::json::parse(req.params_json);
    } catch (...) {
        return make_error(req.id, "Motion.stopTrace: invalid params JSON");
    }
    if (!params.isObject() || !params.hasObjectMember("trace_id")) {
        return make_error(req.id, "Motion.stopTrace: 'trace_id' required");
    }
    constexpr std::int64_t kMaximumJsonSafeInteger = 9007199254740991LL;
    const auto parsed = schema_integer(
        params["trace_id"], 0, kMaximumJsonSafeInteger);
    if (!parsed)
        return make_error(
            req.id,
            "Motion.stopTrace: 'trace_id' must be a nonnegative JSON-safe integer",
            "invalid_params");
    const std::int64_t id = *parsed;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = traces_.find(id);
        if (it != traces_.end()) {
            traces_.erase(it);
            removed = true;
        }
    }
    auto out = choc::value::createObject("");
    out.addMember("removed", choc::value::createBool(removed));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::snapshot(const InspectorMessage& req) {
    auto out = choc::value::createObject("");
    out.addMember("tracing_enabled",
                  choc::value::createBool(Coordinator::instance().tracing_enabled()));
    out.addMember("firehose",
                  choc::value::createBool(Coordinator::instance().firehose()));
    out.addMember("active_traces",
                  choc::value::createInt64(static_cast<int64_t>(
                      Coordinator::instance().active_trace_count())));
    out.addMember("inspector_traces",
                  choc::value::createInt64(static_cast<int64_t>(active_trace_count())));
    out.addMember("emitted_events",
                  choc::value::createInt64(static_cast<int64_t>(
                      Coordinator::instance().emitted_event_count())));
    out.addMember("cost_enabled",
                  choc::value::createBool(CostAttributor::instance().enabled()));
    out.addMember("cost_samples_emitted",
                  choc::value::createInt64(static_cast<int64_t>(
                      CostAttributor::instance().emitted_sample_count())));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::enable_cost(const InspectorMessage& req) {
    CostAttributor::instance().set_enabled(true);
    auto out = choc::value::createObject("");
    out.addMember("cost_enabled", choc::value::createBool(true));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::disable_cost(const InspectorMessage& req) {
    CostAttributor::instance().set_enabled(false);
    auto out = choc::value::createObject("");
    out.addMember("cost_enabled", choc::value::createBool(false));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionInspector::list_traces(const InspectorMessage& req) {
    auto out = choc::value::createObject("");
    auto arr = choc::value::createEmptyArray();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& [id, handle] : traces_) {
            (void)handle;
            arr.addArrayElement(choc::value::createInt64(id));
        }
    }
    out.addMember("trace_ids", arr);
    return make_response(req.id, choc::json::toString(out, false));
}

void MotionInspector::broadcast_event(const SampleEvent& e) {
    prune_inactive_traces();
    if (!event_sink_) return;

    auto params = choc::value::createObject("");
    params.addMember("view_name", choc::value::createString(e.view_name));
    params.addMember("metric_name", choc::value::createString(e.metric_name));
    params.addMember("kind", choc::value::createString(sample_kind_to_string(e.kind)));
    params.addMember("t", wire_number(e.t_seconds));
    params.addMember("frame", choc::value::createInt64(static_cast<int64_t>(e.frame)));
    // Stable identifiers let clients align bursts on the wire the same
    // way the fixture format aligns them.
    params.addMember("trace_id", choc::value::createInt64(e.trace_id));
    params.addMember("metric_id", choc::value::createInt64(e.metric_id));
    params.addMember("burst_id", choc::value::createInt64(e.burst_id));
    // Input events carry input_kind + view_id; without these the wire
    // form loses the entire payload of the event.
    if (e.kind == SampleEvent::Kind::Input) {
        params.addMember("input_kind", choc::value::createString(e.input_kind));
        params.addMember("view_id",    choc::value::createString(e.view_id));
    }
    if (!e.components.empty()) {
        params.addMember("components", components_to_object(e.components));
    }
    if (!e.deltas.empty()) {
        params.addMember("deltas", components_to_object(e.deltas));
    }
    if (e.provenance.is_set()) {
        auto prov = choc::value::createObject("");
        prov.addMember("source_kind", choc::value::createString(e.provenance.source_kind));
        prov.addMember("source_id",   choc::value::createString(e.provenance.source_id));
        prov.addMember("source_file", choc::value::createString(e.provenance.source_file));
        prov.addMember("source_line", choc::value::createInt64(e.provenance.source_line));
        params.addMember("provenance", prov);
    }

    InspectorMessage ev = make_event(event_method_for_kind(e.kind),
                                     choc::json::toString(params, false));
    event_sink_(ev);
}

void MotionInspector::broadcast_cost(const CostSample& s) {
    prune_inactive_traces();
    std::optional<bool> restore_cost_enabled;
    {
        std::lock_guard lock(mtx_);
        if (!cost_owner_.empty() &&
            (!cost_authority_live_ || !cost_authority_live_())) {
            restore_cost_enabled = cost_was_enabled_;
            cost_owner_.clear();
            cost_authority_live_ = {};
            recent_cost_samples_.clear();
            cost_trace_owners_.clear();
        } else if (!cost_owner_.empty()) {
            constexpr std::size_t kMaximumRetainedCostSamples = 64;
            if (recent_cost_samples_.size() == kMaximumRetainedCostSamples)
                recent_cost_samples_.pop_front();
            recent_cost_samples_.push_back(s);
        }
    }
    if (restore_cost_enabled)
        CostAttributor::instance().set_enabled(*restore_cost_enabled);
    if (!event_sink_) return;
    auto params = choc::value::createObject("");
    params.addMember("frame",
                     choc::value::createInt64(static_cast<int64_t>(s.frame)));
    params.addMember("t", wire_number(s.t_seconds));
    params.addMember("render_pass_duration_ms",
                     wire_number(s.render_pass_duration_ms));
    params.addMember("dirty_rect_area_px",
                     wire_number(s.dirty_rect_area_px));
    params.addMember("dirty_rect_count",
                     choc::value::createInt64(s.dirty_rect_count));

    auto ids = choc::value::createEmptyArray();
    for (int id : s.active_trace_ids) {
        ids.addArrayElement(choc::value::createInt64(id));
    }
    params.addMember("active_trace_ids", ids);

    auto provs = choc::value::createEmptyArray();
    for (const auto& p : s.active_provenance) {
        auto obj = choc::value::createObject("");
        obj.addMember("source_kind", choc::value::createString(p.source_kind));
        obj.addMember("source_id",   choc::value::createString(p.source_id));
        obj.addMember("source_file", choc::value::createString(p.source_file));
        obj.addMember("source_line", choc::value::createInt64(p.source_line));
        provs.addArrayElement(obj);
    }
    params.addMember("active_provenance", provs);

    InspectorMessage ev = make_event(methods::kMotionCost,
                                     choc::json::toString(params, false));
    event_sink_(ev);
}

} // namespace pulp::inspect

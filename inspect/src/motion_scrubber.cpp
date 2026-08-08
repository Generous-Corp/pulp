// motion_scrubber.cpp — see motion_scrubber.hpp
//
// Wire shape for re-emitted events deliberately matches
// MotionInspector::broadcast_event so existing clients can consume
// scrubber output without a separate decoder.

#include <pulp/inspect/motion_scrubber.hpp>

#include <pulp/view/motion.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::inspect {

namespace {

using pulp::view::motion::SampleEvent;
using pulp::view::motion::Sink;

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

choc::value::Value event_to_params(const SampleEvent& e) {
    auto params = choc::value::createObject("");
    params.addMember("view_name", choc::value::createString(e.view_name));
    params.addMember("metric_name", choc::value::createString(e.metric_name));
    params.addMember("kind", choc::value::createString(sample_kind_to_string(e.kind)));
    params.addMember("t", wire_number(e.t_seconds));
    params.addMember("frame", choc::value::createInt64(static_cast<int64_t>(e.frame)));
    params.addMember("trace_id", choc::value::createInt64(e.trace_id));
    params.addMember("metric_id", choc::value::createInt64(e.metric_id));
    params.addMember("burst_id", choc::value::createInt64(e.burst_id));
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
    // Scrubber-only marker so receivers can distinguish replayed events
    // from live coordinator events on the same wire.
    params.addMember("replay", choc::value::createBool(true));
    return params;
}

}  // namespace

// ── ctor / dtor ──────────────────────────────────────────────────────

MotionScrubber::MotionScrubber(InspectorEventSink event_sink)
    : event_sink_(std::move(event_sink)) {}

MotionScrubber::~MotionScrubber() {
    std::lock_guard<std::mutex> lock(mtx_);
    sinks_.clear();
    events_.clear();
}

// ── method routing ──────────────────────────────────────────────────

bool MotionScrubber::owns_method(const std::string& method) {
    return method == methods::kMotionLoadFixture
        || method == methods::kMotionScrubTo
        || method == methods::kMotionPlay
        || method == methods::kMotionPause;
}

InspectorMessage MotionScrubber::handle(const InspectorMessage& req) {
    if (req.method == methods::kMotionLoadFixture) return handle_load_fixture(req);
    if (req.method == methods::kMotionScrubTo)     return handle_scrub_to(req);
    if (req.method == methods::kMotionPlay)        return handle_play(req);
    if (req.method == methods::kMotionPause)       return handle_pause(req);
    return make_error(req.id, "Unknown Motion scrubber method: " + req.method);
}

// ── direct API ──────────────────────────────────────────────────────

bool MotionScrubber::load_fixture(const std::string& path) {
    auto events = view::motion::load_fixture(path);
    auto header = view::motion::load_fixture_header(path);
    if (header.version != view::motion::kFixtureSchemaVersion) {
        std::lock_guard<std::mutex> lock(mtx_);
        events_.clear();
        header_ = {};
        playhead_frame_ = 0;
        max_frame_ = 0;
        loaded_ = false;
        playing_ = false;
        return false;
    }

    std::uint64_t max_frame = 0;
    for (const auto& e : events) {
        if (e.frame > max_frame) max_frame = e.frame;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    events_ = std::move(events);
    header_ = header;
    playhead_frame_ = 0;
    max_frame_ = max_frame;
    loaded_ = true;
    playing_ = false;
    return true;
}

std::size_t MotionScrubber::scrub_to(std::uint64_t frame) {
    std::vector<view::motion::SampleEvent> snapshot;
    std::vector<SinkSlot> sinks_snapshot;
    InspectorEventSink event_sink_snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!loaded_) return 0;
        playhead_frame_ = frame;
        snapshot.reserve(events_.size());
        for (const auto& e : events_) {
            if (e.frame <= frame) snapshot.push_back(e);
        }
        sinks_snapshot = sinks_;
        event_sink_snapshot = event_sink_;
    }
    dispatch_snapshot(snapshot, sinks_snapshot, event_sink_snapshot);
    return snapshot.size();
}

std::size_t MotionScrubber::play() {
    std::vector<view::motion::SampleEvent> snapshot;
    std::vector<SinkSlot> sinks_snapshot;
    InspectorEventSink event_sink_snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!loaded_) return 0;
        playing_ = true;
        playhead_frame_ = max_frame_;
        snapshot = events_;
        sinks_snapshot = sinks_;
        event_sink_snapshot = event_sink_;
    }
    dispatch_snapshot(snapshot, sinks_snapshot, event_sink_snapshot);
    return snapshot.size();
}

void MotionScrubber::pause() {
    std::lock_guard<std::mutex> lock(mtx_);
    playing_ = false;
}

int MotionScrubber::add_sink(view::motion::Sink sink) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int id = next_sink_id_++;
    sinks_.push_back({id, std::move(sink)});
    return id;
}

void MotionScrubber::remove_sink(int id) {
    std::lock_guard<std::mutex> lock(mtx_);
    sinks_.erase(
        std::remove_if(sinks_.begin(), sinks_.end(),
                       [id](const SinkSlot& s) { return s.id == id; }),
        sinks_.end());
}

// ── accessors ───────────────────────────────────────────────────────

bool MotionScrubber::loaded() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return loaded_;
}
bool MotionScrubber::playing() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return playing_;
}
std::uint64_t MotionScrubber::playhead_frame() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return playhead_frame_;
}
std::size_t MotionScrubber::event_count() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return events_.size();
}
std::uint64_t MotionScrubber::max_frame() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return max_frame_;
}
view::motion::FixtureHeader MotionScrubber::header() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return header_;
}

// ── emission ────────────────────────────────────────────────────────

// Dispatch a snapshot of events to the given sinks + callback WITHOUT
// holding mtx_. A sink that re-enters add_sink/remove_sink/scrub_to
// (or any other MotionScrubber method) used to self-deadlock under
// the old emit_prefix_locked design.
void MotionScrubber::dispatch_snapshot(
    const std::vector<view::motion::SampleEvent>& events,
    const std::vector<SinkSlot>& sinks,
    const InspectorEventSink& event_sink
) {
    for (const auto& e : events) {
        for (const auto& slot : sinks) {
            if (slot.sink) slot.sink(e);
        }
        if (event_sink) {
            auto params = event_to_params(e);
            InspectorMessage ev = make_event(event_method_for_kind(e.kind),
                                             choc::json::toString(params, false));
            event_sink(ev);
        }
    }
}

// ── protocol handlers ───────────────────────────────────────────────

InspectorMessage MotionScrubber::handle_load_fixture(const InspectorMessage& req) {
    choc::value::Value params;
    try {
        params = choc::json::parse(req.params_json);
    } catch (...) {
        return make_error(req.id, "Motion.loadFixture: invalid params JSON");
    }
    if (!params.isObject() || !params.hasObjectMember("path")) {
        return make_error(req.id, "Motion.loadFixture: 'path' required");
    }
    std::string path(params["path"].getString());
    const bool ok = load_fixture(path);
    if (!ok) {
        return make_error(req.id, "Motion.loadFixture: failed to load fixture: " + path);
    }

    auto out = choc::value::createObject("");
    out.addMember("ok", choc::value::createBool(true));
    out.addMember("event_count",
                  choc::value::createInt64(static_cast<int64_t>(event_count())));
    out.addMember("max_frame",
                  choc::value::createInt64(static_cast<int64_t>(max_frame())));
    auto hdr = header();
    auto hdr_obj = choc::value::createObject("");
    hdr_obj.addMember("version", choc::value::createInt64(hdr.version));
    hdr_obj.addMember("policy", choc::value::createString(hdr.policy));
    hdr_obj.addMember("duration_scale", choc::value::createFloat64(hdr.duration_scale));
    out.addMember("header", hdr_obj);
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionScrubber::handle_scrub_to(const InspectorMessage& req) {
    choc::value::Value params;
    try {
        params = choc::json::parse(req.params_json);
    } catch (...) {
        return make_error(req.id, "Motion.scrubTo: invalid params JSON");
    }
    if (!params.isObject() || !params.hasObjectMember("frame")) {
        return make_error(req.id, "Motion.scrubTo: 'frame' required");
    }
    constexpr std::int64_t kMaximumJsonSafeInteger = 9007199254740991LL;
    const auto parsed = schema_integer(
        params["frame"], 0, kMaximumJsonSafeInteger);
    if (!parsed)
        return make_error(
            req.id,
            "Motion.scrubTo: 'frame' must be a nonnegative JSON-safe integer",
            "invalid_params");
    if (!loaded()) {
        return make_error(req.id, "Motion.scrubTo: no fixture loaded");
    }
    const auto frame = static_cast<std::uint64_t>(*parsed);
    const std::size_t emitted = scrub_to(frame);

    auto out = choc::value::createObject("");
    out.addMember("playhead_frame",
                  choc::value::createInt64(static_cast<int64_t>(playhead_frame())));
    out.addMember("emitted_count",
                  choc::value::createInt64(static_cast<int64_t>(emitted)));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionScrubber::handle_play(const InspectorMessage& req) {
    if (!loaded()) {
        return make_error(req.id, "Motion.play: no fixture loaded");
    }
    const std::size_t emitted = play();
    auto out = choc::value::createObject("");
    out.addMember("playing", choc::value::createBool(true));
    out.addMember("emitted_count",
                  choc::value::createInt64(static_cast<int64_t>(emitted)));
    out.addMember("playhead_frame",
                  choc::value::createInt64(static_cast<int64_t>(playhead_frame())));
    return make_response(req.id, choc::json::toString(out, false));
}

InspectorMessage MotionScrubber::handle_pause(const InspectorMessage& req) {
    pause();
    auto out = choc::value::createObject("");
    out.addMember("playing", choc::value::createBool(false));
    out.addMember("playhead_frame",
                  choc::value::createInt64(static_cast<int64_t>(playhead_frame())));
    return make_response(req.id, choc::json::toString(out, false));
}

}  // namespace pulp::inspect

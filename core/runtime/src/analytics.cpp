#include <pulp/runtime/analytics.hpp>
#include <fstream>
#include <chrono>

namespace pulp::runtime {

namespace {

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    constexpr char kHex[] = "0123456789abcdef";
    for (char c : value) {
        const auto uc = static_cast<unsigned char>(c);
        if (c == '\\') {
            escaped += "\\\\";
        } else if (c == '"') {
            escaped += "\\\"";
        } else if (c == '\n') {
            escaped += "\\n";
        } else if (c == '\r') {
            escaped += "\\r";
        } else if (c == '\t') {
            escaped += "\\t";
        } else if (c == '\b') {
            escaped += "\\b";
        } else if (c == '\f') {
            escaped += "\\f";
        } else if (uc < 0x20) {
            escaped += "\\u00";
            escaped += kHex[(uc >> 4) & 0x0f];
            escaped += kHex[uc & 0x0f];
        } else {
            escaped += c;
        }
    }
    return escaped;
}

}  // namespace

// ── FileAnalyticsDestination ────────────────────────────────────────────

FileAnalyticsDestination::FileAnalyticsDestination(std::string_view path)
    : path_(path) {}

void FileAnalyticsDestination::log_event(const AnalyticsEvent& event) {
    bool should_flush = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.push_back(event);
        should_flush = buffer_.size() >= 100;
    }
    // Flush outside the lock to avoid deadlock
    if (should_flush) flush();
}

void FileAnalyticsDestination::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_.empty()) return;

    std::ofstream file(path_, std::ios::app);
    if (!file) return;

    for (auto& event : buffer_) {
        file << "{\"event\":\"" << json_escape(event.name) << "\",\"time\":" << event.timestamp;
        if (!event.properties.empty()) {
            file << ",\"props\":{";
            bool first = true;
            for (auto& [k, v] : event.properties) {
                if (!first) file << ",";
                file << "\"" << json_escape(k) << "\":\"" << json_escape(v) << "\"";
                first = false;
            }
            file << "}";
        }
        file << "}\n";
    }

    buffer_.clear();
}

// ── Analytics ───────────────────────────────────────────────────────────

Analytics& Analytics::instance() {
    static Analytics analytics;
    return analytics;
}

void Analytics::add_destination(std::unique_ptr<AnalyticsDestination> dest) {
    if (!dest) return;

    std::lock_guard<std::mutex> lock(mutex_);
    destinations_.push_back(std::move(dest));
}

void Analytics::log_event(std::string_view name,
                          const std::map<std::string, std::string>& properties) {
    if (!enabled_) return;

    AnalyticsEvent event;
    event.name = std::string(name);
    event.properties = properties;

    auto now = std::chrono::system_clock::now();
    event.timestamp = std::chrono::duration<double>(now.time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(mutex_);
    event_count_++;
    for (auto& dest : destinations_)
        dest->log_event(event);
}

void Analytics::log(std::string_view name) {
    log_event(name);
}

void Analytics::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& dest : destinations_)
        dest->flush();
}

// ── WidgetTracker ───────────────────────────────────────────────────────

void WidgetTracker::track_click(std::string_view widget_id) {
    Analytics::instance().log_event("widget_click", {{"widget", std::string(widget_id)}});
}

void WidgetTracker::track_value_change(std::string_view widget_id, std::string_view value) {
    Analytics::instance().log_event("value_change", {
        {"widget", std::string(widget_id)},
        {"value", std::string(value)}
    });
}

void WidgetTracker::track_preset_select(std::string_view preset_name) {
    Analytics::instance().log_event("preset_select", {{"preset", std::string(preset_name)}});
}

}  // namespace pulp::runtime

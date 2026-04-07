#include <pulp/runtime/analytics.hpp>
#include <fstream>
#include <chrono>

namespace pulp::runtime {

// ── FileAnalyticsDestination ────────────────────────────────────────────

FileAnalyticsDestination::FileAnalyticsDestination(std::string_view path)
    : path_(path) {}

void FileAnalyticsDestination::log_event(const AnalyticsEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.push_back(event);

    // Auto-flush every 100 events
    if (buffer_.size() >= 100)
        flush();
}

void FileAnalyticsDestination::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_.empty()) return;

    std::ofstream file(path_, std::ios::app);
    if (!file) return;

    for (auto& event : buffer_) {
        file << "{\"event\":\"" << event.name << "\",\"time\":" << event.timestamp;
        if (!event.properties.empty()) {
            file << ",\"props\":{";
            bool first = true;
            for (auto& [k, v] : event.properties) {
                if (!first) file << ",";
                file << "\"" << k << "\":\"" << v << "\"";
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

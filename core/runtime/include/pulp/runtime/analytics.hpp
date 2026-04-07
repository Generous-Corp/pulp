#pragma once

// Analytics — thread-safe event tracking with pluggable destinations.

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <chrono>
#include <cstdint>

namespace pulp::runtime {

/// Single analytics event
struct AnalyticsEvent {
    std::string name;
    std::map<std::string, std::string> properties;
    double timestamp = 0;  // Unix epoch seconds
};

/// Abstract destination for analytics events
class AnalyticsDestination {
public:
    virtual ~AnalyticsDestination() = default;
    virtual void log_event(const AnalyticsEvent& event) = 0;
    virtual void flush() {}
};

/// File-based analytics destination (writes JSON lines)
class FileAnalyticsDestination : public AnalyticsDestination {
public:
    explicit FileAnalyticsDestination(std::string_view path);
    void log_event(const AnalyticsEvent& event) override;
    void flush() override;
private:
    std::string path_;
    std::vector<AnalyticsEvent> buffer_;
    std::mutex mutex_;
};

/// Analytics singleton — thread-safe event logging
class Analytics {
public:
    static Analytics& instance();

    /// Add a destination
    void add_destination(std::unique_ptr<AnalyticsDestination> dest);

    /// Log an event with properties
    void log_event(std::string_view name,
                   const std::map<std::string, std::string>& properties = {});

    /// Log a simple event (name only)
    void log(std::string_view name);

    /// Flush all destinations
    void flush();

    /// Enable/disable analytics
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool is_enabled() const { return enabled_; }

    /// Number of events logged since creation
    uint64_t event_count() const { return event_count_; }

private:
    Analytics() = default;
    std::vector<std::unique_ptr<AnalyticsDestination>> destinations_;
    std::mutex mutex_;
    bool enabled_ = true;
    uint64_t event_count_ = 0;
};

/// Widget interaction tracker — automatically logs UI events
class WidgetTracker {
public:
    /// Track a button click
    static void track_click(std::string_view widget_id);

    /// Track a value change
    static void track_value_change(std::string_view widget_id, std::string_view value);

    /// Track a preset selection
    static void track_preset_select(std::string_view preset_name);
};

}  // namespace pulp::runtime

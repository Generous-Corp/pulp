// console_capture.cpp — JS console log interception

#include <pulp/inspect/console_capture.hpp>

namespace pulp::inspect {

ConsoleCapture::LogCallback ConsoleCapture::callback(LogCallback previous) {
    return [this, prev = std::move(previous)](std::string_view level, std::string_view message) {
        // Forward to previous callback first (preserve existing behavior)
        if (prev) prev(level, message);

        // Capture the entry
        Entry entry;
        entry.level = std::string(level);
        entry.message = std::string(message);
        entry.time = std::chrono::steady_clock::now();

        std::lock_guard lock(mutex_);
        entries_.push_back(std::move(entry));
        if (entries_.size() > kMaxEntries)
            entries_.erase(entries_.begin());
    };
}

std::vector<ConsoleCapture::Entry> ConsoleCapture::entries() const {
    std::lock_guard lock(mutex_);
    return entries_;
}

void ConsoleCapture::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

} // namespace pulp::inspect

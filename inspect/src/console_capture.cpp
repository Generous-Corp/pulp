// console_capture.cpp — JS console log interception

#include <pulp/inspect/console_capture.hpp>

namespace pulp::inspect {

ConsoleCapture::LogCallback ConsoleCapture::callback(LogCallback previous) {
    return [this, prev = std::move(previous)](std::string_view level, std::string_view message) {
        // Forward to previous callback first (preserve existing behavior)
        if (prev) prev(level, message);

        Entry entry;
        entry.level = std::string(level);
        entry.message = std::string(message);
        entry.time = std::chrono::steady_clock::now();

        EntrySink sink;
        Entry pushed;
        {
            std::lock_guard lock(mutex_);
            entry.seq = ++next_seq_;
            entries_.push_back(entry);
            if (entries_.size() > kMaxEntries)
                entries_.erase(entries_.begin());
            sink = sink_;      // copy under lock; fire outside
            pushed = entry;
        }
        // Fire the live-push sink outside the lock so a broadcasting host can't
        // deadlock against a concurrent entries()/entries_since() reader.
        if (sink) sink(pushed);
    };
}

std::vector<ConsoleCapture::Entry> ConsoleCapture::entries() const {
    std::lock_guard lock(mutex_);
    return entries_;
}

std::vector<ConsoleCapture::Entry>
ConsoleCapture::entries_since(uint64_t after_seq, uint64_t& next_seq) const {
    std::lock_guard lock(mutex_);
    next_seq = next_seq_;
    std::vector<Entry> out;
    for (const auto& e : entries_)
        if (e.seq > after_seq)
            out.push_back(e);
    return out;
}

uint64_t ConsoleCapture::latest_seq() const {
    std::lock_guard lock(mutex_);
    return next_seq_;
}

void ConsoleCapture::set_entry_sink(EntrySink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

void ConsoleCapture::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

} // namespace pulp::inspect

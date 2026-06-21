// font_flight_recorder.cpp

#include "pulp/canvas/font_flight_recorder.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <sstream>

namespace pulp::canvas {

struct FontFlightRecorder::Impl {
    mutable std::mutex mtx;
    std::deque<FallbackTraceRecord> buffer;
    std::size_t cap = 1024;
    std::atomic<std::uint64_t> seq{0};
};

FontFlightRecorder::FontFlightRecorder()
    : impl_(std::make_unique<Impl>()) {}
FontFlightRecorder::~FontFlightRecorder() = default;

FontFlightRecorder& FontFlightRecorder::instance() {
    static FontFlightRecorder inst;
    return inst;
}

void FontFlightRecorder::record_fallback(const FallbackTraceRecord& record_in) {
    // Sequence stamp lives outside the lock so two concurrent
    // resolves get strictly monotonic sequence numbers regardless
    // of which one wins the buffer mutex.
    FallbackTraceRecord rec = record_in;
    rec.sequence = impl_->seq.fetch_add(1, std::memory_order_acq_rel);

    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->cap == 0) return;
    if (impl_->buffer.size() >= impl_->cap) impl_->buffer.pop_front();
    impl_->buffer.push_back(std::move(rec));
}

std::vector<FallbackTraceRecord> FontFlightRecorder::drain() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<FallbackTraceRecord> out(impl_->buffer.begin(),
                                          impl_->buffer.end());
    impl_->buffer.clear();
    return out;
}

std::vector<FallbackTraceRecord> FontFlightRecorder::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return {impl_->buffer.begin(), impl_->buffer.end()};
}

void FontFlightRecorder::clear() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->buffer.clear();
}

void FontFlightRecorder::set_capacity(std::size_t capacity) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->cap = capacity;
    while (impl_->buffer.size() > impl_->cap) impl_->buffer.pop_front();
}

std::size_t FontFlightRecorder::capacity() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->cap;
}

std::size_t FontFlightRecorder::size() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->buffer.size();
}

namespace {
void json_escape(std::ostringstream& out, const std::string& s) {
    out << '"';
    for (char c : s) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out << buf;
                } else {
                    out << c;
                }
        }
    }
    out << '"';
}
} // namespace

std::string flight_recorder_drain_json() {
    auto records = FontFlightRecorder::instance().drain();
    std::ostringstream out;
    out << "{\"records\":[";
    bool first = true;
    for (const auto& r : records) {
        if (!first) out << ',';
        first = false;
        out << "{\"requested_family\":";
        json_escape(out, r.requested_family);
        out << ",\"selected_family\":";
        json_escape(out, r.selected_family);
        out << ",\"origin\":" << static_cast<int>(r.origin)
            << ",\"generation\":" << r.generation
            << ",\"sequence\":" << r.sequence << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace pulp::canvas

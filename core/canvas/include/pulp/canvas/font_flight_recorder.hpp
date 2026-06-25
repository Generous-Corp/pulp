// font_flight_recorder.hpp
//
// FontFlightRecorder is the process-global sink for FallbackTraceRecord
// events produced by FontResolver every time a typeface resolves. It's a
// bounded ring buffer; callers (the --font-trace CLI flag, the
// import-missing-font-advisor, a developer's debug overlay) can drain it to
// see exactly which font cascade step produced each glyph.
//
// The recorder runs always-on with a reasonable default capacity
// (1024 records) — there's no measurable overhead at typical UI
// rates and the diagnostic value of "what fonts did the last
// frame use" is high. Callers that need a different cap can call
// set_capacity(); 0 disables recording entirely.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulp::canvas {

/// One resolution event. Mirrors `FallbackTraceStep` in
/// `font_resolver.hpp` but is self-contained so the recorder
/// header doesn't need to drag the resolver header in.
struct FallbackTraceRecord {
    std::string  requested_family;
    std::string  selected_family;
    std::uint8_t origin;        ///< matches FallbackOrigin enum cast to u8
    std::uint64_t generation;   ///< registry generation at resolve time
    std::uint64_t sequence;     ///< monotonic per-recorder sequence number
};

class FontFlightRecorder {
public:
    static FontFlightRecorder& instance();

    /// Append a resolution event. No-op when `capacity() == 0`. When
    /// the buffer is full, the oldest record is dropped to make room.
    void record_fallback(const FallbackTraceRecord& record);

    /// Snapshot + clear the buffered records. Returns oldest-first.
    std::vector<FallbackTraceRecord> drain();

    /// Snapshot only (does not clear). Returns oldest-first.
    std::vector<FallbackTraceRecord> snapshot() const;

    /// Drop all buffered records.
    void clear();

    /// Set the ring-buffer capacity. `0` disables recording.
    /// Default 1024. Shrinking drops the oldest entries.
    void set_capacity(std::size_t capacity);
    std::size_t capacity() const noexcept;

    /// Number of records currently buffered.
    std::size_t size() const noexcept;

private:
    FontFlightRecorder();
    ~FontFlightRecorder();
    FontFlightRecorder(const FontFlightRecorder&)            = delete;
    FontFlightRecorder& operator=(const FontFlightRecorder&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Convenience: drain the global recorder and emit one JSON object per
/// record to `out`. Intended drain path for a future
/// `pulp-ui-preview --font-trace --format=json` flag and the
/// import-missing-font-advisor.
std::string flight_recorder_drain_json();

} // namespace pulp::canvas

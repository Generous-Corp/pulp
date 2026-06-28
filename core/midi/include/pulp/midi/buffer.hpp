#pragma once

#include <pulp/midi/message.hpp>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <initializer_list>
#include <limits>

namespace pulp::midi {

/// Collection of timestamped MIDI events within a single audio buffer period.
///
/// Events are appended via add() and should be sorted by sample_offset
/// (call sort()) before iterating in the audio callback. Supports
/// range-based for loops.
///
/// Realtime contract: the buffer is safe for audio-thread appends only after
/// the owner has called reserve() with the worst-case block capacities and
/// set_realtime_capacity_limit(true). In that mode add() and SysEx append
/// helpers drop and count overflow instead of growing storage. Without that
/// preparation, vector growth remains possible and callers must treat mutation
/// as control/offline-thread work.
///
/// @code
/// MidiBuffer buf;
/// buf.add(MidiEvent::note_on(0, 60, 100));
/// buf.sort();
/// for (const auto& ev : buf) { /* process */ }
/// @endcode
class MidiBuffer {
public:
    MidiBuffer() = default;
    MidiBuffer(const MidiBuffer& other)
        : ump_(other.ump_),
          sysex_copy_payload_capacity_(other.sysex_copy_payload_capacity_),
          limit_to_reserved_capacity_(other.limit_to_reserved_capacity_),
          dropped_events_(other.dropped_events_),
          dropped_sysex_events_(other.dropped_sysex_events_) {
        copy_event_storage(other);
        reserve_copied_sysex_payloads(other);
    }
    MidiBuffer& operator=(const MidiBuffer& other) {
        if (this != &other) {
            sysex_.clear();
            ump_ = other.ump_;
            sysex_copy_payload_capacity_ = other.sysex_copy_payload_capacity_;
            limit_to_reserved_capacity_ = other.limit_to_reserved_capacity_;
            dropped_events_ = other.dropped_events_;
            dropped_sysex_events_ = other.dropped_sysex_events_;
            copy_event_storage(other);
            reserve_copied_sysex_payloads(other);
        }
        return *this;
    }
    MidiBuffer(MidiBuffer&& other) noexcept
        : events_(std::move(other.events_)),
          sort_index_(std::move(other.sort_index_)),
          sort_reorder_(std::move(other.sort_reorder_)),
          sysex_copy_payload_pool_(std::move(other.sysex_copy_payload_pool_)),
          sysex_(std::move(other.sysex_)),
          ump_(other.ump_),
          sysex_copy_payload_capacity_(other.sysex_copy_payload_capacity_),
          limit_to_reserved_capacity_(other.limit_to_reserved_capacity_),
          dropped_events_(other.dropped_events_),
          dropped_sysex_events_(other.dropped_sysex_events_) {
        rebind_sysex_payloads_to_pool();
        other.ump_ = nullptr;
        other.sysex_copy_payload_capacity_ = 0;
        other.limit_to_reserved_capacity_ = false;
        other.dropped_events_ = 0;
        other.dropped_sysex_events_ = 0;
    }
    MidiBuffer& operator=(MidiBuffer&& other) noexcept {
        if (this != &other) {
            sysex_.clear();
            events_ = std::move(other.events_);
            sort_index_ = std::move(other.sort_index_);
            sort_reorder_ = std::move(other.sort_reorder_);
            sysex_copy_payload_pool_ = std::move(other.sysex_copy_payload_pool_);
            sysex_ = std::move(other.sysex_);
            ump_ = other.ump_;
            sysex_copy_payload_capacity_ = other.sysex_copy_payload_capacity_;
            limit_to_reserved_capacity_ = other.limit_to_reserved_capacity_;
            dropped_events_ = other.dropped_events_;
            dropped_sysex_events_ = other.dropped_sysex_events_;
            rebind_sysex_payloads_to_pool();
            other.ump_ = nullptr;
            other.sysex_copy_payload_capacity_ = 0;
            other.limit_to_reserved_capacity_ = false;
            other.dropped_events_ = 0;
            other.dropped_sysex_events_ = 0;
        }
        return *this;
    }

    /// Append a MIDI event to the buffer.
    bool add(const MidiEvent& event) {
        if (!can_append_event()) {
            record_event_drop();
            return false;
        }
        events_.push_back(event);
        return true;
    }

    /// @copydoc add(const MidiEvent&)
    bool add(MidiEvent&& event) {
        if (!can_append_event()) {
            record_event_drop();
            return false;
        }
        events_.push_back(std::move(event));
        return true;
    }

    /// Remove all events.
    void clear() {
        events_.clear();
        dropped_events_ = 0;
    }
    bool empty() const { return events_.empty(); }
    std::size_t size() const { return events_.size(); }
    void reserve_events(std::size_t capacity) {
        events_.reserve(capacity);
        reserve_sort_scratch(capacity);
    }
    void reserve_sysex(std::size_t capacity) { sysex_.reserve(capacity); }

    /// Preallocate storage for realtime callers that append during process().
    void reserve(std::size_t event_capacity,
                 std::size_t sysex_capacity = 0,
                 std::size_t sysex_copy_payload_capacity = 0) {
        events_.reserve(event_capacity);
        reserve_sort_scratch(event_capacity);
        sysex_.reserve(sysex_capacity);
        if (sysex_copy_payload_capacity > 0) {
            reserve_sysex_copy_payloads(sysex_capacity,
                                        sysex_copy_payload_capacity);
        }
    }

    /// When enabled, add() and move-based add_sysex() drop once reserved
    /// capacity is full instead of growing vectors. add_sysex_copy() uses
    /// reserve_sysex_copy_payloads() storage in this mode and drops when no
    /// reserved payload slot can hold the event. Intended for adapter-owned
    /// buffers.
    void set_realtime_capacity_limit(bool enabled = true) {
        limit_to_reserved_capacity_ = enabled;
    }
    bool realtime_capacity_limited() const { return limit_to_reserved_capacity_; }
    std::size_t event_capacity() const { return events_.capacity(); }
    std::size_t sysex_capacity() const { return sysex_.capacity(); }
    std::size_t sysex_copy_payload_capacity() const {
        return sysex_copy_payload_capacity_;
    }
    std::uint32_t dropped_event_count() const { return dropped_events_; }
    std::uint32_t dropped_sysex_count() const { return dropped_sysex_events_; }

    /// Sort events by sample_offset for sample-accurate processing.
    /// Call this before iterating in the audio callback. Realtime callers
    /// must not append while sorting and should rely on adapters to have
    /// bounded the event count before process().
    ///
    /// The sort is **insertion-stable**: events at the same sample offset keep
    /// the relative order in which they were add()'ed (a controller appended
    /// before a note-on at the same offset stays before it). This preserves
    /// the same-offset musical semantics every consumer had, while remaining
    /// deterministic run-to-run. Implemented as an index sort with a
    /// pre-reserved scratch (an std::sort over indices keyed by
    /// (sample_offset, original_index) — a valid strict-weak total order —
    /// then a one-pass reorder through the scratch), NOT std::stable_sort
    /// (which may allocate a temporary buffer). The comparator never touches
    /// variable-length sysex payload bytes. Allocation-free when the scratch
    /// was reserved (reserve()/reserve_events() size it to the event
    /// capacity); only an unreserved, growing buffer can allocate here.
    void sort() {
        const std::size_t n = events_.size();
        if (n < 2) return;
        sort_index_.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            sort_index_[i] = static_cast<std::uint32_t>(i);
        }
        const std::vector<MidiEvent>& ev = events_;
        std::sort(sort_index_.begin(), sort_index_.end(),
            [&ev](std::uint32_t a, std::uint32_t b) {
                if (ev[a].sample_offset != ev[b].sample_offset) {
                    return ev[a].sample_offset < ev[b].sample_offset;
                }
                return a < b;  // tie-break = original insertion order
            });
        sort_reorder_.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            sort_reorder_[i] = std::move(events_[sort_index_[i]]);
        }
        events_.swap(sort_reorder_);
    }

    auto begin() { return events_.begin(); }
    auto end() { return events_.end(); }
    auto begin() const { return events_.begin(); }
    auto end() const { return events_.end(); }

    const MidiEvent& operator[](std::size_t index) const { return events_[index]; }

    /// Attach a UmpBuffer sidecar carrying MIDI 2.0 packets that can't be
    /// represented as choc::midi::ShortMessage (UMP type-4 channel voice,
    /// type-3/5 data, type-0 utility, etc.). Format adapters set this
    /// before process(); plugins opting in to
    /// PluginDescriptor::supports_ump read it via ump(). Ownership stays
    /// with the caller; the buffer must outlive the process() block.
    void attach_ump(class UmpBuffer* ump) { ump_ = ump; }

    /// Attached UmpBuffer or nullptr. A null return means "no UMP events
    /// this block", not "UMP unsupported" — that's declared at descriptor
    /// time via PluginDescriptor::supports_ump.
    const class UmpBuffer* ump() const { return ump_; }
    class UmpBuffer* ump() { return ump_; }

    // ── SysEx sidecar ────────────────────────────────────────────────────
    //
    // choc::midi::ShortMessage is fixed 3 bytes; system-exclusive payloads
    // can run to kilobytes. Sysex therefore travels in a parallel vector
    // whose entries are referenced by sample_offset the same way MidiEvent
    // entries are. Format adapters that carry sysex (CoreMIDI, VST3 event
    // list with kData type, CLAP CLAP_EVENT_MIDI_SYSEX) populate this
    // alongside the short-message stream; plugins that don't care can
    // ignore it.
    class SysexPayload {
    public:
        SysexPayload() = default;
        SysexPayload(std::initializer_list<uint8_t> values) : bytes_(values) {}
        SysexPayload(std::vector<uint8_t> bytes)
            : bytes_(std::move(bytes)) {}

        SysexPayload(const SysexPayload& other)
            : bytes_(other.bytes_),
              realtime_pool_backed_(other.realtime_pool_backed_) {}
        SysexPayload& operator=(const SysexPayload& other) {
            if (this != &other) {
                recycle_now();
                bytes_ = other.bytes_;
                realtime_pool_backed_ = other.realtime_pool_backed_;
                recycle_pool_ = nullptr;
                recycle_capacity_ = 0;
            }
            return *this;
        }

        ~SysexPayload() { recycle_now(); }

        SysexPayload(SysexPayload&&) = delete;
        SysexPayload& operator=(SysexPayload&&) = delete;

        SysexPayload& operator=(std::initializer_list<uint8_t> values) {
            mutable_bytes().assign(values.begin(), values.end());
            return *this;
        }
        SysexPayload& operator=(std::vector<uint8_t> bytes) {
            recycle_now();
            bytes_ = std::move(bytes);
            realtime_pool_backed_ = false;
            return *this;
        }

        std::size_t size() const { return view().size(); }
        bool empty() const { return view().empty(); }
        std::size_t capacity() const { return view().capacity(); }
        const uint8_t* data() const { return view().data(); }
        uint8_t* data() { return mutable_bytes().data(); }
        const uint8_t& front() const { return view().front(); }
        uint8_t& front() { return mutable_bytes().front(); }
        const uint8_t& back() const { return view().back(); }
        uint8_t& back() { return mutable_bytes().back(); }
        const uint8_t& operator[](std::size_t index) const {
            return view()[index];
        }
        uint8_t& operator[](std::size_t index) {
            return mutable_bytes()[index];
        }
        auto begin() const { return view().begin(); }
        auto end() const { return view().end(); }
        auto begin() { return mutable_bytes().begin(); }
        auto end() { return mutable_bytes().end(); }
        void reserve(std::size_t capacity) { mutable_bytes().reserve(capacity); }
        void resize(std::size_t size) { mutable_bytes().resize(size); }
        void clear() { mutable_bytes().clear(); }
        void push_back(uint8_t byte) { mutable_bytes().push_back(byte); }

        std::vector<uint8_t> to_vector() const { return view(); }

        friend bool operator==(const SysexPayload& lhs,
                               const std::vector<uint8_t>& rhs) {
            return lhs.view() == rhs;
        }
        friend bool operator==(const std::vector<uint8_t>& lhs,
                               const SysexPayload& rhs) {
            return lhs == rhs.view();
        }
        friend bool operator!=(const SysexPayload& lhs,
                               const std::vector<uint8_t>& rhs) {
            return !(lhs == rhs);
        }
        friend bool operator!=(const std::vector<uint8_t>& lhs,
                               const SysexPayload& rhs) {
            return !(lhs == rhs);
        }

    private:
        friend class MidiBuffer;

        const std::vector<uint8_t>& view() const { return bytes_; }
        std::vector<uint8_t>& mutable_bytes() { return bytes_; }
        std::vector<uint8_t> release_owned_vector() {
            recycle_pool_ = nullptr;
            realtime_pool_backed_ = false;
            return std::move(bytes_);
        }
        bool realtime_pool_backed() const { return realtime_pool_backed_; }
        void set_realtime_pool_backed(
            bool backed,
            std::vector<std::vector<uint8_t>>* recycle_pool = nullptr,
            std::size_t recycle_capacity = 0) {
            realtime_pool_backed_ = backed;
            recycle_pool_ = backed ? recycle_pool : nullptr;
            recycle_capacity_ = backed ? recycle_capacity : 0;
        }
        void recycle_now() {
            if (recycle_pool_ != nullptr && recycle_capacity_ > 0
                && bytes_.capacity() >= recycle_capacity_
                && recycle_pool_->size() < recycle_pool_->capacity()) {
                bytes_.clear();
                recycle_pool_->push_back(std::move(bytes_));
            }
            recycle_pool_ = nullptr;
            recycle_capacity_ = 0;
            realtime_pool_backed_ = false;
        }

        std::vector<uint8_t> bytes_;
        std::vector<std::vector<uint8_t>>* recycle_pool_ = nullptr;
        std::size_t recycle_capacity_ = 0;
        bool realtime_pool_backed_ = false;
    };

    struct SysexEvent {
        SysexPayload data;           ///< full F0 .. F7 payload
        int32_t sample_offset = 0;   ///< sample position within the block
        double  timestamp = 0.0;     ///< absolute time in seconds
    };

    bool add_sysex(std::vector<uint8_t> data, int32_t sample_offset = 0, double ts = 0.0) {
        if (!can_append_sysex()) {
            record_sysex_drop();
            return false;
        }
        sysex_.emplace_back();
        auto& event = sysex_.back();
        event.data = std::move(data);
        event.sample_offset = sample_offset;
        event.timestamp = ts;
        return true;
    }
    bool add_sysex(SysexEvent&& event) {
        if (event.data.realtime_pool_backed()) {
            return add_sysex_copy(event.data.data(),
                                  event.data.size(),
                                  event.sample_offset,
                                  event.timestamp);
        }
        if (!can_append_sysex()) {
            record_sysex_drop();
            return false;
        }
        sysex_.emplace_back();
        auto& dst = sysex_.back();
        dst.data = event.data.release_owned_vector();
        dst.sample_offset = event.sample_offset;
        dst.timestamp = event.timestamp;
        return true;
    }
    bool add_sysex_copy(const uint8_t* data,
                        std::size_t size,
                        int32_t sample_offset = 0,
                        double ts = 0.0) {
        if (limit_to_reserved_capacity_) {
            return add_sysex_copy_realtime(data, size, sample_offset, ts);
        }
        if (!can_append_sysex()) {
            record_sysex_drop();
            return false;
        }
        return add_sysex(std::vector<uint8_t>(data, data + size),
                         sample_offset,
                         ts);
    }
    void reserve_sysex_copy_payloads(std::size_t payload_count,
                                     std::size_t payload_capacity) {
        sysex_copy_payload_capacity_ = payload_capacity;
        sysex_copy_payload_pool_.clear();
        sysex_copy_payload_pool_.reserve(payload_count);
        for (std::size_t i = 0; i < payload_count; ++i) {
            std::vector<uint8_t> payload;
            payload.reserve(payload_capacity);
            sysex_copy_payload_pool_.push_back(std::move(payload));
        }
    }
    void clear_sysex() {
        for (auto& event : sysex_) {
            recycle_sysex_payload(event.data.release_owned_vector());
        }
        sysex_.clear();
        dropped_sysex_events_ = 0;
    }
    std::size_t sysex_size() const { return sysex_.size(); }
    const std::vector<SysexEvent>& sysex() const { return sysex_; }
    std::vector<SysexEvent>& sysex() { return sysex_; }

private:
    bool can_append_event() const {
        return !limit_to_reserved_capacity_ || events_.size() < events_.capacity();
    }
    bool can_append_sysex() const {
        return !limit_to_reserved_capacity_ || sysex_.size() < sysex_.capacity();
    }
    bool add_sysex_copy_realtime(const uint8_t* data,
                                 std::size_t size,
                                 int32_t sample_offset,
                                 double ts) {
        if (!can_append_sysex() || sysex_copy_payload_pool_.empty()
            || size > sysex_copy_payload_capacity_) {
            record_sysex_drop();
            return false;
        }
        auto payload = std::move(sysex_copy_payload_pool_.back());
        sysex_copy_payload_pool_.pop_back();
        payload.resize(size);
        std::copy(data, data + size, payload.begin());
        sysex_.emplace_back();
        auto& event = sysex_.back();
        event.data = std::move(payload);
        event.data.set_realtime_pool_backed(true,
                                            &sysex_copy_payload_pool_,
                                            sysex_copy_payload_capacity_);
        event.sample_offset = sample_offset;
        event.timestamp = ts;
        return true;
    }
    void recycle_sysex_payload(std::vector<uint8_t>&& payload) {
        if (sysex_copy_payload_capacity_ == 0
            || payload.capacity() < sysex_copy_payload_capacity_
            || sysex_copy_payload_pool_.size()
                   >= sysex_copy_payload_pool_.capacity()) {
            return;
        }
        payload.clear();
        sysex_copy_payload_pool_.push_back(std::move(payload));
    }
    void copy_event_storage(const MidiBuffer& other) {
        events_.clear();
        events_.reserve(other.events_.capacity());
        events_ = other.events_;
        sysex_.clear();
        sysex_.reserve(other.sysex_.capacity());
        sysex_ = other.sysex_;
    }
    void reserve_copied_sysex_payloads(const MidiBuffer& other) {
        sysex_copy_payload_pool_.clear();
        if (sysex_copy_payload_capacity_ == 0) {
            return;
        }
        for (auto& event : sysex_) {
            if (event.data.size() <= sysex_copy_payload_capacity_) {
                event.data.reserve(sysex_copy_payload_capacity_);
                event.data.set_realtime_pool_backed(
                    event.data.realtime_pool_backed(),
                    event.data.realtime_pool_backed()
                        ? &sysex_copy_payload_pool_
                        : nullptr,
                    event.data.realtime_pool_backed()
                        ? sysex_copy_payload_capacity_
                        : 0);
            }
        }
        sysex_copy_payload_pool_.reserve(
            other.sysex_copy_payload_pool_.capacity());
        for (std::size_t i = 0; i < other.sysex_copy_payload_pool_.size(); ++i) {
            std::vector<uint8_t> payload;
            payload.reserve(sysex_copy_payload_capacity_);
            sysex_copy_payload_pool_.push_back(std::move(payload));
        }
    }
    void rebind_sysex_payloads_to_pool() {
        for (auto& event : sysex_) {
            if (event.data.realtime_pool_backed()) {
                event.data.set_realtime_pool_backed(true,
                                                    &sysex_copy_payload_pool_,
                                                    sysex_copy_payload_capacity_);
            }
        }
    }
    static void saturating_increment(std::uint32_t& value) {
        if (value < std::numeric_limits<std::uint32_t>::max()) {
            ++value;
        }
    }
    void record_event_drop() { saturating_increment(dropped_events_); }
    void record_sysex_drop() { saturating_increment(dropped_sysex_events_); }

    // Size the insertion-stable sort()'s scratch (index list + reorder buffer)
    // to the event capacity so sort() is allocation-free for realtime callers.
    // events_.swap(sort_reorder_) means the two reorder buffers ping-pong, so
    // BOTH must hold the capacity for the swap to stay alloc-free across blocks.
    void reserve_sort_scratch(std::size_t capacity) {
        sort_index_.reserve(capacity);
        sort_reorder_.reserve(capacity);
    }

    std::vector<MidiEvent> events_;
    // Scratch for the insertion-stable sort(). Not part of the buffer's logical
    // contents — excluded from copy/move/clear; reserved by reserve()/
    // reserve_events() and resized in place by sort().
    std::vector<std::uint32_t> sort_index_;
    std::vector<MidiEvent> sort_reorder_;
    std::vector<std::vector<uint8_t>> sysex_copy_payload_pool_;
    std::vector<SysexEvent> sysex_;
    class UmpBuffer* ump_ = nullptr;
    std::size_t sysex_copy_payload_capacity_ = 0;
    bool limit_to_reserved_capacity_ = false;
    std::uint32_t dropped_events_ = 0;
    std::uint32_t dropped_sysex_events_ = 0;
};

} // namespace pulp::midi

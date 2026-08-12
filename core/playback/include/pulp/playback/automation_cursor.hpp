#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/playback/automation_program.hpp>
#include <pulp/playback/transport.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>

namespace pulp::playback {

enum class AutomationTransition : std::uint8_t {
    Seed,
    Immediate,
    LinearRamp,
};

struct AutomationBlockEvent {
    std::uint32_t sample_offset = 0;
    float value = 0.0f;
    AutomationTransition transition = AutomationTransition::Seed;
    constexpr bool operator==(const AutomationBlockEvent&) const = default;
};

enum class AutomationCursorCode : std::uint8_t {
    Ok,
    Coalesced,
    AdoptionRejected,
    InvalidTransport,
    TempoMapMismatch,
    InsufficientCapacity,
    WorkCapacityExceeded,
};

enum class AutomationProgramAdoption : std::uint8_t {
    Adopted,
    Unchanged,
    Rejected,
};

struct AutomationCursorResult {
    AutomationCursorCode code = AutomationCursorCode::Ok;
    AutomationProgramAdoption adoption = AutomationProgramAdoption::Unchanged;
    std::uint32_t emitted_events = 0;
    /// Distinct mandatory topology and continuous-refinement positions before
    /// applying the caller's output budget.
    std::uint32_t candidate_points = 0;
    /// Compiled segments intersecting the active transport ranges.
    std::uint32_t intersecting_segments = 0;
};

/// Random-access, immutable segment storage used by the production cursor.
/// The reader returns a segment by value, allowing both native program records
/// and differently packed wire records to use the same rendering algorithm.
class AutomationSegmentView {
  public:
    using Reader = AutomationProgramSegment (*)(const void*, std::size_t) noexcept;

    class Iterator {
      public:
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;
        using value_type = AutomationProgramSegment;
        using difference_type = std::ptrdiff_t;
        using reference = AutomationProgramSegment;

        reference operator*() const noexcept {
            return (*view_)[index_];
        }
        reference operator[](difference_type offset) const noexcept {
            return (
                *view_)[static_cast<std::size_t>(static_cast<difference_type>(index_) + offset)];
        }
        Iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        Iterator operator++(int) noexcept {
            auto copy = *this;
            ++*this;
            return copy;
        }
        Iterator& operator--() noexcept {
            --index_;
            return *this;
        }
        Iterator operator--(int) noexcept {
            auto copy = *this;
            --*this;
            return copy;
        }
        Iterator& operator+=(difference_type offset) noexcept {
            index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) + offset);
            return *this;
        }
        Iterator& operator-=(difference_type offset) noexcept {
            return *this += -offset;
        }
        friend Iterator operator+(Iterator iterator, difference_type offset) noexcept {
            return iterator += offset;
        }
        friend Iterator operator+(difference_type offset, Iterator iterator) noexcept {
            return iterator += offset;
        }
        friend Iterator operator-(Iterator iterator, difference_type offset) noexcept {
            return iterator -= offset;
        }
        friend difference_type operator-(const Iterator& lhs, const Iterator& rhs) noexcept {
            return static_cast<difference_type>(lhs.index_) -
                   static_cast<difference_type>(rhs.index_);
        }
        auto operator<=>(const Iterator&) const = default;

      private:
        friend class AutomationSegmentView;
        Iterator(const AutomationSegmentView* view, std::size_t index) noexcept
            : view_(view), index_(index) {}
        const AutomationSegmentView* view_ = nullptr;
        std::size_t index_ = 0;
    };

    constexpr AutomationSegmentView() noexcept = default;

    AutomationProgramSegment operator[](std::size_t index) const noexcept {
        return reader_(records_, index);
    }
    AutomationProgramSegment front() const noexcept {
        return (*this)[0];
    }
    AutomationProgramSegment back() const noexcept {
        return (*this)[count_ - 1u];
    }
    Iterator begin() const noexcept {
        return Iterator(this, 0);
    }
    Iterator end() const noexcept {
        return Iterator(this, count_);
    }
    std::size_t size() const noexcept {
        return count_;
    }
    bool empty() const noexcept {
        return count_ == 0;
    }
    AutomationSegmentView subspan(std::size_t offset, std::size_t count) const noexcept {
        return {static_cast<const std::byte*>(records_) + offset * record_stride_, count, reader_,
                record_stride_};
    }

    template <typename Record>
    static AutomationSegmentView from_records(std::span<const Record> records,
                                              Reader reader) noexcept {
        return {records.data(), records.size(), reader, sizeof(Record)};
    }

  private:
    constexpr AutomationSegmentView(const void* records, std::size_t count, Reader reader,
                                    std::size_t record_stride) noexcept
        : records_(records), count_(count), reader_(reader), record_stride_(record_stride) {}

    const void* records_ = nullptr;
    std::size_t count_ = 0;
    Reader reader_ = nullptr;
    std::size_t record_stride_ = 0;
};

/// The identity, exact tempo-map object, and immutable segments consumed by
/// AutomationCursor. It is deliberately non-owning; its owner must keep the
/// native program or pinned wire bytes alive for the complete process call.
class AutomationProgramView {
  public:
    AutomationProgramView() = default;
    AutomationProgramView(ProgramGeneration generation,
                          AutomationProgramInstanceToken instance_token, timeline::ItemId lane_id,
                          const timebase::CompiledTempoMap& tempo_map,
                          AutomationSegmentView segments, float leading_value) noexcept
        : generation_(generation), instance_token_(instance_token), lane_id_(lane_id),
          tempo_map_(&tempo_map), segments_(segments), leading_value_(leading_value) {}

    static AutomationProgramView from(const AutomationProgram& program) noexcept;

    ProgramGeneration generation() const noexcept {
        return generation_;
    }
    AutomationProgramInstanceToken instance_token() const noexcept {
        return instance_token_;
    }
    timeline::ItemId lane_id() const noexcept {
        return lane_id_;
    }
    const timebase::CompiledTempoMap& tempo_map() const noexcept {
        return *tempo_map_;
    }
    AutomationSegmentView segments() const noexcept {
        return segments_;
    }
    float leading_value() const noexcept {
        return leading_value_;
    }
    bool empty() const noexcept {
        return segments_.empty();
    }

  private:
    ProgramGeneration generation_ = 0;
    AutomationProgramInstanceToken instance_token_;
    timeline::ItemId lane_id_;
    const timebase::CompiledTempoMap* tempo_map_ = nullptr;
    AutomationSegmentView segments_;
    float leading_value_ = 0.0f;
};

/// Allocation-free renderer for one immutable AutomationProgram. The caller
/// owns the output budget and routes the resulting plain-domain control points.
/// A non-seed point describes the transition from the preceding emitted point
/// in the same transport range. `process()` may use the span as bounded scratch;
/// only its first `emitted_events` entries are defined when the call returns.
class AutomationCursor {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs;

    AutomationCursorResult process(const AutomationProgram& program,
                                   const TransportSnapshot& transport,
                                   std::span<AutomationBlockEvent> output,
                                   std::uint32_t max_intersecting_segments =
                                       std::numeric_limits<std::uint32_t>::max()) noexcept;
    AutomationCursorResult process(const AutomationProgramView& program,
                                   const TransportSnapshot& transport,
                                   std::span<AutomationBlockEvent> output,
                                   std::uint32_t max_intersecting_segments =
                                       std::numeric_limits<std::uint32_t>::max()) noexcept;
    void reset() noexcept;

    timeline::ItemId active_lane_id() const noexcept {
        return active_key_.item_id;
    }
    ProgramGeneration active_generation() const noexcept {
        return active_key_.generation;
    }

  private:
    RendererProgramKey active_key_;
    AutomationProgramInstanceToken active_instance_token_;
    bool has_block_index_ = false;
    std::uint64_t last_block_index_ = 0;
};

} // namespace pulp::playback

#pragma once

#include <pulp/timeline/clip.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace pulp::timeline_editor {

/// Notes intersecting one time-and-pitch query, plus structural work diagnostics.
struct TimePitchQueryResult {
    std::vector<timeline::NoteEvent> notes;
    std::size_t visited_candidates = 0;
};

/// Immutable index for visible-note and marquee queries.
///
/// Time ranges are half-open and pitch bounds are inclusive. Results retain the
/// model's canonical `(start, id)` order. The index augments its start-sorted
/// tree with each subtree's latest end and pitch census, so a long note crossing
/// the query's left edge remains reachable without scanning every earlier note.
class TimePitchIndex {
  public:
    explicit TimePitchIndex(const timeline::MidiContent& content)
        : TimePitchIndex(content.notes()) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return notes_.size();
    }

    /// Finds notes intersecting `[start, end)` within `[lowest_pitch, highest_pitch]`.
    [[nodiscard]] TimePitchQueryResult query(timebase::TickPosition start,
                                             timebase::TickPosition end, std::uint8_t lowest_pitch,
                                             std::uint8_t highest_pitch) const {
        TimePitchQueryResult result;
        if (notes_.empty() || start >= end || lowest_pitch > highest_pitch || lowest_pitch > 127)
            return result;
        highest_pitch = std::min<std::uint8_t>(highest_pitch, 127);
        const auto pitch_mask = mask_for_range(lowest_pitch, highest_pitch);
        query_node(1, 0, notes_.size(), start, end, pitch_mask, result);
        return result;
    }

  private:
    explicit TimePitchIndex(std::span<const timeline::NoteEvent> notes)
        : notes_(notes.begin(), notes.end()) {
        std::sort(notes_.begin(), notes_.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.start != rhs.start)
                return lhs.start < rhs.start;
            return lhs.id < rhs.id;
        });
        if (!notes_.empty()) {
            tree_.resize(notes_.size() * 4);
            build(1, 0, notes_.size());
        }
    }

    struct Node {
        std::int64_t max_end = std::numeric_limits<std::int64_t>::min();
        std::array<std::uint64_t, 2> pitches{};
    };

    static std::array<std::uint64_t, 2> mask_for_range(std::uint8_t lowest,
                                                       std::uint8_t highest) noexcept {
        std::array<std::uint64_t, 2> mask{};
        for (std::uint16_t pitch = lowest; pitch <= highest; ++pitch)
            mask[pitch / 64] |= std::uint64_t{1} << (pitch % 64);
        return mask;
    }

    static bool intersects(const std::array<std::uint64_t, 2>& lhs,
                           const std::array<std::uint64_t, 2>& rhs) noexcept {
        return (lhs[0] & rhs[0]) != 0 || (lhs[1] & rhs[1]) != 0;
    }

    void build(std::size_t node, std::size_t begin, std::size_t end) {
        if (end - begin == 1) {
            const auto& note = notes_[begin];
            tree_[node].max_end = (note.start + note.duration).value;
            tree_[node].pitches[note.pitch / 64] |= std::uint64_t{1} << (note.pitch % 64);
            return;
        }
        const auto middle = begin + (end - begin) / 2;
        build(node * 2, begin, middle);
        build(node * 2 + 1, middle, end);
        tree_[node].max_end = std::max(tree_[node * 2].max_end, tree_[node * 2 + 1].max_end);
        tree_[node].pitches = {
            tree_[node * 2].pitches[0] | tree_[node * 2 + 1].pitches[0],
            tree_[node * 2].pitches[1] | tree_[node * 2 + 1].pitches[1],
        };
    }

    void query_node(std::size_t node, std::size_t begin, std::size_t end,
                    timebase::TickPosition query_start, timebase::TickPosition query_end,
                    const std::array<std::uint64_t, 2>& pitch_mask,
                    TimePitchQueryResult& result) const {
        const auto& summary = tree_[node];
        if (summary.max_end <= query_start.value || notes_[begin].start >= query_end ||
            !intersects(summary.pitches, pitch_mask))
            return;
        if (end - begin == 1) {
            ++result.visited_candidates;
            const auto& note = notes_[begin];
            const auto note_end = note.start + note.duration;
            if (note.start < query_end && note_end > query_start &&
                (pitch_mask[note.pitch / 64] & (std::uint64_t{1} << (note.pitch % 64))) != 0)
                result.notes.push_back(note);
            return;
        }
        const auto middle = begin + (end - begin) / 2;
        query_node(node * 2, begin, middle, query_start, query_end, pitch_mask, result);
        query_node(node * 2 + 1, middle, end, query_start, query_end, pitch_mask, result);
    }

    std::vector<timeline::NoteEvent> notes_;
    std::vector<Node> tree_;
};

} // namespace pulp::timeline_editor

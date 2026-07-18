#pragma once

#include <pulp/timeline/document_session.hpp>

#include <cassert>

namespace timeline_test {

using namespace pulp::timeline;
using namespace pulp::timebase;

inline Clip make_note_clip(ItemId clip_id, ItemId note_id, std::int64_t start,
                           std::uint16_t velocity = 1000) {
    auto notes = NoteContent::create({{note_id, {0}, {kTicksPerQuarter / 4}, velocity, 60, 0}});
    assert(notes);
    auto clip = Clip::create(clip_id, {start}, {kTicksPerQuarter}, std::move(notes).value());
    assert(clip);
    return std::move(clip).value();
}

inline Project make_project() {
    auto track = Track::create({4}, "track", {make_note_clip({5}, {6}, 0)});
    assert(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    assert(sequence);
    auto project = Project::create({{1}, "project", 7, {3}, {}, {std::move(sequence).value()}});
    assert(project);
    return std::move(project).value();
}

inline const Clip& clip(const Project& project, ItemId id = {5}) {
    return *project.find_sequence({3})->find_track({4})->find_clip(id);
}

inline std::uint16_t velocity(const Project& project) {
    const auto& value = clip(project);
    return std::get<NoteContent>(value.content()).notes()[0].velocity;
}

inline Transaction transaction(WriterId writer, std::uint64_t transaction_sequence,
                               std::uint64_t first_command_sequence, DocumentRevision revision,
                               std::vector<Command> commands) {
    Transaction result;
    result.id = {writer, transaction_sequence};
    result.expected_revision = revision;
    for (auto& command : commands)
        result.commands.push_back({{writer, first_command_sequence++}, std::move(command)});
    return result;
}

inline bool same_project(const Project& lhs, const Project& rhs) {
    if (lhs.id() != rhs.id() || lhs.next_item_id() != rhs.next_item_id() ||
        lhs.sequences().size() != rhs.sequences().size())
        return false;
    for (std::size_t s = 0; s < lhs.sequences().size(); ++s) {
        const auto& left_sequence = lhs.sequences()[s];
        const auto& right_sequence = rhs.sequences()[s];
        if (left_sequence.id() != right_sequence.id() ||
            left_sequence.tracks().size() != right_sequence.tracks().size())
            return false;
        for (std::size_t t = 0; t < left_sequence.tracks().size(); ++t) {
            const auto left_clips = left_sequence.tracks()[t].clips();
            const auto right_clips = right_sequence.tracks()[t].clips();
            if (left_clips.size() != right_clips.size())
                return false;
            for (std::size_t c = 0; c < left_clips.size(); ++c)
                if (!equivalent(left_clips[c], right_clips[c]))
                    return false;
        }
    }
    return true;
}

inline Transaction session_transaction(WriterToken& writer, DocumentRevision revision,
                                       std::vector<Command> commands) {
    Transaction result;
    result.id = writer.allocate_transaction_id();
    result.expected_revision = revision;
    for (auto& command : commands)
        result.commands.push_back({writer.allocate_command_id(), std::move(command)});
    return result;
}

} // namespace timeline_test

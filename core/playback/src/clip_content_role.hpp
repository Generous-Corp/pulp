#pragma once

#include <pulp/timeline/model.hpp>

#include <cstdint>
#include <variant>

namespace pulp::playback::detail {

// A compiler-visible content role is explicit: adding a variant alternative
// cannot silently turn a renderable clip into an unexplained no-op.
enum class ClipContentRole : std::uint8_t {
    Silent,
    Audio,
    Notes,
    Sequence,
};

inline ClipContentRole clip_content_role(const timeline::Clip& clip) noexcept {
    return std::visit(
        timeline::ClipContentCases{
            [](const timeline::EmptyContent&) { return ClipContentRole::Silent; },
            [](const timeline::MediaRef&) { return ClipContentRole::Audio; },
            [](const timeline::MidiContent&) { return ClipContentRole::Notes; },
            [](const timeline::RegisteredContent&) { return ClipContentRole::Silent; },
            [](const timeline::OpaqueContent&) { return ClipContentRole::Silent; },
            [](const timeline::SequenceRef&) { return ClipContentRole::Sequence; },
        },
        clip.content());
}

} // namespace pulp::playback::detail

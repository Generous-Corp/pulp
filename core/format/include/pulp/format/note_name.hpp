#pragma once

/// @file note_name.hpp
/// Naming individual notes so a host can label them in its editors.
///
/// A drum instrument's C1 is "Kick", not "C1". A sampler's top octave may be
/// keyswitches selecting articulations rather than pitches. Hosts show these
/// names down the side of the piano roll and in drum editors when the plug-in
/// publishes them, and show bare pitches when it does not.

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::format {

/// One named note.
///
/// `port`, `channel`, and `key` follow the wildcard convention the note-name
/// host APIs use: -1 matches every port / channel / key, so a single entry can
/// name a key across all channels. `key` is a MIDI note number, 0..127.
struct NoteName {
    std::string name;
    std::int16_t port = -1;
    std::int16_t channel = -1;
    std::int16_t key = 0;
};

}  // namespace pulp::format

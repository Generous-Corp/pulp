#pragma once

/// @file musical_typing.hpp
/// Computer-keyboard "musical typing" — maps the QWERTY row to MIDI notes so a
/// plugin/standalone editor can be played from the keyboard without a MIDI
/// device. This is the reusable, platform-agnostic CORE: an editor feeds it raw
/// key up/down events (from whatever the host delivers) and it emits note on/off.
///
/// Layout matches the canonical Tracktion/JUCE MidiKeyboardComponent row
/// "awsedftgyhujkolp" (a piano keyboard laid over the home + top rows):
///   a=C  w=C# s=D  e=D# d=E  f=F  t=F# g=G  y=G# h=A  u=A# j=B
///   k=C(+12) o=C# l=D  p=D#
/// `z` / `x` shift the base octave down / up.
///
/// Design notes (aligned with a Codex review + JUCE/PlunderTube study):
///   - Rejects Cmd/Ctrl/Alt/Meta chords so host menu shortcuts (Cmd+S …) still
///     work — `handle_key` returns false for them and for unmapped keys, so the
///     caller falls through to the host.
///   - De-dups OS key auto-repeat (a held key only sounds once).
///   - Tracks which note each KEY produced, so an octave shift mid-hold still
///     releases the right note; `all_notes_off()` releases everything (call it on
///     focus loss / capture disable to avoid stuck notes).
///   - No platform / focus / threading concerns live here — the host delivers
///     keys, the callbacks hand notes to wherever (e.g. a lock-free UI->audio
///     note queue). UI-thread use.

#include <pulp/view/input_events.hpp>

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace pulp::view {

class MusicalTypingController {
public:
    /// MIDI note for the first key ('a'). Default 60 (middle C). For a slice
    /// sampler set this to the slice root so 'a' triggers slice 0, 'w' slice 1, …
    void set_base_note(int note) { base_note_ = note; }
    int base_note() const { return base_note_; }

    std::function<void(int note, float velocity)> on_note_on;
    std::function<void(int note)> on_note_off;
    float velocity = 0.8f;

    /// Feed a key event. Returns true iff it was a mapped note/octave key with no
    /// Cmd/Ctrl/Alt/Meta modifier (i.e. consumed). Everything else returns false
    /// so the host keeps its shortcuts and other keys.
    bool handle_key(const KeyEvent& e) {
        if (e.modifiers & (kModCmd | kModCtrl | kModAlt | kModMeta)) return false;

        if (e.key == KeyCode::z || e.key == KeyCode::x) {
            if (e.is_down && !e.is_repeat) octave_shift_ += (e.key == KeyCode::x) ? 1 : -1;
            return true;
        }
        const int semi = semitone_for_key(e.key);
        if (semi < 0) return false;

        const int kc = static_cast<int>(e.key);
        if (e.is_down) {
            if (held_.count(kc)) return true;  // auto-repeat — already sounding
            const int note = std::clamp(base_note_ + octave_shift_ * 12 + semi, 0, 127);
            held_[kc] = note;
            if (on_note_on) on_note_on(note, velocity);
        } else {
            auto it = held_.find(kc);
            if (it != held_.end()) {
                if (on_note_off) on_note_off(it->second);
                held_.erase(it);
            }
        }
        return true;
    }

    /// Release every held note (focus loss / capture disabled). Safe to call any
    /// time; a no-op when nothing is held.
    void all_notes_off() {
        for (const auto& [kc, note] : held_)
            if (on_note_off) on_note_off(note);
        held_.clear();
    }

    bool any_held() const { return !held_.empty(); }

    /// QWERTY key -> semitone offset within the layout, or -1 if not a note key.
    static int semitone_for_key(KeyCode k) {
        switch (k) {
            case KeyCode::a: return 0;   case KeyCode::w: return 1;
            case KeyCode::s: return 2;   case KeyCode::e: return 3;
            case KeyCode::d: return 4;   case KeyCode::f: return 5;
            case KeyCode::t: return 6;   case KeyCode::g: return 7;
            case KeyCode::y: return 8;   case KeyCode::h: return 9;
            case KeyCode::u: return 10;  case KeyCode::j: return 11;
            case KeyCode::k: return 12;  case KeyCode::o: return 13;
            case KeyCode::l: return 14;  case KeyCode::p: return 15;
            default: return -1;
        }
    }

private:
    int base_note_ = 60;
    int octave_shift_ = 0;
    std::unordered_map<int, int> held_;  // keyCode -> note currently sounding
};

}  // namespace pulp::view

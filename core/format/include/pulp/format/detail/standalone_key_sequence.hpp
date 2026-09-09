#pragma once

#include <pulp/view/input_events.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::format::detail {

/// One synthesized key press: the portable key identity plus its modifier mask.
/// The platform driver owns the translation back to a native virtual keycode.
struct KeySequenceStep {
    view::KeyCode key = view::KeyCode::unknown;
    uint16_t modifiers = 0;
    /// The normalized source token ("down", "cmd+a"). Kept so a run can name
    /// its artifacts and logs after what the operator actually asked for
    /// rather than after a re-derived guess at the key's spelling.
    std::string label;
};

/// Map a single modifier token ("cmd", "shift", ...) to its mask bit.
/// Returns 0 for an unrecognized token so the caller can reject the whole spec.
inline uint16_t key_sequence_modifier_from_name(std::string_view name) {
    if (name == "cmd" || name == "command" || name == "meta" || name == "super")
        return view::kModCmd;
    if (name == "shift") return view::kModShift;
    if (name == "ctrl" || name == "control") return view::kModCtrl;
    if (name == "alt" || name == "opt" || name == "option") return view::kModAlt;
    return 0;
}

/// Map a key token ("down", "return", "a", "f7") to its portable KeyCode.
/// Returns KeyCode::unknown for an unrecognized token.
inline view::KeyCode key_sequence_key_from_name(std::string_view name) {
    using KC = view::KeyCode;
    if (name.empty()) return KC::unknown;

    if (name == "down") return KC::down;
    if (name == "up") return KC::up;
    if (name == "left") return KC::left;
    if (name == "right") return KC::right;
    if (name == "home") return KC::home;
    if (name == "end") return KC::end_;
    if (name == "pageup") return KC::page_up;
    if (name == "pagedown") return KC::page_down;
    // `return` and `enter` are the same physical key here; KeyCode spells it
    // `enter`, while every keyboard and test script says "return".
    if (name == "return" || name == "enter") return KC::enter;
    if (name == "escape" || name == "esc") return KC::escape;
    if (name == "tab") return KC::tab;
    if (name == "space") return KC::space;
    if (name == "backspace") return KC::backspace;
    if (name == "delete" || name == "del") return KC::delete_;

    // Single printable character: letters and digits are ASCII-valued in
    // KeyCode, so an 'a'..'z' / '0'..'9' token maps straight through.
    if (name.size() == 1) {
        const char c = name.front();
        if (c >= 'a' && c <= 'z') return static_cast<KC>(c);
        if (c >= '0' && c <= '9') return static_cast<KC>(c);
        if (c == ';') return KC::semicolon;
        if (c == '\'') return KC::apostrophe;
    }

    // Function keys f1..f12.
    if (name.size() >= 2 && name.front() == 'f') {
        int n = 0;
        for (size_t i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') return KC::unknown;
            n = n * 10 + (name[i] - '0');
            if (n > 12) return KC::unknown;
        }
        if (n >= 1 && n <= 12)
            return static_cast<KC>(static_cast<int>(KC::f1) + (n - 1));
    }
    return KC::unknown;
}

/// Parse a comma-separated key sequence such as "down,down,cmd+a,return".
///
/// Fails CLOSED: any unknown key or modifier token rejects the entire spec
/// rather than silently dropping that step. A driver that quietly skips a
/// typo'd key would report a passing run that pressed fewer keys than asked,
/// which is exactly the silent-underrun failure a test harness must not have.
/// Whitespace around tokens is ignored; an empty spec parses to zero steps.
inline bool parse_key_sequence(std::string_view spec,
                               std::vector<KeySequenceStep>& out,
                               std::string& error) {
    out.clear();
    error.clear();

    auto trim = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
            v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
            v.remove_suffix(1);
        return v;
    };
    auto lower = [](std::string_view v) {
        std::string s(v);
        for (auto& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    };

    std::string_view remaining = spec;
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        auto token = trim(remaining.substr(0, comma));
        if (comma == std::string_view::npos)
            remaining = {};
        else
            remaining.remove_prefix(comma + 1);

        if (token.empty()) continue;

        KeySequenceStep step;
        std::string_view rest = token;
        // Every '+'-separated segment before the last one is a modifier.
        for (;;) {
            const auto plus = rest.find('+');
            if (plus == std::string_view::npos) break;
            const auto mod_name = lower(trim(rest.substr(0, plus)));
            const uint16_t bit = key_sequence_modifier_from_name(mod_name);
            if (bit == 0) {
                error = "unknown modifier '" + mod_name + "'";
                out.clear();
                return false;
            }
            step.modifiers |= bit;
            rest.remove_prefix(plus + 1);
        }

        const auto key_name = lower(trim(rest));
        step.key = key_sequence_key_from_name(key_name);
        if (step.key == view::KeyCode::unknown) {
            error = "unknown key '" + key_name + "'";
            out.clear();
            return false;
        }
        // Normalized (lowercased, inner whitespace removed) so "Cmd + A" and
        // "cmd+a" name the same artifact.
        step.label = lower(token);
        step.label.erase(std::remove_if(step.label.begin(), step.label.end(),
                                        [](char c) { return c == ' ' || c == '\t'; }),
                         step.label.end());
        out.push_back(step);
    }
    return true;
}

}  // namespace pulp::format::detail

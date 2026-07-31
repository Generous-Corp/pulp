#pragma once

// The `@` mention list — a dropdown over the composer.
//
// Typing `@` in the prompt opens a filtered list of Rack modules; typing
// narrows it, up/down moves, Enter or click inserts, Escape dismisses. It sits
// over the prompt card, which is why it is an overlay rather than an ordinary
// child, and why this is C++ rather than the widget bridge — the bridge exposes
// `start` and `end` insets and nothing vertical, so it cannot place a dropdown
// under a caret at all.
//
// Three availability states, and only one of them can be inserted. Rack keeps
// missing modules as placeholders and offers to fetch them, so an unavailable
// mention is an offer rather than a wall — but wiring a patch to a module the
// user does not have produces a patch that cannot make sound, which is the
// failure this distinction exists to prevent.

#include <pulp/view/view.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace forge_modular {

/// One row in the list.
struct MentionCandidate {
    enum class Availability {
        ready,       ///< installed; can be wired
        available,   ///< in the library, free, not installed yet
        paid,        ///< in the library, costs money
    };
    std::string brand;      ///< "Fundamental", "4ms"
    std::string name;       ///< "VCO", "DrumBus"
    std::string slug;       ///< what gets inserted
    Availability state = Availability::available;

    /// Only an installed module can be wired into a patch that will sound.
    bool insertable() const { return state == Availability::ready; }
};

/// Where candidates come from. A function rather than a baked list so the
/// 4,705-module index can be loaded from wherever it actually lives without
/// this file knowing about paths, and so a test can supply five instead.
using MentionSource = std::function<std::vector<MentionCandidate>(const std::string& query)>;

/// The dropdown.
///
/// Owns no text: it observes what was typed and reports what was chosen. The
/// composer stays the single source of truth for the prompt, so there is never
/// a second copy to disagree with.
class MentionOverlay {
public:
    /// Build the view. Hidden until `open()`.
    std::unique_ptr<pulp::view::View> build();

    void set_source(MentionSource source) { source_ = std::move(source); }

    /// Called with the slug when a row is chosen.
    std::function<void(const std::string& slug)> on_choose;

    /// Feed it the prompt text and the caret. Returns true when the overlay
    /// wants the keystroke -- the composer must not also act on it, or Enter
    /// both inserts a mention and submits the prompt.
    bool handle_text(const std::string& text, std::size_t caret);

    /// Arrow keys, Enter, Escape. Returns true when consumed, for the same
    /// reason.
    bool handle_key(int key_code);

    bool is_open() const { return open_; }
    void close();

    /// Drop the pointers into a view tree that is going away.
    ///
    /// The overlay belongs to the shell, which outlives every editor, while the
    /// views belong to the editor's tree. `build()` supplies fresh ones for the
    /// next editor; between the two there is nothing here to point at, and a
    /// kept pointer would name freed memory.
    void forget_views() {
        root_ = nullptr;
        list_ = nullptr;
        open_ = false;
    }

    /// What is currently listed, in order. Exposed so a test can assert the
    /// filtering rather than scrape the view tree.
    const std::vector<MentionCandidate>& candidates() const { return candidates_; }
    int selected_index() const { return selected_; }

private:
    void refresh(const std::string& query);
    void rebuild_rows();

    pulp::view::View* root_ = nullptr;
    pulp::view::View* list_ = nullptr;
    MentionSource source_;
    std::vector<MentionCandidate> candidates_;
    std::string query_;
    int selected_ = 0;
    bool open_ = false;
};

}  // namespace forge_modular

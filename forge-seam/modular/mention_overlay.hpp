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

#include <pulp/view/widgets.hpp>
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

    /// The name the query actually matched, when it is not the one displayed.
    ///
    /// VCV's display names and the names people use are often different words:
    /// Audible Instruments' "Macro Oscillator" is Braids, "Bernoulli Gate" is
    /// Branches. Typing "br" finds them by slug, and the row showed neither
    /// the query nor anything containing it, so a correct match read as a
    /// random one.
    ///
    /// LAST on purpose: every existing brace-initialiser in the tests is
    /// positional, and a field added in the middle breaks all of them.
    std::string alias;

    /// What this row names.
    ///
    /// A MAKER IS A ROW IN ITS OWN RIGHT. Typing `@CV` offers "CV funk, 50
    /// modules" above the individual hits, and picking it inserts the maker
    /// rather than a module -- because naming a maker is a sourcing
    /// preference, and the thing that expands it is the prompt inventory, not
    /// the patch. Nothing about picking this row places 50 modules.
    ///
    /// Also last, for the same reason as `alias`.
    enum class Kind { module_, brand };
    Kind kind = Kind::module_;

    /// Only an installed module can be wired into a patch that will sound. A
    /// maker is always pickable: it costs nothing and installs nothing.
    bool insertable() const {
        return kind == Kind::brand || state == Availability::ready;
    }
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
/// A row's name, cut to the width a row can hold.
///
/// Exposed so the rule can be tested directly. A Label sizes to its text and
/// does not ellipsize, and flex_shrink cannot shorten a string, so a long name
/// ran under the badge beside it — cutting the string is the only thing that
/// actually shortens it.
std::string elide_for_row(const std::string& text);

class MentionOverlay {
public:
    /// Build the view. Hidden until `open()`.
    std::unique_ptr<pulp::view::View> build();

    void set_source(MentionSource source) { source_ = std::move(source); }

    /// Called with the slug when a row is chosen.
    std::function<void(const std::string& slug)> on_choose;

    /// Why a pick did nothing. A module that is not installed cannot be wired
    /// into a patch that will sound, so it is refused — and refusing in
    /// silence reads as a broken list. Reported as: "you cannot pick this, and
    /// here is what to do about it."
    std::function<void(const MentionCandidate& what)> on_refused;

    /// What a notice MEANS, which is what decides its colour.
    ///
    /// Amber says "something is wrong". A download starting is not wrong, and
    /// colouring ordinary progress as a warning spends the one colour that
    /// should make somebody look. Only a genuine block -- not signed in, or
    /// paid and unowned -- earns it.
    enum class Tone { progress, blocked };

    /// Say something beside the composer that outlives the list closing.
    ///
    /// The list is hidden the moment a row is chosen, and the run card does
    /// not exist until a build starts, so neither can carry a message about
    /// the pick that just happened.
    void show_notice(const std::string& text, Tone tone = Tone::progress);
    Tone notice_tone() const { return notice_tone_; }
    /// The colour the notice is ACTUALLY drawn in.
    ///
    /// Not the same question as notice_tone(): the tone is what was asked for
    /// and this is what was done about it, and a test that only reads the
    /// former cannot notice a notice that is amber whatever it is told.
    pulp::canvas::Color notice_color() const;

    /// Where the composer is, in root coordinates.
    ///
    /// A notice about a pick belongs under the thing that was being typed
    /// into, at its inset and its width. Hard-coding the dropdown's own
    /// narrower geometry made the message read as a detached box floating
    /// beside the card. The shell measures the card and tells us, because only
    /// it can see the laid-out tree.
    void set_composer_frame(float left, float width);

    /// The panel the rows and the notice are drawn on, for a test that asserts
    /// what a person is actually shown lines up with the composer.
    pulp::view::View* panel() { return root_; }
    /// The label the notice is drawn into. Owned by the shell, because it
    /// has to survive the list being hidden.
    void attach_notice(pulp::view::Label* label);
    const std::string& notice() const { return notice_text_; }

    /// Feed it the prompt text and the caret. Returns true when the overlay
    /// wants the keystroke -- the composer must not also act on it, or Enter
    /// both inserts a mention and submits the prompt.
    bool handle_text(const std::string& text, std::size_t caret);

    /// Arrow keys, Enter, Escape. Returns true when consumed, for the same
    /// reason.
    bool handle_key(int key_code);

    /// The same, from a real key event. Kept separate from the raw-code form
    /// because the raw codes are macOS virtual keys and an event carries a
    /// portable enum -- translating in one place beats two tables that drift.
    bool handle_key_event(const pulp::view::KeyEvent& event);

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
        row_views_.clear();
        open_ = false;
    }

    /// What is currently listed, in order. Exposed so a test can assert the
    /// filtering rather than scrape the view tree.
    const std::vector<MentionCandidate>& candidates() const { return candidates_; }
    int selected_index() const { return selected_; }

    /// First candidate currently drawn. The list holds every match and shows a
    /// window of them, so this is what the up/down affordances are about.
    int scroll_top() const { return first_visible_; }

    /// How many rows are drawn at once.
    static int visible_rows();

    /// Choose the row at `index` -- what a click does. Public so a test can
    /// exercise the same path the mouse takes rather than a private cousin.
    void choose(std::size_t index);

    /// Move the selection, scrolling it into view. `delta` of +1 is Down.
    void move_selection(int delta);

private:
    void refresh(const std::string& query);
    void rebuild_rows();
    /// Re-colour the existing rows for the current selection.
    ///
    /// Separate from rebuild_rows() because hovering must NOT restructure the
    /// view tree: the hover is dispatched while the framework is walking the
    /// child list, and destroying those children mid-walk leaves it iterating
    /// freed memory. That segfaulted on a mouse move.
    void highlight_selected();

    std::vector<pulp::view::View*> row_views_;
    /// Keep the selected row inside the drawn window.
    void scroll_to_selection();

    int first_visible_ = 0;

    pulp::view::View* root_ = nullptr;
    pulp::view::View* list_ = nullptr;
    MentionSource source_;
    std::vector<MentionCandidate> candidates_;
    std::string query_;
    int selected_ = 0;
    bool open_ = false;
    pulp::view::Label* notice_ = nullptr;
    std::string notice_text_;
    Tone notice_tone_ = Tone::progress;
    /// The composer's measured inset and width, or 0 before anything has
    /// measured it. Applied whenever the panel is carrying a notice with the
    /// rows gone; the rows keep the dropdown's own narrower geometry.
    float composer_left_ = 0;
    float composer_width_ = 0;
    void apply_panel_frame();
    /// True only while choose() is rewriting the field, so the change that
    /// rewrite provokes is not mistaken for the user typing.
    bool inserting_ = false;
};

}  // namespace forge_modular

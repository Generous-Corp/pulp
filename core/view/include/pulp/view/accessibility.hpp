#pragma once

// Accessibility interfaces for platform screen reader integration.
// Provides ValueInterface, TextInterface, TableInterface, CellInterface
// that can be attached to Views for accessibility provider implementation.

#include <string>
#include <string_view>
#include <functional>
#include <optional>
#include <vector>

namespace pulp::view {

/// Interface for accessible values (sliders, knobs, progress bars)
class AccessibilityValueInterface {
public:
    // Out-of-line in src/accessibility.cpp so the vtable + typeinfo
    // anchors there — required for dynamic_cast on Android NDK builds
    // that link with -Wl,--no-undefined.
    virtual ~AccessibilityValueInterface();

    /// Current value as a number
    virtual double get_current_value() const = 0;

    /// Set the value (if editable)
    virtual void set_current_value(double value) = 0;

    /// Minimum value
    virtual double get_minimum_value() const = 0;

    /// Maximum value
    virtual double get_maximum_value() const = 0;

    /// Step increment for keyboard navigation
    virtual double get_step_size() const { return (get_maximum_value() - get_minimum_value()) / 100.0; }

    /// Value as a human-readable string (e.g., "50%" or "-6 dB")
    virtual std::string get_value_string() const;
};

/// Interface for accessible text content (text editors, labels)
class AccessibilityTextInterface {
public:
    virtual ~AccessibilityTextInterface();

    /// Get the full text
    virtual std::string get_text() const = 0;

    /// Set the text (if editable)
    virtual void set_text(std::string_view text) = 0;

    /// Get selected text range
    virtual std::pair<int, int> get_selection() const { return {0, 0}; }

    /// Set selection range
    virtual void set_selection(int start, int end) { (void)start; (void)end; }

    /// Whether the text is editable
    virtual bool is_editable() const { return false; }

    /// Number of characters
    virtual int get_character_count() const {
        return static_cast<int>(get_text().size());
    }

    /// Get text in a range
    virtual std::string get_text_range(int start, int end) const {
        auto text = get_text();
        if (start < 0) start = 0;
        if (end > static_cast<int>(text.size())) end = static_cast<int>(text.size());
        if (start >= end) return "";
        return text.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
    }
};

/// Interface for accessible tables and lists
class AccessibilityTableInterface {
public:
    virtual ~AccessibilityTableInterface();

    /// Number of rows
    virtual int get_row_count() const = 0;

    /// Number of columns
    virtual int get_column_count() const = 0;

    /// Get header text for a column
    virtual std::string get_column_header(int column) const = 0;

    /// Get the currently selected row (-1 if none)
    virtual int get_selected_row() const { return -1; }

    /// Select a row
    virtual void select_row(int row) { (void)row; }

    /// Whether multiple selection is supported
    virtual bool supports_multi_selection() const { return false; }

    /// Get all selected rows
    virtual std::vector<int> get_selected_rows() const {
        int row = get_selected_row();
        return row >= 0 ? std::vector<int>{row} : std::vector<int>{};
    }
};

/// Interface for individual table cells
class AccessibilityCellInterface {
public:
    virtual ~AccessibilityCellInterface();

    /// Cell text content
    virtual std::string get_cell_text(int row, int column) const = 0;

    /// Whether the cell is editable
    virtual bool is_cell_editable(int row, int column) const { (void)row; (void)column; return false; }

    /// Row and column span (for merged cells)
    virtual std::pair<int, int> get_cell_span(int row, int column) const {
        (void)row; (void)column;
        return {1, 1};
    }
};

// ── Value resolution ────────────────────────────────────────────────────

class View;

/// Tri-state check/press state, resolved from the ARIA slots a View carries
/// (`aria-checked` first, then `aria-pressed` — a checkbox/switch state is
/// more load-bearing than a toggle-button pressed state).
enum class AccessToggleState { unset, off, on, mixed };

/// The View's check/press state. `unset` for everything that is not a
/// checkbox / switch / toggle button.
AccessToggleState accessibility_toggle_state(const View& v);

/// The state as the word a reader announces: "checked" / "unchecked" /
/// "mixed" (aria-checked) or "pressed" / "not pressed" / "mixed"
/// (aria-pressed). Empty when `unset`.
std::string accessibility_toggle_state_string(const View& v);

/// What assistive tech reads as the element's VALUE, resolved from the one
/// source that is actually present, in priority order:
///
///   1. AccessibilityValueInterface  → get_value_string()  (slider, meter,
///      progress bar — "-6.0 dB", "60%")
///   2. AccessibilityTextInterface   → get_text()          (text field — the
///      content the user typed)
///   3. View::access_value()         → the manually-set string (combo box's
///      selected item, or any app-set value)
///   4. The check/press state         → "checked" / "unchecked" / "mixed"
///      (a Checkbox / Toggle / ToggleButton has no other value; without this
///      slot iOS and Android announce "button" with no state at all — only
///      macOS special-cases it into a native @YES/@NO accessibilityValue)
///   5. "" — nothing to read.
///
/// Every platform bridge that reads a VALUE resolves it through THIS function:
/// macOS (state → native @YES/@NO first, then this), iOS, Android, Windows
/// (IValueProvider::get_Value + UIA_ValueValuePropertyId) and Linux
/// (org.a11y.atspi.Text, via accessibility_text_content()). macOS's
/// -accessibilityValue used to read only slot 3, so a TextEditor holding
/// "hello" returned nil and VoiceOver announced "text field" and then silence.
///
/// NOT covered by this function: Windows never announces a check/press state at
/// all, because the UIA Toggle pattern needs an IToggleProvider that
/// PulpFragmentProvider does not implement (see accessibility_win.cpp).
std::string accessibility_value_string(const View& v);

/// True when the view has ANY value source at all (slots 1–4 above). The
/// Windows provider gates the UIA Value pattern on this: advertising
/// IsValuePatternAvailable on an element whose get_Value returns a NULL BSTR
/// is worse than not advertising it.
bool has_accessibility_value(const View& v);

/// The accessibility interfaces a View can actually serve. Platform bridges
/// that publish a per-interface surface (AT-SPI's org.a11y.atspi.Value /
/// org.a11y.atspi.Text; UIA's pattern set) export exactly these — an interface
/// advertised without a source behind it reads back empty.
struct AccessibilityInterfaceSet {
    /// Numeric range (min / max / current): AccessibilityValueInterface.
    bool value = false;
    /// String content: AccessibilityTextInterface (a TextEditor) OR the
    /// access_value slot (a ComboBox's selected item). AT-SPI has no "value
    /// string" interface — a string value is read through Text.
    bool text = false;
};
AccessibilityInterfaceSet accessibility_interfaces(const View& v);

/// The string content a text interface exposes: AccessibilityTextInterface's
/// content, else the access_value slot, else "". This is what AT-SPI's
/// Text.GetText / Text.CharacterCount serve.
std::string accessibility_text_content(const View& v);

/// True when the text content is user-editable (an editable
/// AccessibilityTextInterface). Drives AT-SPI's EDITABLE state and UIA's
/// IsReadOnly.
bool accessibility_text_editable(const View& v);

/// The single gate EVERY accessibility bridge uses to decide whether a View
/// enters the platform accessibility tree (macOS window host + plug-in editor
/// host, iOS, Windows UIA fragments, Linux AT-SPI objects, Android TalkBack
/// nodes) and the cross-platform snapshot's count_announceable().
///
/// A View is exposed when it has a role AND something to say with it:
///
///   * a structural role (group / list / table / dialog / ... ) — it announces
///     the children underneath it;
///   * an accessible name;
///   * a value source (slider, meter, progress bar, text field, combo box);
///   * an ARIA state (aria-checked / aria-pressed — a checkbox or switch).
///
/// A role with NONE of those announces "button" (or "text field", or "image")
/// and then falls silent: that is a WCAG 4.1.2 (Name, Role, Value) failure and
/// pure noise in the tree, so the View is left out until it has a name. Keep
/// this the ONLY predicate — a bridge that hand-rolls `access_role() != none`
/// re-admits unnamed buttons on that one platform.
bool is_accessibility_element(const View& v);

// ── Live-region announcements ───────────────────────────────────────────

/// Politeness level for a live-region announcement. Mirrors the WAI-ARIA
/// `aria-live` values: polite announcements wait for the current reader
/// utterance to finish; assertive announcements interrupt.
enum class AnnouncementPriority {
    Polite,
    Assertive,
};

/// Request the installed accessibility announcement sink speak `text`. When no
/// sink is installed (or no screen reader is active), the call is logged at info
/// level and otherwise a no-op — it is always safe to call, including from test
/// harnesses.
///
/// The C++ API and sink plumbing are present. Built-in platform bridge sinks are
/// still pending: macOS/iOS should install the native announcement notification
/// path, Android should route through TalkBack TYPE_ANNOUNCEMENT, Windows
/// through UiaRaiseNotificationEvent, and Linux through AT-SPI announcement
/// events.
void announce_accessibility(std::string_view text,
                            AnnouncementPriority priority = AnnouncementPriority::Polite);

/// Install a platform-specific announcement sink when a live-region backend is
/// available. Pass nullptr to detach (the default logger is restored). Not
/// thread-safe — UI thread only.
using AnnouncementSink =
    std::function<void(std::string_view text, AnnouncementPriority)>;
void set_announcement_sink(AnnouncementSink sink);

}  // namespace pulp::view

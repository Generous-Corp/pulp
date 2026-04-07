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
    virtual ~AccessibilityValueInterface() = default;

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
    virtual ~AccessibilityTextInterface() = default;

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
    virtual ~AccessibilityTableInterface() = default;

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
    virtual ~AccessibilityCellInterface() = default;

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

}  // namespace pulp::view

#pragma once

/// @file
/// A ready-made "About / third-party notices" view for a Vellum editor.
///
/// `AboutNotices` presents the `format::ThirdPartyNotices` of a plug-in bundle:
/// the product name, one row per notice (name, version, license) and the full
/// license text of the one row the user expands. The rows are derived by the
/// pure `make_about_model`, so what the view shows can be read and verified
/// without painting it. Needs Vellum's view layer, so it is only built when the
/// SDK ships one.

#include <pulp/format/third_party_notices.hpp>

#include <vellum/view/view.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pulp::vellum {

/// One displayed notice. `license_text` is empty unless the row is expanded.
struct AboutRow {
    std::string name;
    std::string version;
    std::string license;
    std::string license_text;
    bool expanded = false;
};

/// Everything the view displays, in display order.
struct AboutModel {
    std::string product;
    std::vector<AboutRow> rows;
    /// Shown instead of the rows when there are none: the notices' `reason`
    /// when one was recorded, otherwise a neutral line.
    std::string empty_text;
};

/// Builds the model for `notices` with at most one row expanded. An
/// out-of-range `expanded` index expands nothing.
AboutModel make_about_model(const format::ThirdPartyNotices& notices,
                            std::optional<std::size_t> expanded = std::nullopt);

class AboutNotices : public ::vellum::view::View {
  public:
    /// Shows `notices`; the view keeps its own copy.
    explicit AboutNotices(const format::ThirdPartyNotices& notices);
    /// Shows the running module's own notices (`format::third_party_notices()`).
    AboutNotices();

    const AboutModel& model() const {
        return model_;
    }
    std::optional<std::size_t> expanded_row() const {
        return expanded_;
    }

    /// Expands one row (collapsing any other) or, with `std::nullopt`, none.
    void set_expanded(std::optional<std::size_t> row);
    /// Expands `row`, or collapses it when it is already the expanded one.
    void toggle(std::size_t row);

    /// The license text on display, or empty when no row is expanded.
    const std::string& shown_license_text() const;

    /// The view for a row, for hit-testing; invalidated by the next change.
    ::vellum::view::View* row_view(std::size_t row);

  private:
    void build();
    void refresh();

    format::ThirdPartyNotices notices_;
    std::optional<std::size_t> expanded_;
    AboutModel model_;
    ::vellum::view::View* list_ = nullptr;
};

} // namespace pulp::vellum

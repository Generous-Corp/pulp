#include <pulp/vellum/about_notices.hpp>

#include <vellum/view/ui_components.hpp>
#include <vellum/view/widgets.hpp>

#include <memory>
#include <utility>

namespace pulp::vellum {

namespace vv = ::vellum::view;

AboutModel make_about_model(const format::ThirdPartyNotices& notices,
                            std::optional<std::size_t> expanded) {
    AboutModel model;
    model.product = notices.product;
    model.rows.reserve(notices.notices.size());
    for (std::size_t i = 0; i < notices.notices.size(); ++i) {
        const auto& notice = notices.notices[i];
        AboutRow row;
        row.name = notice.name;
        row.version = notice.version;
        row.license = notice.license;
        row.expanded = expanded.has_value() && *expanded == i;
        if (row.expanded)
            row.license_text = notice.license_text;
        model.rows.push_back(std::move(row));
    }
    if (model.rows.empty()) {
        model.empty_text =
            notices.reason.empty() ? "No third-party notices are available." : notices.reason;
    }
    return model;
}

AboutNotices::AboutNotices(const format::ThirdPartyNotices& notices)
    : notices_(notices), model_(make_about_model(notices_)) {
    build();
}

AboutNotices::AboutNotices() : AboutNotices(format::third_party_notices()) {}

void AboutNotices::set_expanded(std::optional<std::size_t> row) {
    if (row.has_value() && *row >= notices_.notices.size())
        row = std::nullopt;
    if (row == expanded_)
        return;
    expanded_ = row;
    refresh();
}

void AboutNotices::toggle(std::size_t row) {
    if (expanded_ == row)
        set_expanded(std::nullopt);
    else
        set_expanded(row);
}

const std::string& AboutNotices::shown_license_text() const {
    static const std::string none;
    if (!expanded_.has_value() || *expanded_ >= model_.rows.size())
        return none;
    return model_.rows[*expanded_].license_text;
}

vv::View* AboutNotices::row_view(std::size_t row) {
    if (list_ == nullptr || row >= model_.rows.size() || row >= list_->child_count())
        return nullptr;
    return list_->child_at(row);
}

void AboutNotices::build() {
    flex().direction = vv::FlexDirection::column;
    flex().padding = 12;
    flex().gap = 8;

    auto title = std::make_unique<vv::Label>(model_.product.empty() ? "About" : model_.product);
    title->set_font_weight(700);
    add_child(std::move(title));

    auto scroll = std::make_unique<vv::ScrollView>();
    scroll->set_direction(vv::ScrollView::Direction::vertical);
    scroll->flex().flex_grow = 1;

    auto list = std::make_unique<vv::View>();
    list->flex().direction = vv::FlexDirection::column;
    list->flex().gap = 6;
    list_ = list.get();

    if (model_.rows.empty())
        list->add_child(std::make_unique<vv::Label>(model_.empty_text));

    // Rows persist across expand and collapse: a click handler runs inside the
    // row it belongs to, so a toggle must never destroy that row.
    for (std::size_t i = 0; i < model_.rows.size(); ++i) {
        const auto& data = model_.rows[i];
        auto row = std::make_unique<vv::View>();
        row->flex().direction = vv::FlexDirection::column;
        row->flex().gap = 4;
        row->on_click = [this, i] { toggle(i); };

        auto header = std::make_unique<vv::View>();
        header->flex().direction = vv::FlexDirection::row;
        header->flex().gap = 8;
        header->add_child(std::make_unique<vv::Label>(data.name));
        header->add_child(std::make_unique<vv::Label>(data.version));
        header->add_child(std::make_unique<vv::Label>(data.license));
        row->add_child(std::move(header));
        list->add_child(std::move(row));
    }
    scroll->add_child(std::move(list));
    add_child(std::move(scroll));
}

void AboutNotices::refresh() {
    model_ = make_about_model(notices_, expanded_);
    for (std::size_t i = 0; i < model_.rows.size(); ++i) {
        vv::View* row = row_view(i);
        if (row == nullptr)
            continue;
        const bool has_text = row->child_count() > 1;
        if (model_.rows[i].expanded && !has_text) {
            row->add_child(std::make_unique<vv::Label>(model_.rows[i].license_text));
        } else if (!model_.rows[i].expanded && has_text) {
            row->remove_child(row->child_at(1));
        }
    }
}

} // namespace pulp::vellum

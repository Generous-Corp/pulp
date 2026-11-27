#include <pulp/vellum/settings_view.hpp>

#include <vellum/view/view.hpp>

#include <vellum/view/ui_components.hpp>

#include <utility>

namespace pulp::vellum {

std::unique_ptr<::vellum::view::View>
make_settings_view(std::vector<ViewSettingsSection> sections) {
    std::erase_if(sections, [](const ViewSettingsSection& s) { return s.view == nullptr; });
    if (sections.empty())
        return nullptr;
    if (sections.size() == 1)
        return std::move(sections.front().view);
    auto tabs = std::make_unique<::vellum::view::TabPanel>();
    for (auto& section : sections)
        tabs->add_tab(std::move(section.title), std::move(section.view));
    return tabs;
}

} // namespace pulp::vellum

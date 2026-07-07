// pulp-format: processor interface and format adapters
#include <pulp/format/format.hpp>
#include <pulp/view/view.hpp>

#include <utility>

namespace pulp::format {

Processor::SettingsSection::SettingsSection() = default;
Processor::SettingsSection::SettingsSection(std::string title_in,
                                             std::unique_ptr<view::View> view_in)
    : title(std::move(title_in)), view(std::move(view_in)) {}
Processor::SettingsSection::~SettingsSection() = default;
Processor::SettingsSection::SettingsSection(SettingsSection&&) noexcept = default;
Processor::SettingsSection& Processor::SettingsSection::operator=(SettingsSection&&) noexcept = default;

std::unique_ptr<view::View> Processor::create_view() {
    return nullptr;
}

} // namespace pulp::format

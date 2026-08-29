// Out-of-line special members for Processor::SettingsSection.
//
// SettingsSection owns a std::unique_ptr<view::View>, so its destructor and
// move operations need view::View complete. That completeness requirement,
// not any use of a view symbol, is why these five definitions sit on the
// view side of the split while Processor::create_view() stays in
// format.cpp: create_view() only ever returns nullptr, which the forward
// declaration in processor.hpp already satisfies.
//
// Keep this file free of anything the vtable references. format.cpp is the
// key-function TU and must remain independently linkable from
// pulp-format-core; moving a vtable slot's definition here would break that.
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

} // namespace pulp::format

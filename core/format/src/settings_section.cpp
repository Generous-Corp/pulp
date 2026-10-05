// The two Processor::SettingsSection members that need view::View complete.
//
// SettingsSection holds its view through a type-erased deleter, so its
// destructor and moves are defaulted in processor.hpp and compile wherever
// View is incomplete. Adopting a std::unique_ptr<view::View> (which installs a
// deleter that runs `delete` on a complete View) and handing it back out as an
// ordinary owning pointer are the only operations that need the type, so they
// sit here, on the view side of the format-core / format-view split.
//
// Keep this file free of anything the vtable references. format.cpp is the
// key-function TU and must remain independently linkable from
// pulp-format-core; moving a vtable slot's definition here would break that.
#include <pulp/format/format.hpp>
#include <pulp/view/view.hpp>

#include <utility>

namespace pulp::format {

Processor::SettingsSection::SettingsSection(std::string title_in,
                                             std::unique_ptr<view::View> view_in)
    : title(std::move(title_in)),
      view(view_in.release(), [](view::View* v) { delete v; }) {}

std::unique_ptr<view::View> Processor::SettingsSection::take_view() {
    return std::unique_ptr<view::View>(view.release());
}

} // namespace pulp::format

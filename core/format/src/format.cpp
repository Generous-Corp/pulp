// pulp-format: the Processor vtable anchor.
//
// The destructor is the first non-inline, non-pure virtual on Processor. Keep
// its definition in the core half so pulp-format-core owns the Processor vtable
// without compiling against the view include tree. The view-returning
// create_view() default lives in format_view.cpp on pulp-format-view.
#include <pulp/format/format.hpp>

namespace pulp::format {

Processor::~Processor() = default;

} // namespace pulp::format

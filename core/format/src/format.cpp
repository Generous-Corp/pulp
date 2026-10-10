// pulp-format: the Processor vtable anchor.
//
// The destructor is the first non-inline, non-pure virtual on Processor. Keep
// The default view factory is the first non-inline virtual, so its definition
// anchors the Processor vtable in pulp-format-core without compiling against
// the view include tree. Returning nullptr only needs the forward declaration
// from processor.hpp, and keeping the symbol in core is required by core-only
// consumers whose derived vtables retain the slot.
#include <pulp/format/format.hpp>

namespace pulp::format {

std::unique_ptr<view::View> Processor::create_view() {
    return nullptr;
}

} // namespace pulp::format

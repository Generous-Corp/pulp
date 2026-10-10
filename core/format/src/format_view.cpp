// View-coupled default Processor factory.
//
// Keeping this definition on pulp-format-view lets pulp-format-core compile
// without the view include path while preserving the public Processor ABI.
#include <pulp/format/format.hpp>
#include <pulp/view/view.hpp>

namespace pulp::format {

std::unique_ptr<view::View> Processor::create_view() {
    return nullptr;
}

} // namespace pulp::format

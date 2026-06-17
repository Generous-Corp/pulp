#include <pulp/view/group_box_view.hpp>
#include <pulp/runtime/base64.hpp>

#include <string>

namespace pulp::view {

namespace detail { const char* group_box_view_svg_b64(); }

namespace {
std::string decode_group_box_view_svg() {
    if (auto bytes = runtime::base64_decode(detail::group_box_view_svg_b64()))
        return std::string(bytes->begin(), bytes->end());
    return {};
}
}  // namespace

GroupBoxView::GroupBoxView() : DesignFrameView(decode_group_box_view_svg(), /*elements=*/{}) {}

}  // namespace pulp::view

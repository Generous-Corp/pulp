#include <pulp/view/range_slider_view.hpp>
#include <pulp/runtime/base64.hpp>

#include <string>

namespace pulp::view {

namespace detail { const char* range_slider_view_svg_b64(); }

namespace {
std::string decode_range_slider_view_svg() {
    if (auto bytes = runtime::base64_decode(detail::range_slider_view_svg_b64()))
        return std::string(bytes->begin(), bytes->end());
    return {};
}
}  // namespace

RangeSliderView::RangeSliderView() : DesignFrameView(decode_range_slider_view_svg(), /*elements=*/{}) {}

}  // namespace pulp::view

#include <pulp/view/channel_strip_view.hpp>
#include <pulp/runtime/base64.hpp>

#include <string>

namespace pulp::view {

namespace detail { const char* channel_strip_view_svg_b64(); }

namespace {
std::string decode_channel_strip_view_svg() {
    if (auto bytes = runtime::base64_decode(detail::channel_strip_view_svg_b64()))
        return std::string(bytes->begin(), bytes->end());
    return {};
}
}  // namespace

ChannelStripView::ChannelStripView() : DesignFrameView(decode_channel_strip_view_svg(), /*elements=*/{}) {}

}  // namespace pulp::view

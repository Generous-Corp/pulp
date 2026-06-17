#include <pulp/view/waveform_recorder_view.hpp>
#include <pulp/runtime/base64.hpp>

#include <string>

namespace pulp::view {

namespace detail { const char* waveform_recorder_view_svg_b64(); }

namespace {
std::string decode_waveform_recorder_view_svg() {
    if (auto bytes = runtime::base64_decode(detail::waveform_recorder_view_svg_b64()))
        return std::string(bytes->begin(), bytes->end());
    return {};
}
}  // namespace

WaveformRecorderView::WaveformRecorderView() : DesignFrameView(decode_waveform_recorder_view_svg(), /*elements=*/{}) {}

}  // namespace pulp::view

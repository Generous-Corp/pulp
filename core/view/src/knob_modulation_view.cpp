#include <pulp/view/knob_modulation_view.hpp>
#include <pulp/runtime/base64.hpp>

#include <string>

namespace pulp::view {

namespace detail { const char* knob_modulation_view_svg_b64(); }

namespace {
std::string decode_knob_modulation_view_svg() {
    if (auto bytes = runtime::base64_decode(detail::knob_modulation_view_svg_b64()))
        return std::string(bytes->begin(), bytes->end());
    return {};
}
}  // namespace

KnobModulationView::KnobModulationView() : DesignFrameView(decode_knob_modulation_view_svg(), /*elements=*/{}) {}

}  // namespace pulp::view

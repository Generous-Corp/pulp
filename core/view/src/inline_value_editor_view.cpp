#include <pulp/view/inline_value_editor_view.hpp>
#include <pulp/runtime/base64.hpp>

#include <string>

namespace pulp::view {

namespace detail { const char* inline_value_editor_view_svg_b64(); }

namespace {
std::string decode_inline_value_editor_view_svg() {
    if (auto bytes = runtime::base64_decode(detail::inline_value_editor_view_svg_b64()))
        return std::string(bytes->begin(), bytes->end());
    return {};
}
}  // namespace

InlineValueEditorView::InlineValueEditorView() : DesignFrameView(decode_inline_value_editor_view_svg(), /*elements=*/{}) {}

}  // namespace pulp::view

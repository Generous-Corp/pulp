#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/runtime/base64.hpp>

#include <string>

namespace pulp::view {

namespace detail {
const char* musical_typing_svg_b64();  // defined in musical_typing_keyboard_svg.cpp
}

namespace {
// Decode the embedded faithful SVG once. Host-side construction; never the
// audio/render thread.
std::string decode_embedded_svg() {
    if (auto bytes = runtime::base64_decode(detail::musical_typing_svg_b64()))
        return std::string(bytes->begin(), bytes->end());
    return {};
}
}  // namespace

MusicalTypingKeyboard::MusicalTypingKeyboard()
    : DesignFrameView(decode_embedded_svg(), /*elements=*/{}) {}

}  // namespace pulp::view

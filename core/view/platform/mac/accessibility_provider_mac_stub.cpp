#include <pulp/view/accessibility_provider.hpp>

namespace pulp::view {

// The SDL host uses the platform provider contract even on macOS, while the
// native AppKit host owns the richer VoiceOver bridge in accessibility_mac.mm.
// Keep the SDL/macOS path link-complete without making it depend on an AppKit
// host lifetime or duplicating the native element tree.
void* init_accessibility(View&, void*) {
    return nullptr;
}

void shutdown_accessibility(void*) {}

} // namespace pulp::view

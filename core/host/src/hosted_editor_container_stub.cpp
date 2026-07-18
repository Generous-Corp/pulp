// Editor-container stub for platforms with no native-window seam.
//
// Windows and Linux have no desktop WindowHost that implements
// attach_native_child_view today, so there is nothing to parent an editor into.
// Returning nullptr makes create_hosted_editor() report "no editor", which
// EditorAttachment already handles by falling back to no attachment. The
// per-format negotiation code stays compiled and unit-testable on every
// platform; only the platform seam is absent.

#include <pulp/host/hosted_editor_container.hpp>

namespace pulp::host {

void* create_editor_container(void*, uint32_t, uint32_t) { return nullptr; }

void resize_editor_container(void*, uint32_t, uint32_t) {}

void destroy_editor_container(void*) {}

double editor_container_scale(void*) { return 1.0; }

} // namespace pulp::host

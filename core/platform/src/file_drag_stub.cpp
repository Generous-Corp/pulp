// Platforms without a system file drag from core/platform. Recorded drags
// still work through the common code.

#include <pulp/platform/file_drag.hpp>

namespace pulp::platform::detail {

FileDragError begin_native_file_drag(MainThreadScope::Handle, NativeViewHandle,
                                     const FileDragRequest&, std::function<void(FileDragEnd)>,
                                     void*) {
    return FileDragError::unavailable;
}

bool drag_event_anchors(NativeViewHandle, void*) {
    return false;
}

void* make_drag_image(const DragImage&) {
    return nullptr;
}

void* make_file_drag_source(unsigned, std::function<void(FileDragEnd)>) {
    return nullptr;
}

} // namespace pulp::platform::detail

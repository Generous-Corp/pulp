#pragma once

/// @file file_drag.hpp
/// Outbound file drags: drag existing files out of an editor into Finder, a
/// DAW timeline or any other drop target.
///
/// Call begin_file_drag() while handling a pointer-down or pointer-drag
/// event; the operating system anchors the drag to that event and takes over
/// from there. The files must already exist (this is a file-URL drag, not a
/// file promise). When the drag ends, `on_end` reports what the destination
/// did, once, on the main thread, unless the MainThreadScope is gone by then.
///
/// A recorder installed with set_file_drag_recorder() replaces the operating
/// system's drag, so tests and automation can start a drag without a real
/// pointer event and observe exactly what would have been dragged.

#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/native_handle.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulp::platform {

/// Operations a drag offers its destination (a bit set).
enum DragOperationBits : unsigned {
    drag_copy = 1u,
    drag_move = 2u,
};

/// What the destination did with the files.
enum class DragOperation { none, copy, move };

/// Short stable name ("none", "copy", "move").
const char* to_string(DragOperation operation) noexcept;

/// The picture under the pointer during the drag. Without one the files'
/// icons are used.
struct DragImage {
    int width = 0;  ///< pixels
    int height = 0; ///< pixels
    /// Premultiplied RGBA, row-major, width * height * 4 bytes.
    std::vector<std::uint8_t> rgba;
    /// Pixels per point.
    float scale = 2.0f;
    /// The point of the image held under the pointer, in points from its
    /// top-left corner.
    float hotspot_x = 0, hotspot_y = 0;
};

struct FileDragRequest {
    std::vector<std::string> paths;
    unsigned allowed = drag_copy;
    std::optional<DragImage> image;
};

enum class FileDragError {
    none,
    /// The request named no files.
    no_paths,
    /// None of the named files exists.
    missing_file,
    /// The triggering event is not a mouse-down or mouse-drag in the view's
    /// window, so the system has nothing to anchor the drag to.
    not_in_drag_event,
    /// The view is not in a window.
    no_window,
    /// No view, an image that does not match its size, or no native drag on
    /// this platform.
    unavailable,
};

/// Short stable name ("none", "no_paths", ...).
const char* to_string(FileDragError error) noexcept;

struct FileDragEnd {
    DragOperation operation = DragOperation::none;
    /// Where the drag ended, in screen points (top-left origin of the main
    /// screen); zero for a recorded drag.
    float screen_x = 0, screen_y = 0;
};

/// Start dragging `request.paths` (those that exist) out of `view` (an
/// NSView* on macOS). `triggering_event` is the NSEvent* being handled, or
/// nullptr for the application's current event. Returns none when the drag
/// started (or was recorded). Main thread only.
FileDragError begin_file_drag(MainThreadScope& scope, NativeViewHandle view,
                              const FileDragRequest& request,
                              std::function<void(FileDragEnd)> on_end,
                              void* triggering_event = nullptr);

/// While set, begin_file_drag() records the request (with only the paths that
/// exist) instead of starting a system drag, needs no pointer event or
/// window, and reports the operation the recorder returns. An empty function
/// restores system drags.
using FileDragRecorder = std::function<DragOperation(const FileDragRequest& request)>;
void set_file_drag_recorder(FileDragRecorder recorder);

/// The system drag-operation mask (macOS NSDragOperation bits) offered for
/// `allowed`.
unsigned native_drag_mask(unsigned allowed) noexcept;
/// The operation a system drag-operation value reports. A drop on the Trash
/// (delete) counts as a move.
DragOperation drag_operation_from_native(unsigned native_operation) noexcept;

namespace detail {
/// macOS: whether `event` (an NSEvent*) can anchor a drag from `view`: a
/// mouse-down or mouse-drag in the view's window.
bool drag_event_anchors(NativeViewHandle view, void* event);
/// macOS: an NSImage (autoreleased) for `image`, sized in points; nullptr when
/// the pixel buffer does not match its size.
void* make_drag_image(const DragImage& image);
/// macOS: a drag source (retained, +1) offering native_drag_mask(allowed) and
/// calling `on_end` once when its session ends. Exposed for tests; the source
/// releases itself when the session ends.
void* make_file_drag_source(unsigned allowed, std::function<void(FileDragEnd)> on_end);
} // namespace detail

} // namespace pulp::platform

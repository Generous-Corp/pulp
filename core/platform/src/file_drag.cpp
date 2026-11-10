// Outbound file drags: request validation, the recorder seam and the
// operation mapping. The system drag lives in platform/mac/file_drag_mac.mm.

#include <pulp/platform/file_drag.hpp>

#include <filesystem>
#include <mutex>
#include <utility>

namespace pulp::platform {

namespace detail {
// Platform half (file_drag_mac.mm; a stub elsewhere). `request.paths` holds
// only existing files.
FileDragError begin_native_file_drag(MainThreadScope::Handle handle, NativeViewHandle view,
                                     const FileDragRequest& request,
                                     std::function<void(FileDragEnd)> on_end,
                                     void* triggering_event);
} // namespace detail

namespace {

// NSDragOperation values; the macOS backend checks them against AppKit.
constexpr unsigned kNativeCopy = 1u;
constexpr unsigned kNativeMove = 16u;
constexpr unsigned kNativeDelete = 32u;

std::mutex g_recorder_mutex;
FileDragRecorder g_recorder;

} // namespace

const char* to_string(DragOperation operation) noexcept {
    switch (operation) {
    case DragOperation::none:
        return "none";
    case DragOperation::copy:
        return "copy";
    case DragOperation::move:
        return "move";
    }
    return "unknown";
}

const char* to_string(FileDragError error) noexcept {
    switch (error) {
    case FileDragError::none:
        return "none";
    case FileDragError::no_paths:
        return "no_paths";
    case FileDragError::missing_file:
        return "missing_file";
    case FileDragError::not_in_drag_event:
        return "not_in_drag_event";
    case FileDragError::no_window:
        return "no_window";
    case FileDragError::unavailable:
        return "unavailable";
    }
    return "unknown";
}

unsigned native_drag_mask(unsigned allowed) noexcept {
    unsigned mask = 0;
    if (allowed & drag_copy)
        mask |= kNativeCopy;
    if (allowed & drag_move)
        mask |= kNativeMove;
    return mask;
}

DragOperation drag_operation_from_native(unsigned native_operation) noexcept {
    if (native_operation & (kNativeMove | kNativeDelete))
        return DragOperation::move;
    if (native_operation & kNativeCopy)
        return DragOperation::copy;
    return DragOperation::none;
}

void set_file_drag_recorder(FileDragRecorder recorder) {
    std::lock_guard<std::mutex> lock(g_recorder_mutex);
    g_recorder = std::move(recorder);
}

FileDragError begin_file_drag(MainThreadScope& scope, NativeViewHandle view,
                              const FileDragRequest& request,
                              std::function<void(FileDragEnd)> on_end, void* triggering_event) {
    if (request.paths.empty())
        return FileDragError::no_paths;
    FileDragRequest existing = request;
    existing.paths.clear();
    for (const auto& path : request.paths) {
        std::error_code ec;
        if (!path.empty() && std::filesystem::exists(path, ec))
            existing.paths.push_back(path);
    }
    if (existing.paths.empty())
        return FileDragError::missing_file;
    if (existing.image) {
        const auto& image = *existing.image;
        if (image.width <= 0 || image.height <= 0 || image.scale <= 0 ||
            image.rgba.size() !=
                static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u)
            return FileDragError::unavailable;
    }

    FileDragRecorder recorder;
    {
        std::lock_guard<std::mutex> lock(g_recorder_mutex);
        recorder = g_recorder;
    }
    MainThreadScope::Handle handle = scope.handle();
    if (recorder) {
        const DragOperation operation = recorder(existing);
        handle.post([on_end = std::move(on_end), operation] {
            if (on_end)
                on_end(FileDragEnd{operation, 0, 0});
        });
        return FileDragError::none;
    }
    return detail::begin_native_file_drag(handle, view, existing, std::move(on_end),
                                          triggering_event);
}

} // namespace pulp::platform

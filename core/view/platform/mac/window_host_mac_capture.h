// window_host_mac_capture.h — private forward declarations for the PNG /
// capture helpers used by window_host_mac.mm.
//
// The implementations live in window_host_mac_capture.mm. This header is only
// consumed by window_host_mac.mm and is not part of the public SDK surface.
//
// Only free functions that don't touch PulpView ivars live here. The PulpView
// @implementation block and its ivars stay in window_host_mac.mm.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef __OBJC__
// Per-binary-unique ObjC class names (renames PulpView when a shipped binary
// defines PULP_VIEW_OBJC_SUFFIX); keeps the capture helpers in sync with the
// PulpView @implementation in window_host_mac.mm.
#include "pulp_mac_objc_names.h"

@class NSBitmapImageRep;
@class NSData;
@class NSView;
@class NSWindow;

namespace pulp::view::mac_capture {

// Copy raw bytes out of an NSData blob.
std::vector<uint8_t> nsdata_to_bytes(NSData* data);

// PNG-encode an NSBitmapImageRep via Cocoa's NSBitmapImageFileTypePNG.
std::vector<uint8_t> bitmap_rep_to_png(NSBitmapImageRep* rep);

// PNG-encode a tightly packed RGBA pixel buffer via CGImage +
// NSBitmapImageRep. `row_bytes` may exceed `pixel_w * 4` for padded
// scanlines.
std::vector<uint8_t> encode_rgba_to_png(const uint8_t* pixels,
                                        uint32_t pixel_w,
                                        uint32_t pixel_h,
                                        size_t row_bytes);

// Capture the current display content of a NSView via Cocoa's
// `bitmapImageRepForCachingDisplayInRect:` + `cacheDisplayInRect:`.
// Used by the headless WindowHost::capture_png() path.
std::vector<uint8_t> capture_view_cache_png(NSView* view);

// Force-display the window's contentView then capture it. Mirrors what
// a developer sees on screen, without the screencapture(1) shell-out.
std::vector<uint8_t> capture_window_content_png(NSWindow* window, NSView* contentView);

// Fallback path: shell out to `/usr/sbin/screencapture -l<windowNumber>`,
// writing to a temp file and reading the PNG back. Used when the
// in-process cache path returns empty (offscreen / unmapped windows).
std::vector<uint8_t> capture_window_screencapture_png(NSWindow* window);

}  // namespace pulp::view::mac_capture

#endif  // __OBJC__

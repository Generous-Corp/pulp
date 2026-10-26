#pragma once

// Whether this translation unit compiles the macOS GPU hosts:
// MacGpuPluginViewHost, MacGpuWindowHost, their Metal-backed NSViews and the
// categories on them. They need Skia, and a binary built with the lite editor
// stack (PULP_VIEW_LITE_HOST, set on its per-binary ObjC cluster) leaves them
// out so the linker can drop Skia, Dawn and Tint from it entirely; that
// binary's editors open on the CoreGraphics host.
//
// Only function bodies and whole GPU-only classes sit behind this switch,
// never a declaration another translation unit can see, so a lite cluster and
// a full library object agree on every shared type.

#if defined(PULP_HAS_SKIA) && !(defined(PULP_VIEW_LITE_HOST) && PULP_VIEW_LITE_HOST)
#define PULP_VIEW_MAC_GPU_HOST 1
#else
#define PULP_VIEW_MAC_GPU_HOST 0
#endif

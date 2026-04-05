---
name: design
description: AI-driven design session — describe a look, transform the UI
---

Start an AI-driven design session. The user describes a visual style in natural language and the design tool transforms the plugin UI.

If $ARGUMENTS is provided, use it as the style description.

Workflow:
1. Build the design tool: `cmake --build build --target pulp-design-tool -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)`
2. Launch: `./build/tools/cli/pulp design`
3. The user describes a look ("80s Macintosh", "neon cyberpunk", "minimal Dieter Rams")
4. The design system transforms: colors, widget shapes, shadows, typography
5. Changes are visible immediately via hot-reload

For headless/automated design iteration, use `pulp design-debug` which captures before/after screenshots and diffs.

Design tokens export to JSON, CSS variables, C++ headers, GPU shader uniforms, and OKLCH color systems via `pulp export-tokens`.

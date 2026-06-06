"""Pinned visual-harness dependency metadata.

These constants mirror ``tools/deps/manifest.json`` so tests and tooling can
fail early when the deterministic rendering stack drifts without an explicit
manifest update.

Skia is pinned at chrome/m150 via the danielraffel/skia-builder fork
(see manifest.json determinism.skia_builder_fork). The fork tracks
upstream olilarkin's tag pattern and adds iOS/visionOS/mac-x86_64
slices upstream does not. ``SKIA_COMMIT`` and ``SKIA_BUILDER_REF`` are
intentionally omitted on the m150 manifest entry — the fork tracks
branch HEAD rather than a specific commit, and the test fixtures
match the structured fields actually present in manifest.json.
"""

from __future__ import annotations

SKIA_BRANCH = "chrome/m150"
SKIA_BUILDER_FORK = "https://github.com/danielraffel/skia-builder"
SKIA_PYTHON_SMOKE_VERSION = "144.0.post2"

FONT_SHA256 = {
    "external/fonts/Inter-Regular.ttf": (
        "40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82"
    ),
    "external/fonts/JetBrainsMono-Regular.ttf": (
        "a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f"
    ),
}

RELEASE_ASSET_SHA256 = {
    # chrome/m150 dropped the linux-arm64 slice; Linux arm64 stays on the
    # m149 asset or rebuilds from source until the fork republishes it.
    "linux-x64": (
        "bb4d3a868a72560b25e467952bb7792d4af2bc9dcab1f77afca208b7b1f0d07b"
    ),
    "mac-arm64": (
        "13b0e9818c3b05db661af85cb1e2bf2ef10e30d468b81351dd90295237d17734"
    ),
    "mac-universal": (
        "f27908b847a6a130828073f65a02d052bb1672c999bc9d26384348719c315035"
    ),
}

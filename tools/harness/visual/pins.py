"""Pinned visual-harness dependency metadata.

These constants mirror ``tools/deps/manifest.json`` so tests and tooling can
fail early when the deterministic rendering stack drifts without an explicit
manifest update.

Skia is pinned at chrome/m149 via the danielraffel/skia-builder fork
(see manifest.json determinism.skia_builder_fork). The fork tracks
upstream olilarkin's tag pattern and adds iOS/visionOS/mac-x86_64
slices upstream does not. ``SKIA_COMMIT`` and ``SKIA_BUILDER_REF`` are
intentionally omitted on the m149 manifest entry — the fork tracks
branch HEAD rather than a specific commit, and the test fixtures
match the structured fields actually present in manifest.json.
"""

from __future__ import annotations

SKIA_BRANCH = "chrome/m149"
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
    "linux-x64": (
        "53e2bfb5225148311da9bbcb7e65da4479acf774bc3d40b0341530cdc48e97b6"
    ),
    "mac-arm64": (
        "774f5df966cd7133d05ce217eb3ed7bb226246ac336f764d7409350f175437f7"
    ),
    "mac-universal": (
        "416c5872296bd69f307cd279a3125e6574b86ef9effbb10adc31203781e434aa"
    ),
}

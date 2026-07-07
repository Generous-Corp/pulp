"""Pinned visual-harness dependency metadata.

These constants mirror ``tools/deps/manifest.json`` so tests and tooling can
fail early when the deterministic rendering stack drifts without an explicit
manifest update.

Skia is pinned at chrome/m151 via the danielraffel/skia-builder fork
(see manifest.json determinism.skia_builder_fork). The fork tracks
upstream olilarkin's tag pattern and adds iOS/visionOS/mac-x86_64
slices upstream does not. ``SKIA_COMMIT`` and ``SKIA_BUILDER_REF`` are
intentionally omitted on the m151 manifest entry — the fork tracks
branch HEAD rather than a specific commit, and the test fixtures
match the structured fields actually present in manifest.json.
"""

from __future__ import annotations

SKIA_BRANCH = "chrome/m151"
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
    # The chrome/m151 release ships linux-arm64 alongside the other slices; must
    # stay in lockstep with manifest.json (guarded by
    # test_skia_linux_arm64_asset.py).
    "linux-arm64": (
        "2420eed074e041384973338f9d8a41364b9ff444ffa0eb1857cb1ebdbd8781e9"
    ),
    "linux-x64": (
        "68315e29a8fd3848ab05225b18355b5fe8c85f6e7df3575087ff02d8ce17a56a"
    ),
    "mac-arm64": (
        "229a0822c1bd9103abbf51eb7baf5fd22141ed7b023199fd9417d8f6d13b0b0e"
    ),
    "mac-universal": (
        "2f8caa1cb805b8af6cd6a712d59a8b845f6bf981a35063608de7be8e955bc302"
    ),
}

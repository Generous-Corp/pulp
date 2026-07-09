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
        "518b74ee7f0b245c209349023e58a2891a83a7ab776504d7d8a23d1e76fbf4de"
    ),
    # macOS slices are the chrome/m151-minos13 re-cut (minos 13.0 stamp).
    "mac-arm64": (
        "648250f9ee625f0c6c73c521b5a2de7cf46812b06aa2300e4bec8b2bb6d4081b"
    ),
    "mac-universal": (
        "284964fda380a2cc5ff4f885ae557ef04dab5987ebd94fc01354b95878ad85cf"
    ),
}

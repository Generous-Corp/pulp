"""Pinned visual-harness dependency metadata.

These constants mirror ``tools/deps/manifest.json`` so tests and tooling can
fail early when the deterministic rendering stack drifts without an explicit
manifest update.

Skia is pinned at chrome/m152 via the danielraffel/skia-builder fork
(see manifest.json determinism.skia_builder_fork). The fork tracks
upstream olilarkin's tag pattern and adds iOS/visionOS/mac-x86_64
slices upstream does not. ``SKIA_COMMIT`` and ``SKIA_BUILDER_REF`` are
intentionally omitted on the m152 manifest entry — the fork tracks
branch HEAD rather than a specific commit, and the test fixtures
match the structured fields actually present in manifest.json.
"""

from __future__ import annotations

SKIA_BRANCH = "chrome/m152"
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
    # The chrome/m152 release ships linux-arm64 alongside the other slices; must
    # stay in lockstep with manifest.json (guarded by
    # test_skia_linux_arm64_asset.py).
    "linux-arm64": (
        "12aa2ba8a43472461dd552f7ac28420137bd6a3175542563c3bbbf06124d7df6"
    ),
    "linux-x64": (
        "b0114b0edd1e07d274fd37b8fb3508966590b9dda1fdd1f3ab24441c12dee4ed"
    ),
    # The mac-arm64 key selects the verified universal archive: its arm64 Dawn
    # slice retains the 13.0 deployment target, while the standalone M152
    # mac-arm64 archive leaked a 15.0 stamp.
    "mac-arm64": (
        "a066fd95d447fe00aa9890ae404fda1fb1db369006b1c705b401c8605f8ae244"
    ),
    "mac-universal": (
        "a066fd95d447fe00aa9890ae404fda1fb1db369006b1c705b401c8605f8ae244"
    ),
}

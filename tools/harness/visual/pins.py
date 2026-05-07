"""Pinned visual-harness dependency metadata.

These constants mirror ``tools/deps/manifest.json`` so tests and tooling can
fail early when the deterministic rendering stack drifts without an explicit
manifest update.
"""

from __future__ import annotations

SKIA_BRANCH = "chrome/m144"
SKIA_COMMIT = "cd0c5f445516ea4e90e02b5f634cbc5ca23b5a44"
SKIA_BUILDER_REF = "7eecb8abf1f77b2a8bac2e81c38e20708cb79c24"
SKIA_PYTHON_SMOKE_VERSION = "144.0.post2"

BUNDLED_PINS = {
    "dawn": "6acf6ef3fe237cd4be7b825389602c93a1f16f2f",
    "harfbuzz": "08b52ae2e44931eef163dbad71697f911fadc323",
    "icu": "364118a1d9da24bb5b770ac3d762ac144d6da5a4",
}

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
        "bde9977efb2c796e22a71b0086e2b15961b0ccc7dc4de8559b369229e922ce34"
    ),
    "mac-arm64": (
        "d495bc4b77fba29e055d9275852401d02998dd5fda0e628050fb72c4c9fce87a"
    ),
    "mac-universal": (
        "7990695504abf0e4b5b05d42616812845d9e72d41b243f347f261ab5fe8cb478"
    ),
}

"""Pinned visual-harness dependency metadata.

These constants mirror ``tools/deps/manifest.json`` so tests and tooling can
fail early when the deterministic rendering stack drifts without an explicit
manifest update.

Skia is pinned at chrome/m158 via the danielraffel/skia-builder fork
(see manifest.json determinism.skia_builder_fork). The fork tracks
upstream olilarkin's tag pattern and adds iOS/visionOS/mac-x86_64
slices upstream does not. ``SKIA_COMMIT`` and ``SKIA_BUILDER_REF`` bind the
published archive to the exact Skia source and skia-builder workflow revision.
"""

from __future__ import annotations

SKIA_BRANCH = "chrome/m158"
SKIA_COMMIT = "be7d7c3e996e567eda7dc9774274f7a0103cdb8c"
SKIA_BUILDER_REF = "312f05e24a5d5a45d4a3e70ca70ff3ccff1b4d97"
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
    # The chrome/m158 release ships linux-arm64 alongside the other slices; must
    # stay in lockstep with manifest.json (guarded by
    # test_skia_linux_arm64_asset.py).
    "linux-arm64": (
        "053833562a4ec4eeb97d71515fdf437cad7b6e8d668ca7e7d8b486f65528aaf9"
    ),
    "linux-x64": (
        "74c31d020cd18f1f9e544b1d8440bcc293e82982e50a9777a3940d84e2ccd666"
    ),
    # The mac-arm64 key selects the verified universal archive: its arm64 Dawn
    # slice retains the verified 13.0 deployment target.
    "mac-arm64": (
        "dca73c0eee5b0f6d129fe357d0754f7165c6b436051e0de947aeedd3e1bde250"
    ),
    "mac-universal": (
        "dca73c0eee5b0f6d129fe357d0754f7165c6b436051e0de947aeedd3e1bde250"
    ),
}


# Recorded identity of the committed raster goldens, keyed by fixture id. The
# byte-for-byte golden lives beside its fixture; this digest is the reviewable
# copy of that identity, so a golden rewritten by mistake shows up as a digest
# change in the diff rather than as an opaque binary blob.
#
# Produced by the pinned skia-python wheel (SKIA_PYTHON_SMOKE_VERSION) rendering
# the fixture's declared ops. A different pin renders different bytes, which is
# why the loader refuses to render with an unpinned Skia.
RASTER_GOLDEN_SHA256 = {
    "canvas2d/raster-determinism": (
        "9d6b81079fbbb5ac26fe31d4af8d0b011da661fda4a894b9717f38d32c27add8"
    ),
}

# Hosts on which the digests above have actually been observed, keyed the way
# raster.platform_key() reports them.
#
# darwin-arm64 and darwin-x86_64 were both measured directly: the same pinned
# wheel, rendering the same fixture, in separate processes, produced identical
# bytes on each. That settles the instruction-set axis on Apple silicon (the
# x86_64 reading was taken under Rosetta translation).
#
# linux-x86_64 is deliberately absent. No Linux execution path existed on the
# host where the golden was recorded, so cross-operating-system byte identity is
# UNPROVEN: fontconfig/CoreText, libc, and the wheel's own build differ between
# the two, and any of them could change the raster. The Linux lane of the visual
# harness compares against the same golden and reports its computed digest, so
# the first Linux run settles the question in one direction or the other.
RASTER_GOLDEN_VERIFIED_PLATFORMS = ("darwin-arm64", "darwin-x86_64")

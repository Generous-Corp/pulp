#!/usr/bin/env python3
"""Tests for tools/ci/dependency_pins.py: naming the dependencies a pin-file
change moved on one platform, and refusing to name them when it cannot."""
from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dependency_pins as dp  # noqa: E402

FIXTURES = HERE / "fixtures" / "dependency_pins"
MAP = dp.load_map()


def real(name: str) -> str:
    """The real copies of the Windows ARM64 Skia change (a9bf2db9): the whole
    PulpDependencies.cmake on each side and that side's Skia and Yoga
    manifest entries."""
    return (FIXTURES / name).read_text(encoding="utf-8")


def sides(manifest: tuple[str | None, str | None] | None = None,
          cmake: tuple[str | None, str | None] | None = None,
          fetch: tuple[str | None, str | None] | None = None) -> tuple[dict, dict]:
    base, head = {}, {}
    for path, pair in ((dp.MANIFEST, manifest), (dp.DEPENDENCIES_CMAKE, cmake), (dp.FETCHCONTENT_CMAKE, fetch)):
        if pair is not None:
            base[path], head[path] = pair
    return base, head


def edit_manifest(text: str, edit) -> str:
    doc = json.loads(text)
    edit({e["name"]: e for e in doc["dependencies"]})
    return json.dumps(doc, indent=1)


class RealChangeTests(unittest.TestCase):
    def test_the_windows_arm64_skia_change_moves_nothing_on_darwin(self):
        base, head = sides((real("manifest.base.json"), real("manifest.head.json")),
                           (real("PulpDependencies.base.cmake.txt"), real("PulpDependencies.head.cmake.txt")))
        self.assertNotEqual(base, head)
        self.assertEqual(dp.attribute(base, head, "darwin", MAP), dp.Pins("names"))
        self.assertEqual(dp.attribute(base, head, "linux", MAP), dp.Pins("names"))

    def test_the_same_change_moves_skia_where_the_branch_is_taken(self):
        base, head = sides((real("manifest.base.json"), real("manifest.head.json")),
                           (real("PulpDependencies.base.cmake.txt"), real("PulpDependencies.head.cmake.txt")))
        # No truth table for windows: the WIN32 arm stays and its change counts.
        self.assertEqual(dp.attribute(base, head, "windows", MAP), dp.Pins("names", frozenset({"Skia"})))
        manifest_only = sides((real("manifest.base.json"), real("manifest.head.json")))
        self.assertEqual(dp.attribute(*manifest_only, "windows", MAP), dp.Pins("names", frozenset({"Skia"})))

    def test_a_mac_skia_asset_moves_skia_on_darwin_only(self):
        def bump(entries):
            entries["Skia"]["determinism"]["release_assets"]["mac-arm64"]["sha256"] = "0" * 64
        head = edit_manifest(real("manifest.head.json"), bump)
        base_head = sides((real("manifest.head.json"), head))
        self.assertEqual(dp.attribute(*base_head, "darwin", MAP), dp.Pins("names", frozenset({"Skia"})))
        self.assertEqual(dp.attribute(*base_head, "linux", MAP), dp.Pins("names"))

    def test_a_skia_change_outside_its_assets_moves_skia_everywhere(self):
        def bump(entries):
            entries["Skia"]["determinism"]["skia_commit"] = "f" * 40
        pair = sides((real("manifest.head.json"), edit_manifest(real("manifest.head.json"), bump)))
        for platform in ("darwin", "linux", "windows"):
            self.assertEqual(dp.attribute(*pair, platform, MAP), dp.Pins("names", frozenset({"Skia"})))


class ManifestTests(unittest.TestCase):
    def test_documentation_fields_move_nothing(self):
        def doc(entries):
            entries["Yoga"]["notes"] = "changed"
            entries["Yoga"]["source_files"] = ["x"]
            entries["Yoga"]["license"] = "x"
        pair = sides((real("manifest.head.json"), edit_manifest(real("manifest.head.json"), doc)))
        self.assertEqual(dp.attribute(*pair, "darwin", MAP), dp.Pins("names"))

    def test_a_version_moves_its_name(self):
        def bump(entries):
            entries["Yoga"]["version"] = "v9"
        pair = sides((real("manifest.head.json"), edit_manifest(real("manifest.head.json"), bump)))
        self.assertEqual(dp.attribute(*pair, "darwin", MAP), dp.Pins("names", frozenset({"Yoga"})))

    def test_an_unparseable_or_reshaped_manifest_moves_all(self):
        head = real("manifest.head.json")
        for bad in (head[:-20], head.replace('"dependencies"', '"deps"'), json.dumps({"dependencies": [], "x": 1}),
                    json.dumps({"dependencies": [{"name": "Yoga"}, {"name": "Yoga"}]}), None):
            with self.subTest(bad=(bad or "absent")[:40]):
                pins = dp.attribute(*sides((head, bad)), "darwin", MAP)
                self.assertEqual(pins.scope, "all")
                self.assertTrue(pins.why)

    def test_a_name_the_map_does_not_know_moves_all(self):
        def add(entries):
            entries["Yoga"]["name"] = "Unmapped"
        pins = dp.attribute(*sides((real("manifest.head.json"),
                                    edit_manifest(real("manifest.head.json"), add))), "darwin", MAP)
        self.assertEqual((pins.scope, pins.why), ("all", "no target mapping for Unmapped"))


CMAKE = """# Header
include(Shared.cmake)

# Yoga
pulp_register_fetchcontent_source(yoga REF v1)
FetchContent_Declare(yoga GIT_TAG v1)

# Skia
option(PULP_SKIA_AUTOFETCH "x" ON)
if(APPLE)
    set(_plat "darwin")
elseif(WIN32)
    set(_plat "windows")
endif()
if(PULP_ENABLE_GPU)
    if(WIN32)
        set(_lib "skia.lib")
    else()
        set(_lib "libskia.a")
    endif()
endif()

# Guard
if(PULP_ENABLE_GPU AND NOT PULP_HAS_SKIA)
    message(WARNING "no skia")
endif()
"""


class CMakeTests(unittest.TestCase):
    def moved(self, head: str, platform: str = "darwin") -> dp.Pins:
        return dp.attribute(*sides(cmake=(CMAKE, head)), platform, MAP)

    def test_blocks_follow_comment_headers_at_top_level(self):
        starts = [b[0].text for b in dp.blocks(CMAKE, "x")]
        self.assertEqual(starts, ["include(Shared.cmake)", "pulp_register_fetchcontent_source(yoga REF v1)",
                                  'option(PULP_SKIA_AUTOFETCH "x" ON)', "if(PULP_ENABLE_GPU AND NOT PULP_HAS_SKIA)"])

    def test_a_pin_in_a_block_moves_its_name(self):
        self.assertEqual(self.moved(CMAKE.replace("GIT_TAG v1", "GIT_TAG v2")), dp.Pins("names", frozenset({"Yoga"})))

    def test_comments_and_layout_move_nothing(self):
        head = CMAKE.replace("# Yoga\n", "# Yoga, the layout engine\n").replace(
            "FetchContent_Declare(yoga GIT_TAG v1)", "FetchContent_Declare(\n    yoga\n    GIT_TAG v1  # pinned\n)")
        self.assertEqual(self.moved(head), dp.Pins("names"))

    def test_another_platforms_branch_moves_nothing_here(self):
        for old, new in (('set(_plat "windows")', 'set(_plat "windows-arm64")'),
                         ('set(_lib "skia.lib")', 'set(_lib "skia-arm64.lib")')):
            head = CMAKE.replace(old, new)
            self.assertEqual(self.moved(head, "darwin"), dp.Pins("names"))
            self.assertEqual(self.moved(head, "linux"), dp.Pins("names"))
            self.assertEqual(self.moved(head, "windows"), dp.Pins("names", frozenset({"Skia"})))

    def test_this_platforms_branch_moves_its_name(self):
        for old, new in (('set(_lib "libskia.a")', 'set(_lib "libskia2.a")'),     # taken as else()
                         ('set(_plat "darwin")', 'set(_plat "darwin-arm64")')):   # taken as if(APPLE)
            with self.subTest(old=old):
                head = CMAKE.replace(old, new)
                self.assertEqual(self.moved(head, "darwin"), dp.Pins("names", frozenset({"Skia"})))
                self.assertEqual(self.moved(head, "linux"), dp.Pins("names", frozenset({"Skia"}) if "lib" in old
                                                                   else frozenset()))

    def test_an_undecidable_condition_keeps_every_branch(self):
        # A decided branch ends the chain; one undecidable condition before it
        # keeps every branch, so another platform's body then counts.
        base = CMAKE.replace("if(APPLE)", "if(APPLE AND PULP_FORCE)")
        head = base.replace('set(_plat "windows")', 'set(_plat "windows-arm64")')
        self.assertEqual(dp.attribute(*sides(cmake=(base, head)), "darwin", MAP), dp.Pins("names", frozenset({"Skia"})))
        decided = CMAKE.replace('set(_plat "windows")', 'set(_plat "windows-arm64")')
        self.assertEqual(self.moved(decided, "darwin"), dp.Pins("names"))

    def test_a_change_no_anchor_claims_moves_all(self):
        for head in (CMAKE.replace("include(Shared.cmake)", "include(Shared.cmake)\ninclude(More.cmake)"),
                     CMAKE.replace('message(WARNING "no skia")', 'message(FATAL_ERROR "no skia")'),
                     CMAKE + "\n# New\nset(X 1)\n"):
            pins = self.moved(head)
            self.assertEqual(pins.scope, "all")
            self.assertIn("outside any dependency's block", pins.why)

    def test_an_anchor_lost_or_gained_moves_all(self):
        head = CMAKE.replace("pulp_register_fetchcontent_source(yoga REF v1)\nFetchContent_Declare(yoga GIT_TAG v1)",
                             "set(Y 1)")
        self.assertEqual(self.moved(head).scope, "all")

    def test_unparseable_cmake_moves_all(self):
        for head in (CMAKE + "#[[ bracket ]]\n", CMAKE + "set(X [[y]])\n", CMAKE + "set(X\n", CMAKE + "@@\n"):
            with self.subTest(head=head[-14:]):
                self.assertEqual(self.moved(head).scope, "all")

    def test_shared_fetchcontent_logic_moves_all_but_its_comments_do_not(self):
        text = "function(f)\n    set(X 1)\nendfunction()\n"
        self.assertEqual(dp.attribute(*sides(fetch=(text, text.replace("X 1", "X 2"))), "darwin", MAP).scope, "all")
        self.assertEqual(dp.attribute(*sides(fetch=(text, "# note\n" + text)), "darwin", MAP), dp.Pins("names"))


def index(platform: str = "darwin", links: dict | None = None) -> dp.DependencyIndex:
    def t(kind, art, deps=()):
        return {"type": kind, "artifacts": [f"<build>/{art}"], "dependencies": list(deps)}
    targets = {"exe-yoga": t("EXECUTABLE", "test/exe-yoga", ["pulp-view"]), "exe-plain": t("EXECUTABLE", "test/plain"),
               "exe-skia": t("EXECUTABLE", "test/exe-skia"),
               "pulp-view": t("STATIC_LIBRARY", "core/libview.a", ["yogacore"]),
               "yogacore": t("STATIC_LIBRARY", "_deps/yoga-build/libyogacore.a"),
               "pulp-canvas": t("STATIC_LIBRARY", "core/libcanvas.a")}
    if links is None:
        links = {"<build>/test/exe-skia": {"archives": {"/Users/x/.cache/pulp/skia/darwin-arm64-1/libskia.a": {}}},
                 "<build>/test/exe-yoga": {"archives": {}}, "<build>/test/plain": {"archives": {}}}
    return dp.DependencyIndex(MAP, platform, targets, copy.deepcopy(targets), links)


class ClosureTests(unittest.TestCase):
    def test_a_dependency_reaches_through_the_codemodel_closure(self):
        idx = index()
        yoga = frozenset({"Yoga"})
        self.assertTrue(idx.reaches("test/exe-yoga", "exe-yoga", yoga))
        self.assertFalse(idx.reaches("test/plain", "exe-plain", yoga))
        self.assertFalse(idx.reaches("test/exe-skia", "exe-skia", yoga))

    def test_a_prebuilt_reaches_through_the_recorded_link(self):
        idx = index()
        skia = frozenset({"Skia"})
        self.assertTrue(idx.reaches("test/exe-skia", "exe-skia", skia))
        self.assertFalse(idx.reaches("test/exe-yoga", "exe-yoga", skia))

    def test_a_name_the_build_cannot_show_moves_all(self):
        # The positive control: no yoga target and no Skia archive anywhere.
        idx = index(links={})
        self.assertEqual(dp.resolve(dp.Pins("names", frozenset({"Skia"})), idx).scope, "all")
        self.assertEqual(dp.resolve(dp.Pins("names", frozenset({"Yoga"})), idx), dp.Pins("names", frozenset({"Yoga"})))
        blind = dp.resolve(dp.Pins("names", frozenset({"Highway"})), idx)
        self.assertEqual((blind.scope, blind.why), ("all", "the map finds no target or archive for Highway"))

    def test_a_name_not_built_on_this_platform_moves_nothing(self):
        self.assertEqual(dp.resolve(dp.Pins("names", frozenset({"Oboe"})), index()), dp.Pins("names"))
        self.assertEqual(dp.resolve(dp.Pins("names", frozenset({"Oboe"})), index("android")).scope, "all")

    def test_an_explicit_target_is_a_dependency_target(self):
        idx = index()
        self.assertEqual(idx.targets_of("WOFF2"), {"pulp-canvas"})


class MapTests(unittest.TestCase):
    def test_every_mapped_name_is_a_manifest_entry_and_can_be_found(self):
        manifest = json.loads((HERE.parents[1] / dp.MANIFEST).read_text(encoding="utf-8"))
        names = {e["name"] for e in manifest["dependencies"]}
        self.assertEqual(sorted(set(MAP) - names), [])
        for name, entry in MAP.items():
            with self.subTest(name=name):
                self.assertTrue(entry.get("fetchcontent") or entry.get("targets") or entry.get("archives"))
                self.assertEqual(set(entry) - {"fetchcontent", "targets", "archives", "anchors", "platforms"}, set())

    def test_every_anchor_claims_a_block_in_the_live_file(self):
        text = (HERE.parents[1] / dp.DEPENDENCIES_CMAKE).read_text(encoding="utf-8")
        owners = set()
        anchors = dp.anchors_of(MAP)
        for block in dp.blocks(text, dp.DEPENDENCIES_CMAKE):
            owners |= dp.owners(block, anchors)
        declared = {n for n, e in MAP.items() if e.get("anchors")}
        self.assertEqual(sorted(declared - owners), [])


if __name__ == "__main__":
    unittest.main()

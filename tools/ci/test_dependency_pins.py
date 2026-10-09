#!/usr/bin/env python3
"""Tests for tools/ci/dependency_pins.py: naming the dependencies a pin-file
change moved on one platform, and refusing to name them when it cannot."""
from __future__ import annotations

import copy
import gzip
import json
import sys
import tempfile
import unittest
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    tomllib = None

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

    def test_a_field_outside_the_documentation_allowlist_moves_its_name(self):
        def invent(entries):
            entries["Yoga"]["invented_build_field"] = "x"
        pair = sides((real("manifest.head.json"), edit_manifest(real("manifest.head.json"), invent)))
        self.assertEqual(dp.attribute(*pair, "darwin", MAP), dp.Pins("names", frozenset({"Yoga"})))

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

    def test_a_chain_on_a_non_platform_variable_is_kept_whole(self):
        # The negative control for the collapse: PULP_HAS_VST3 decides
        # nothing, so a change in any of its branches counts here.
        base = CMAKE.replace('option(PULP_SKIA_AUTOFETCH "x" ON)',
                             'option(PULP_SKIA_AUTOFETCH "x" ON)\nif(PULP_HAS_VST3)\n    set(V 1)\n'
                             'elseif(WIN32)\n    set(V 2)\nelse()\n    set(V 3)\nendif()')
        for old in ("set(V 1)", "set(V 2)", "set(V 3)"):
            with self.subTest(old=old):
                head = base.replace(old, old.replace(")", "0)"))
                self.assertEqual(dp.attribute(*sides(cmake=(base, head)), "darwin", MAP),
                                 dp.Pins("names", frozenset({"Skia"})))

    def test_a_not_arm_collapses_to_the_taken_arm_only(self):
        base = CMAKE.replace('option(PULP_SKIA_AUTOFETCH "x" ON)',
                             'option(PULP_SKIA_AUTOFETCH "x" ON)\nif(NOT WIN32)\n    set(N 1)\nelse()\n'
                             '    set(N 2)\nendif()')
        taken = base.replace("set(N 1)", "set(N 10)")
        untaken = base.replace("set(N 2)", "set(N 20)")
        self.assertEqual(dp.attribute(*sides(cmake=(base, taken)), "darwin", MAP), dp.Pins("names", frozenset({"Skia"})))
        self.assertEqual(dp.attribute(*sides(cmake=(base, untaken)), "darwin", MAP), dp.Pins("names"))
        self.assertEqual(dp.attribute(*sides(cmake=(base, untaken)), "windows", MAP),
                         dp.Pins("names", frozenset({"Skia"})))

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


def pointer(doc, path: str):
    for part in path.strip("/").split("/"):
        if not isinstance(doc, dict) or part not in doc:
            raise KeyError(path)
        doc = doc[part]
    return doc


class RealRecordTests(unittest.TestCase):
    """A record written before this rule, reduced to what the closure reads
    (tools/ci/fixtures/dependency_pins/record-11ca217e.json.gz). The rule
    changes how a record is read, never what one holds, so such a record
    stays bindable and is keyed by the finer rule."""

    @classmethod
    def setUpClass(cls):
        cls.record = json.loads(gzip.decompress((FIXTURES / "record-11ca217e.json.gz").read_bytes()))
        targets = cls.record["targets"]
        cls.index = dp.DependencyIndex(MAP, "darwin", targets, targets, cls.record["links"])
        cls.executables = {a.removeprefix("<build>/"): n for n, t in targets.items()
                           if t["type"] in ("EXECUTABLE", "MODULE_LIBRARY") for a in t["artifacts"]}

    def rekeyed(self, pins: dp.Pins) -> int:
        pins = dp.resolve(pins, self.index)
        if pins.scope == "all":
            return len(self.executables)
        return sum(self.index.reaches(a, t, pins.names) for a, t in self.executables.items())

    def test_the_windows_skia_change_rekeys_no_mac_executable_of_a_real_record(self):
        base, head = sides((real("manifest.base.json"), real("manifest.head.json")),
                           (real("PulpDependencies.base.cmake.txt"), real("PulpDependencies.head.cmake.txt")))
        self.assertEqual(self.rekeyed(dp.attribute(base, head, "darwin", MAP)), 0)
        # The control: a mac Skia asset reaches exactly the record's Skia linkers.
        def bump(entries):
            entries["Skia"]["determinism"]["release_assets"]["mac-arm64"]["sha256"] = "0" * 64
        mac = sides((real("manifest.head.json"), edit_manifest(real("manifest.head.json"), bump)))
        linkers = sum(any("/pulp/skia/" in a for a in rec["archives"])
                      for exe, rec in self.record["links"].items() if exe.removeprefix("<build>/") in self.executables)
        self.assertEqual(self.rekeyed(dp.attribute(*mac, "darwin", MAP)), linkers)
        self.assertGreater(linkers, 100)
        self.assertLess(linkers, len(self.executables))

    def test_an_sdk_ref_bump_rekeys_exactly_that_sdks_linkers_of_a_real_record(self):
        for old, new, name, target in (("build_66", "build_67", "VST3 SDK", "vst3-sdk"),
                                       ("AudioUnitSDK-1.3.0", "AudioUnitSDK-1.4.0", "AudioUnitSDK", "ausdk")):
            with self.subTest(name=name):
                pins = dp.attribute({dp.SETUP_SCRIPT: SETUP}, {dp.SETUP_SCRIPT: SETUP.replace(old, new)},
                                    "darwin", MAP)
                self.assertEqual(pins, dp.Pins("names", frozenset({name})))
                linkers = sum(target in self.index._deps(t) for t in self.executables.values())
                self.assertEqual(self.rekeyed(pins), linkers)
                self.assertGreater(linkers, 0)
                self.assertLess(linkers, len(self.executables))
        unrelated = dp.attribute({dp.SETUP_SCRIPT: SETUP}, {dp.SETUP_SCRIPT: SETUP.replace("echo done", "echo ok")},
                                 "darwin", MAP)
        self.assertEqual(self.rekeyed(unrelated), 0)

    @unittest.skipIf(tomllib is None, "tomllib unavailable; cannot read .shipyard/config.toml")
    def test_the_record_still_binds_under_this_base_record_policy(self):
        # rules_digest hashes this table; a fact added to strand older records
        # would fail here.
        with (HERE.parents[1] / ".shipyard" / "config.toml").open("rb") as handle:
            config = tomllib.load(handle)
        policy = config["targets"]["mac"]["changed_surface_selection"]["executable_reuse"]["base_record"]
        job = self.record["job"]
        self.assertEqual(pointer(job, policy["platform"]), "darwin-arm64")
        pointer(job, policy["toolchain"])
        for fact in policy["require"]:
            with self.subTest(fact=fact["pointer"]):
                value = pointer(job, fact["pointer"])
                if "equals" in fact:
                    self.assertEqual(value, fact["equals"])


ROOT = """project(x)

# Tests
if(PULP_BUILD_TESTS)
    FetchContent_Declare(Catch2 GIT_TAG v3.7.1)
    FetchContent_MakeAvailable(Catch2)
endif()

# Library
add_library(pulp-x STATIC x.cpp)
"""


class RootCMakeTests(unittest.TestCase):
    def test_a_root_fetchcontent_block_change_moves_all(self):
        pins = dp.attribute({dp.ROOT_CMAKE: ROOT}, {dp.ROOT_CMAKE: ROOT.replace("v3.7.1", "v3.8.0")}, "darwin", MAP)
        self.assertEqual((pins.scope, pins.why), ("all", "a FetchContent block in CMakeLists.txt changed"))

    def test_other_root_changes_move_nothing_through_the_pins(self):
        for head in (ROOT.replace("x.cpp", "y.cpp"), ROOT.replace("# Tests", "# Tests, Catch2")):
            self.assertEqual(dp.attribute({dp.ROOT_CMAKE: ROOT}, {dp.ROOT_CMAKE: head}, "darwin", MAP), dp.Pins("names"))

    def test_the_live_root_file_parses_and_has_a_fetchcontent_block(self):
        text = (HERE.parents[1] / dp.ROOT_CMAKE).read_text(encoding="utf-8")
        self.assertTrue(dp.fetchcontent_blocks(text, "darwin"))


SETUP = """#!/usr/bin/env bash
VST3_SDK_REF="v3.8.0_build_66"
ensure_shared_git_source_with_retry "VST3 SDK" "https://github.com/steinbergmedia/vst3sdk.git" \\
    "$VST3_SDK_REF" "$(fetchcontent_cache_dir_name "vst3sdk" "$VST3_SDK_REF")"
ensure_shared_git_source_with_retry "woff2" "https://github.com/google/woff2.git" \\
    "fb9c3379" "$(fetchcontent_cache_dir_name "woff2" "fb9c3379")"
WGPU_NATIVE_VERSION="v24.0.3.1"
ensure_shared_archive_source "wgpu-native runtime" \\
    "https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_NATIVE_VERSION}/x.zip"
if [ "$(uname)" = Darwin ]; then
    AU_SDK_REF="AudioUnitSDK-1.3.0"
fi
echo done
"""


class SetupScriptTests(unittest.TestCase):
    def moved(self, head: str) -> dp.Pins:
        return dp.attribute({dp.SETUP_SCRIPT: SETUP}, {dp.SETUP_SCRIPT: head}, "darwin", MAP)

    def test_a_ref_bump_moves_its_sdk(self):
        self.assertEqual(self.moved(SETUP.replace("v3.8.0_build_66", "v3.9.0_build_1")),
                         dp.Pins("names", frozenset({"VST3 SDK"})))
        self.assertEqual(self.moved(SETUP.replace("AudioUnitSDK-1.3.0", "AudioUnitSDK-1.4.0")),
                         dp.Pins("names", frozenset({"AudioUnitSDK"})))

    def test_a_clone_source_change_moves_its_dependency(self):
        fork = SETUP.replace("github.com/steinbergmedia/vst3sdk.git", "github.com/someone/vst3sdk.git")
        self.assertEqual(self.moved(fork), dp.Pins("names", frozenset({"VST3 SDK"})))
        # The clone's label is the manifest name, matched without case.
        self.assertEqual(self.moved(SETUP.replace('"fb9c3379"', '"fb9c3380"', 1)),
                         dp.Pins("names", frozenset({"WOFF2"})))

    def test_a_clone_no_dependency_claims_moves_all(self):
        for head in (SETUP.replace("v24.0.3.1", "v25.0.0"),  # a variable the unclaimed clone reads
                     SETUP + 'ensure_shared_git_source_with_retry "Unknown" "https://x/y.git" "1"\n'):
            with self.subTest(head=head[-60:]):
                pins = self.moved(head)
                self.assertEqual(pins.scope, "all")
                self.assertIn("which no dependency claims", pins.why)

    FETCH = """retry_git() {
    git "$@"
}

ensure_shared_git_source() {
    # clone into the shared cache
    retry_git clone "$2" "$4"
}

unrelated() {
    echo "$1"
}
""" + SETUP

    def test_a_change_to_the_shared_fetch_logic_moves_all(self):
        for old, new in (('retry_git clone "$2" "$4"', 'retry_git clone --mirror "$2" "$4"'),  # the clone function
                         ('    git "$@"', '    git -c http.proxy=x "$@"')):                   # a function it calls
            with self.subTest(new=new):
                pins = dp.attribute({dp.SETUP_SCRIPT: self.FETCH}, {dp.SETUP_SCRIPT: self.FETCH.replace(old, new)},
                                    "darwin", MAP)
                self.assertEqual((pins.scope, pins.why),
                                 ("all", "setup.sh changed the shared fetch logic every clone runs"))

    def test_comments_in_and_functions_outside_the_fetch_logic_move_nothing(self):
        for old, new in (("# clone into the shared cache", "# clone it"), ('echo "$1"', 'echo "$2"')):
            with self.subTest(new=new):
                self.assertEqual(dp.attribute({dp.SETUP_SCRIPT: self.FETCH},
                                              {dp.SETUP_SCRIPT: self.FETCH.replace(old, new)}, "darwin", MAP),
                                 dp.Pins("names"))

    def test_the_live_fetch_logic_holds_the_clone_functions(self):
        logic = dp._fetch_logic((HERE.parents[1] / dp.SETUP_SCRIPT).read_text(encoding="utf-8"))
        self.assertTrue({"ensure_shared_git_source", "ensure_shared_git_source_with_retry",
                         "ensure_shared_archive_source", "fetchcontent_cache_dir_name"} <= set(logic))

    def test_other_setup_lines_move_nothing(self):
        self.assertEqual(self.moved(SETUP.replace("echo done", "echo finished\nset -x")), dp.Pins("names"))

    def test_a_ref_no_dependency_claims_moves_all(self):
        pins = self.moved(SETUP + 'NEW_SDK_REF="1"\n')
        self.assertEqual((pins.scope, pins.why), ("all", "setup.sh changed NEW_SDK_REF, which no dependency claims"))

    def test_every_sdk_ref_in_the_live_script_is_claimed(self):
        text = (HERE.parents[1] / dp.SETUP_SCRIPT).read_text(encoding="utf-8")
        refs = {m.group(1) for m in dp.SDK_REF.finditer(text)}
        claimed = {r for e in MAP.values() for r in e.get("setup_refs") or []}
        self.assertTrue(refs)
        self.assertEqual(sorted(refs - claimed), [])


# Source kinds that never link into a C++ executable; every other kind must
# be mapped or listed as unmapped with a reason.
NON_LINKING_KINDS = frozenset({"npm", "python-pip", "transitive-python", "cargo", "release-asset"})


class MapTests(unittest.TestCase):
    def test_load_map_refuses_a_name_in_both_lists(self):
        good = {"schema": dp.MAP_SCHEMA, "dependencies": {"Yoga": {"fetchcontent": ["yoga"]}},
                "unmapped": {"CHOC": "header-only"}}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "map.json"
            path.write_text(json.dumps(good), encoding="utf-8")
            self.assertEqual(dp.load_map(path), good["dependencies"])
            for bad in ({**good, "unmapped": {"Yoga": "both"}}, {k: v for k, v in good.items() if k != "unmapped"},
                        {**good, "schema": "other"}):
                with self.subTest(bad=sorted(bad)):
                    path.write_text(json.dumps(bad), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        dp.load_map(path)

    def test_every_linking_dependency_is_mapped_or_listed_unmapped(self):
        manifest = json.loads((HERE.parents[1] / dp.MANIFEST).read_text(encoding="utf-8"))
        unmapped = dp.load_unmapped()
        silent = sorted(e["name"] for e in manifest["dependencies"]
                        if e["source_kind"] not in NON_LINKING_KINDS and e["name"] not in MAP
                        and e["name"] not in unmapped)
        self.assertEqual(silent, [])
        self.assertTrue(all(isinstance(r, str) and r for r in unmapped.values()))
        self.assertEqual(sorted(set(unmapped) - {e["name"] for e in manifest["dependencies"]}), [])
        # Unmapped means every executable at run time.
        pins = dp.attribute(*sides((json.dumps({"dependencies": [{"name": "CHOC", "version": "1"}]}),
                                    json.dumps({"dependencies": [{"name": "CHOC", "version": "2"}]}))), "darwin", MAP)
        self.assertEqual(pins.scope, "all")

    def test_every_mapped_name_is_a_manifest_entry_and_can_be_found(self):
        manifest = json.loads((HERE.parents[1] / dp.MANIFEST).read_text(encoding="utf-8"))
        names = {e["name"] for e in manifest["dependencies"]}
        self.assertEqual(sorted(set(MAP) - names), [])
        for name, entry in MAP.items():
            with self.subTest(name=name):
                self.assertTrue(entry.get("fetchcontent") or entry.get("targets") or entry.get("archives"))
                self.assertEqual(set(entry) - {"fetchcontent", "targets", "archives", "anchors", "platforms",
                                                       "setup_refs"}, set())

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

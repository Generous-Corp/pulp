"""Tests for adapter auto-discovery — pulp #1401.

The harness used to require editing ``verifier.py`` for every new surface
adapter, which serialised parallel work and forced sequential rebases on
sibling PRs (#1395 / #1396 / #1397 / #1398 / #1399). The fix replaces the
manual ``ADAPTERS = {...}`` registry with a decorator + ``pkgutil.iter_modules``
walk over ``tools/harness/adapters/``.

These tests pin down the three behaviours the issue's test plan calls out:

1. A new adapter file dropped into ``tools/harness/adapters/`` is picked up
   without ``verifier.py`` edits.
2. An adapter file that throws on import does NOT crash the verifier — it is
   logged and skipped.
3. The yoga adapter still classifies the same way after the refactor (no
   PASS / DIVERGE / NO-OP / NOT-IMPL / OOS regression vs. the baseline that
   landed in #1395).

Plus a couple of unit tests for the ``register_adapter`` decorator itself.

Invocation::

    python3 -m unittest tools.harness.tests.test_auto_discover

The tests are pure ``unittest`` — no pytest dependency — so they run on a
clean Python install with only the repo on ``sys.path``.
"""

from __future__ import annotations

import importlib
import logging
import sys
import textwrap
import unittest
from pathlib import Path

# Make the repo root importable when this file is run directly.
HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness import verifier  # noqa: E402
from tools.harness.adapters import base as adapters_base  # noqa: E402
from tools.harness.adapters.base import (  # noqa: E402
    AdapterBase,
    CatalogEntry,
    Result,
    register_adapter,
)
from tools.harness.status import Status  # noqa: E402


# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────


def _adapters_dir() -> Path:
    """Filesystem path to the adapters package."""
    import tools.harness.adapters as adapters_pkg

    return Path(next(iter(adapters_pkg.__path__)))


class _RegistrySnapshot:
    """Save & restore the adapter registry around a test that mutates it.

    Also clears any drop-in adapter modules from ``sys.modules`` so the
    next test session sees a clean slate.
    """

    def __init__(self) -> None:
        self._saved: dict[str, type] = {}
        self._drop_in_module_names: list[str] = []

    def __enter__(self) -> "_RegistrySnapshot":
        self._saved = dict(adapters_base.ADAPTERS)
        return self

    def track_module(self, name: str) -> None:
        self._drop_in_module_names.append(name)

    def __exit__(self, exc_type, exc, tb) -> None:
        # Restore the registry exactly as we found it.
        adapters_base.ADAPTERS.clear()
        adapters_base.ADAPTERS.update(self._saved)
        # Evict any temp modules we imported so subsequent imports re-run.
        for mod_name in self._drop_in_module_names:
            sys.modules.pop(mod_name, None)


# ─────────────────────────────────────────────────────────────────────────────
# Decorator unit tests
# ─────────────────────────────────────────────────────────────────────────────


class RegisterAdapterDecoratorTests(unittest.TestCase):
    def test_decorator_registers_class_and_stamps_surface(self) -> None:
        with _RegistrySnapshot():

            @register_adapter("__test_decorator__")
            class _TmpAdapter(AdapterBase):
                pass

            self.assertIs(adapters_base.ADAPTERS["__test_decorator__"], _TmpAdapter)
            self.assertEqual(_TmpAdapter.surface, "__test_decorator__")

    def test_decorator_returns_class_unchanged(self) -> None:
        with _RegistrySnapshot():

            @register_adapter("__test_returns_cls__")
            class _TmpAdapter(AdapterBase):
                pass

            # Decorator must still return the class so users can `from foo import Bar`.
            self.assertTrue(issubclass(_TmpAdapter, AdapterBase))

    def test_decorator_rejects_empty_name(self) -> None:
        with self.assertRaises(ValueError):
            register_adapter("")
        with self.assertRaises(ValueError):
            register_adapter(None)  # type: ignore[arg-type]

    def test_re_registration_overrides_previous(self) -> None:
        with _RegistrySnapshot():

            @register_adapter("__rebind__")
            class A(AdapterBase):
                pass

            @register_adapter("__rebind__")
            class B(AdapterBase):
                pass

            self.assertIs(adapters_base.ADAPTERS["__rebind__"], B)


# ─────────────────────────────────────────────────────────────────────────────
# Drop-in pickup
# ─────────────────────────────────────────────────────────────────────────────


class DropInAdapterPickupTests(unittest.TestCase):
    """A new file in ``adapters/`` is auto-discovered without verifier edits."""

    SURFACE_NAME = "__test_dropin_surface__"
    # Module name MUST NOT start with "_" or it will be skipped by the
    # discovery walk's underscore guard.
    MODULE_BASENAME = "test_dropin_adapter_1401"

    def setUp(self) -> None:
        self.adapters_dir = _adapters_dir()
        self.module_path = self.adapters_dir / f"{self.MODULE_BASENAME}.py"
        # Module is intentionally NOT prefixed with ``_`` so the discovery
        # walk picks it up. (The skip rule covers ``_<name>``, ``base``.)
        # We use a unique dunder-style surface name instead to namespace it.
        # Drop the file.
        self.module_path.write_text(
            textwrap.dedent(
                f"""
                \"\"\"Test fixture for pulp #1401 — drop-in adapter pickup.\"\"\"

                from tools.harness.adapters.base import (
                    AdapterBase,
                    CatalogEntry,
                    Result,
                    register_adapter,
                )
                from tools.harness.status import Status


                @register_adapter("{self.SURFACE_NAME}")
                class DropInAdapter(AdapterBase):
                    surface = "{self.SURFACE_NAME}"

                    def run(self, entry):  # type: ignore[override]
                        return Result(entry=entry, status=Status.PASS, detail="dropin ok")
                """
            ).lstrip()
        )

    def tearDown(self) -> None:
        try:
            self.module_path.unlink()
        except FileNotFoundError:
            pass
        # Clean up the ADAPTERS entry the import side-effect created.
        adapters_base.ADAPTERS.pop(self.SURFACE_NAME, None)
        # Evict the imported module so a re-discover run picks it up fresh
        # next time it's needed.
        sys.modules.pop(
            f"tools.harness.adapters.{self.MODULE_BASENAME}", None
        )

    def test_new_adapter_is_picked_up_without_verifier_edits(self) -> None:
        # Sanity: the surface should NOT be in the registry before discovery
        # (we just created the file; nothing has imported it yet).
        adapters_base.ADAPTERS.pop(self.SURFACE_NAME, None)
        sys.modules.pop(
            f"tools.harness.adapters.{self.MODULE_BASENAME}", None
        )
        self.assertNotIn(self.SURFACE_NAME, adapters_base.ADAPTERS)

        # Calling _discover_adapters() should import the new file and the
        # decorator should land it in the registry.
        verifier._discover_adapters(reload=True)
        self.assertIn(
            self.SURFACE_NAME,
            adapters_base.ADAPTERS,
            "drop-in adapter file was not picked up by _discover_adapters()",
        )
        # The aliased re-export on verifier must reflect the same dict.
        self.assertIn(self.SURFACE_NAME, verifier.ADAPTERS)
        # ADAPTERS on verifier and in adapters_base should be the same object,
        # so callers that imported either symbol see the same entries.
        self.assertIs(verifier.ADAPTERS, adapters_base.ADAPTERS)


# ─────────────────────────────────────────────────────────────────────────────
# Broken adapter — must be logged + skipped, not crash
# ─────────────────────────────────────────────────────────────────────────────


class BrokenAdapterIsSkippedTests(unittest.TestCase):
    """A module that raises on import must NOT crash the verifier."""

    MODULE_BASENAME = "_test_broken_adapter_1401_will_raise"

    def setUp(self) -> None:
        self.adapters_dir = _adapters_dir()
        # IMPORTANT: we pick a name that does NOT start with "_" because the
        # discovery walk skips underscore-prefixed modules. A module that
        # starts with "_" would never be imported at all and would
        # silently bypass this regression. Strip the leading underscore.
        self.basename = self.MODULE_BASENAME.lstrip("_")
        self.module_path = self.adapters_dir / f"{self.basename}.py"
        self.module_path.write_text(
            textwrap.dedent(
                """
                \"\"\"Test fixture for pulp #1401 — broken adapter.

                This module raises ImportError on import to verify the
                discovery loop logs and skips it without crashing.
                \"\"\"

                raise ImportError("intentional failure for pulp #1401 test")
                """
            ).lstrip()
        )

    def tearDown(self) -> None:
        try:
            self.module_path.unlink()
        except FileNotFoundError:
            pass
        sys.modules.pop(f"tools.harness.adapters.{self.basename}", None)

    def test_broken_adapter_is_logged_and_skipped(self) -> None:
        # Capture WARNING records emitted by the verifier's logger.
        with self.assertLogs(verifier.logger, level=logging.WARNING) as cap:
            registry = verifier._discover_adapters(reload=True)

        # Expected outcomes:
        #   1. _discover_adapters returned the live registry (didn't raise).
        #   2. The yoga adapter is still present (broken sibling didn't
        #      poison the rest of the discovery walk).
        #   3. A warning log named the broken module.
        self.assertIs(registry, adapters_base.ADAPTERS)
        self.assertIn(
            "yoga",
            registry,
            "broken sibling adapter caused yoga to drop out of the registry",
        )
        joined = "\n".join(cap.output)
        self.assertIn("failed to load", joined)
        self.assertIn(self.basename, joined)


# ─────────────────────────────────────────────────────────────────────────────
# Yoga regression — same verdict on every entry as before the refactor
# ─────────────────────────────────────────────────────────────────────────────


class YogaAdapterNoRegressionTests(unittest.TestCase):
    """Yoga must be discovered and classify entries identically to before.

    This is the issue's "Existing PASS/DIVERGE/NO-OP/NOT-IMPL/OOS
    classification unchanged" criterion.

    The expected status counts here mirror the on-main baseline at
    sha 189255c2 (yoga harness scaffold landed in #1395).
    """

    EXPECTED = {
        "total": 53,
        "pass": 7,
        "diverge": 28,
        "no_op": 0,
        "not_impl": 18,
        "oos": 0,
    }

    def test_yoga_adapter_present_after_discovery(self) -> None:
        verifier._discover_adapters(reload=True)
        self.assertIn("yoga", verifier.ADAPTERS)
        # The class registered under "yoga" must be a real AdapterBase.
        self.assertTrue(issubclass(verifier.ADAPTERS["yoga"], AdapterBase))

    def test_yoga_results_match_baseline(self) -> None:
        repo_root = verifier.find_repo_root(REPO_ROOT)
        results = verifier.run_surface(repo_root, "yoga")

        bucketed = {st: 0 for st in (
            Status.PASS,
            Status.DIVERGE,
            Status.NO_OP,
            Status.NOT_IMPL,
            Status.OOS,
        )}
        for r in results:
            bucketed[r.status] = bucketed.get(r.status, 0) + 1

        self.assertEqual(len(results), self.EXPECTED["total"])
        self.assertEqual(bucketed[Status.PASS], self.EXPECTED["pass"])
        self.assertEqual(bucketed[Status.DIVERGE], self.EXPECTED["diverge"])
        self.assertEqual(bucketed[Status.NO_OP], self.EXPECTED["no_op"])
        self.assertEqual(bucketed[Status.NOT_IMPL], self.EXPECTED["not_impl"])
        self.assertEqual(bucketed[Status.OOS], self.EXPECTED["oos"])


if __name__ == "__main__":
    unittest.main()

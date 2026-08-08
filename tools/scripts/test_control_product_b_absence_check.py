#!/usr/bin/env python3
"""Mutation proof for the Product B absence gate."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

import control_product_b_absence_check as gate


class ProductBAbsenceGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)
        (self.root / "inspect/include").mkdir(parents=True)
        (self.root / "core/format").mkdir(parents=True)
        (self.root / "apple/Sources").mkdir(parents=True)
        (self.root / "templates").mkdir(parents=True)
        (self.root / "docs/policies").mkdir(parents=True)
        (self.root / "inspect/include/clean.hpp").write_text("namespace pulp {}\n", encoding="utf-8")
        (self.root / "docs/policies/plugin-collaboration.md").write_text(
            "# Policy\n\nProduct B remains unshipped under this NO-GO; this policy is not an API or\n"
            "product commitment.\n", encoding="utf-8"
        )

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def test_clean_fixture_passes(self) -> None:
        self.assertEqual(gate.verify(self.root), [])

    def test_rejects_product_namespace_in_public_source(self) -> None:
        path = self.root / "inspect/include/route.hpp"
        path.write_text("namespace pulp::inspect::ProductBRoute {}\n", encoding="utf-8")
        self.assertTrue(any("ProductB" in error and "route.hpp" in error for error in gate.verify(self.root)))

    def test_rejects_product_namespace_in_core_source(self) -> None:
        path = self.root / "core/format/route.cpp"
        path.write_text("namespace pulp::format::ProductBRoute {}\n", encoding="utf-8")
        self.assertTrue(any("ProductB" in error and "core/format/route.cpp" in error
                            for error in gate.verify(self.root)))

    def test_rejects_product_namespace_in_shipped_swift(self) -> None:
        path = self.root / "apple/Sources/Route.swift"
        path.write_text("enum ProductBRoute {}\n", encoding="utf-8")
        self.assertTrue(any("ProductB" in error and "apple/Sources/Route.swift" in error
                            for error in gate.verify(self.root)))

    def test_rejects_product_namespace_in_template(self) -> None:
        path = self.root / "templates/control.hpp.in"
        path.write_text("product_b_route\n", encoding="utf-8")
        self.assertTrue(any("product_b" in error and "templates/control.hpp.in" in error
                            for error in gate.verify(self.root)))

    def test_rejects_normative_public_document_claim(self) -> None:
        path = self.root / "docs/reference/control.md"
        path.parent.mkdir(parents=True)
        path.write_text("Cross-vendor support is enabled.\n", encoding="utf-8")
        self.assertTrue(any("cross-vendor support" in error for error in gate.verify(self.root)))

    def test_rejects_binary_marker(self) -> None:
        binary = self.root / "pulp-control-broker"
        binary.write_bytes(b"header\\0pulp::inspect::ProductBRoute\\0")
        self.assertTrue(any("in binary" in error and "ProductB" in error
                            for error in gate.verify(self.root, (binary,))))

    def test_no_go_exemption_requires_the_no_go(self) -> None:
        path = self.root / "docs/policies/plugin-collaboration.md"
        path.write_text("# Policy\n\nProduct B remains out of scope.\n", encoding="utf-8")
        self.assertTrue(any("required explicit NO-GO policy text" in error
                            for error in gate.verify(self.root)))

    def test_required_no_go_policy_cannot_be_deleted(self) -> None:
        (self.root / "docs/policies/plugin-collaboration.md").unlink()
        self.assertTrue(any("required explicit NO-GO policy is missing" in error
                            for error in gate.verify(self.root)))


if __name__ == "__main__":
    unittest.main()

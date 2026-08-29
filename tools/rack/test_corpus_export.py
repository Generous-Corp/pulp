#!/usr/bin/env python3
"""Fail-closed checks for the public/private corpus export boundary."""

from __future__ import annotations

import copy
import json
import os
import tempfile

import corpus_export as export
import patch as patch_mod


def expect_refused(report: dict) -> None:
    try:
        export.validate_public_report(report)
    except export.PublicBoundaryError:
        return
    raise AssertionError("source-shaped report was accepted")


def main() -> int:
    marker = "SYNTHETIC_PRIVATE_MARKER"
    corpus = [({
        "id": "opaque-17",
        "url": "https://private.invalid/items/opaque-17",
        "title": marker,
        "author": "fixture-uploader",
        "sha256": "a" * 64,
        "tags": [],
    }, {
        "modules": [{"id": 91, "plugin": "FixtureVendor", "model": marker}],
        "cables": [{
            "outputModuleId": 91, "outputId": 0,
            "inputModuleId": 91, "inputId": 1,
        }],
    })]
    private_priors = {"admitted": [{
        "plugin": "FixtureVendor", "model": marker, "direction": "input",
        "index": 7, "signal": "pitch", "support": 4,
        "contributors": ["fixture-uploader", "second-fixture"],
    }]}
    report = export.build_public_report(
        corpus, {}, {"roles": {}, "ports": {}}, {}, private_priors,
        generated_at="2026-01-02T03:04:05Z")
    encoded = json.dumps(report, sort_keys=True)
    for private_value in (marker, "opaque-17", "fixture-uploader",
                          "FixtureVendor", "private.invalid", "a" * 64):
        assert private_value not in encoded
    assert report["aggregate_counts"]["patches_analyzed"] == 1
    assert report["corroborated_priors"] == [{
        "signal": "pitch", "support_bucket": "4_to_7", "prior_count": 1,
    }]
    print("  ok  source-shaped inputs collapse to source-neutral aggregates")

    malicious = [
        ("url", "https://private.invalid/items/opaque-17"),
        ("title", "Synthetic Upload Name"),
        ("uploader", "fixture-uploader"),
        ("id", "opaque-17"),
        ("sha256", "b" * 64),
        ("body", {"opaque": marker}),
        ("modules", [{"plugin": "FixtureVendor", "model": "FixtureModel"}]),
        ("cables", [{"from": [0, 0], "to": [1, 0]}]),
        ("source_prose", "Synthetic source wording that must remain private"),
    ]
    for key, value in malicious:
        candidate = copy.deepcopy(report)
        candidate[key] = value
        expect_refused(candidate)

    candidate = copy.deepcopy(report)
    candidate["conclusions"].append(
        "Synthetic source wording that must remain private")
    expect_refused(candidate)
    candidate = copy.deepcopy(report)
    candidate["corroborated_priors"].append({
        "signal": "pitch", "support_bucket": "3", "prior_count": 1,
        "uploader": "fixture-uploader",
    })
    expect_refused(candidate)
    candidate = copy.deepcopy(report)
    candidate["aggregate_counts"]["coverage_buckets"][0]["count"] = 99
    expect_refused(candidate)
    print("  ok  source locators, identities, prose, fingerprints and topology "
          "fail closed")

    with tempfile.TemporaryDirectory() as tmp:
        occupied = os.path.join(tmp, "occupied")
        os.mkdir(occupied)
        private_body = os.path.join(occupied, "old-private-body.vcv")
        with open(private_body, "w") as handle:
            handle.write(marker)
        try:
            export.write_public_report(occupied, report)
        except export.PublicBoundaryError:
            pass
        else:
            raise AssertionError(
                "export reused a directory containing private material")
        with open(private_body) as handle:
            assert handle.read() == marker

        clean = os.path.join(tmp, "public")
        path = export.write_public_report(clean, report)
        assert os.listdir(clean) == ["public-corpus-learnings.json"]
        with open(path) as handle:
            export.validate_public_report(json.load(handle))

        interrupted = os.path.join(tmp, "interrupted")
        original_dump = export.json.dump
        try:
            export.json.dump = lambda *_args, **_kwargs: (_ for _ in ()).throw(
                OSError("synthetic short write"))
            try:
                export.write_public_report(interrupted, report)
            except OSError as error:
                assert "synthetic short write" in str(error)
            else:
                raise AssertionError("interrupted export unexpectedly passed")
        finally:
            export.json.dump = original_dump
        assert not os.path.exists(interrupted)
        assert not [name for name in os.listdir(tmp)
                    if name.startswith(".interrupted.tmp-")]

        raced = os.path.join(tmp, "raced")
        os.mkdir(raced)
        marker = os.path.join(raced, "concurrent-owner")
        with open(marker, "w", encoding="utf-8") as handle:
            handle.write("preserve me\n")
        try:
            export.write_public_report(raced, report)
        except export.PublicBoundaryError:
            pass
        else:
            raise AssertionError("existing raced destination was replaced")
        with open(marker, encoding="utf-8") as handle:
            assert handle.read() == "preserve me\n"

        cli_dest = os.path.join(tmp, "cli-public")
        originals = (
            export.cs.held_patches, patch_mod.inventory,
            export.ic.load_roles, export.ic.load_idioms,
            export.private_usage_prior_report,
        )
        try:
            export.cs.held_patches = lambda: corpus
            patch_mod.inventory = lambda: {}
            export.ic.load_roles = lambda: {"roles": {}, "ports": {}}
            export.ic.load_idioms = lambda: {}
            export.private_usage_prior_report = lambda _roles: private_priors
            assert export.export(cli_dest) == 0
        finally:
            (export.cs.held_patches, patch_mod.inventory,
             export.ic.load_roles, export.ic.load_idioms,
             export.private_usage_prior_report) = originals
        with open(os.path.join(cli_dest, "public-corpus-learnings.json")) as handle:
            cli_report = json.load(handle)
        assert cli_report["corroborated_priors"] == [{
            "signal": "pitch", "support_bucket": "4_to_7",
            "prior_count": 1,
        }]
    print("  ok  export refuses stale destinations and writes exactly one safe "
          "file; CLI includes reduced priors; interruption leaves no output")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

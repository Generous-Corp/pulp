# Existing-build development provenance

Use `sdk_development_provenance.py check --source-dir SOURCE --build-dir BUILD
--prefix PREFIX --source-sha FULL_SHA` to inspect an existing Release GPU-audio
SDK installation. `check` is read-only. After reviewing its JSON, `stamp` with
the same arguments creates `PREFIX/sdk-provenance.json` atomically and refuses
to replace an existing marker. The published marker is mode 0644.

The marker is always `kind: development`, `distribution_eligible: false`, with
profile `existing-build-experiment`. It records allowlisted actual feature flags, not arbitrary cache values or the
stronger `forge-dev` profile's feature promises. It verifies clean source HEAD,
cache source/build paths, generated/installed build header and package config identity,
installed version/build-type markers, and the
GPU-audio archive. Archive comparison normalizes only symbol-index timestamps;
object bytes and all other archive bytes must match. Header, cache, CMake package,
and archive hashes are included. This does not retrospectively prove a complete
SDK was built hermetically, authenticate every installed dependency, or validate
GPU execution. Keep the producer build/install receipt alongside this marker.

Stop all builds/installs/source mutations before checking and stamping. Changing
artifacts afterward invalidates the receipt. No release tags or release markers
are created. Local Forge consumers must explicitly set
`FORGE_ALLOW_DEVELOPMENT_SDK=ON`; Spectr may retain its exact expected-SHA gate.
Release packaging remains prohibited.

Tests: `python3 tools/scripts/test_sdk_development_provenance.py`.

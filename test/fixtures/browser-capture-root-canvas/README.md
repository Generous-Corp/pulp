Native Chromium DOMSnapshot from the original short-HTML root canvas capture.

The snapshot is retained byte-for-byte, including the empty document style row, doctype, sparse metadata, and computed styles. SHA-256: `d4f36fa195578ff97c8f002c79d81f8bb54e5b277495368b9349e7fca3126d6c`. The test supplies a minimal envelope and reference PNG header; this is an importer structural regression, not pixel evidence.

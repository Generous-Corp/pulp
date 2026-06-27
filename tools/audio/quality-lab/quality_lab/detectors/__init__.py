"""Layer-A artifact detectors (§6).

One detector = one small module + one test (§14.4). Each is a pure analyzer:
(reference, candidate, sr, onset map) -> DetectorResult. No detector reaches into
another's internals; shared DSP lives in the package, not copied per file.
"""

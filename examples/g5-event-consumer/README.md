# G-5 event trace fixture (pre-runtime)

This directory freezes the first two-sample trace oracle for the reviewed
one-producer G-5 contract. `check_trace.py` accepts a JSON observation file from
the future installed-SDK consumer and compares two legal partitions. The
fixture is intentionally a contract check, not evidence that Pulp currently
transports graph-produced events. No graph event API is called here.

Run `python3 check_trace.py observations.json` after the runtime consumer emits
an observation with the schema in `expected.json`. Run
`python3 -m unittest test_check_trace.py` to prove the oracle accepts its frozen
positive and rejects planted timing, tie-order, and counter mistakes.

The runtime consumer must eventually add feedforward audio samples, reset,
refusal and budget controls before G-5 can advance beyond PARTIAL.

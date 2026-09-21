#!/usr/bin/env python3
"""Focused tests for the honest paced lead-screening driver."""

from __future__ import annotations

import json
from pathlib import Path
import stat
import tempfile
import unittest

import gpu_audio_p4_lead_campaign as campaign


FAKE_PROBE = r'''#!/usr/bin/env python3
import csv
import json
import pathlib
import sys

args = {item.split("=", 1)[0]: item.split("=", 1)[1] for item in sys.argv[1:] if "=" in item}
out = pathlib.Path(args["--output-dir"])
out.mkdir(exist_ok=True)
lead = int(args["--lead"])
receipt = {
    "schema": "pulp.gpu-audio-paced-convolution.v1",
    "status": "completed",
    "measured_miss_counter_delta": 0,
    "produced_blocks_before_stop": 3,
    "negative_control": False,
}
(out / "receipt.json").write_text(json.dumps(receipt))
with (out / "blocks.csv").open("w", newline="") as stream:
    writer = csv.writer(stream)
    writer.writerow(["callback_sequence", "measured", "source_sequence", "scheduled_ns",
                     "callback_begin_ns", "callback_end_ns", "miss_counter_delta",
                     "max_absolute_error", "finite"])
    writer.writerow([lead, 1, lead - 1, 0, 100, 130, 0, "0.000001", 1])
print(json.dumps(receipt))
'''


class LeadCampaignTests(unittest.TestCase):
    def make_probe(self, root: Path, contents: str = FAKE_PROBE) -> Path:
        probe = root / "probe.py"
        probe.write_text(contents)
        probe.chmod(probe.stat().st_mode | stat.S_IXUSR)
        return probe

    def test_four_leads_emit_explicit_unavailable_fields_and_unassigned_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = self.make_probe(root)
            output = root / "capture"
            args = campaign.parse_args(["--probe", str(probe), "--output-dir", str(output)])
            self.assertEqual(campaign.run(args), 0)

            manifest = json.loads((output / "campaign.json").read_text())
            self.assertEqual(manifest["schema"], campaign.SCHEMA)
            self.assertEqual(manifest["leads_blocks"], [1, 2, 4, 8])
            self.assertEqual(manifest["performance_verdict"], "unassigned")
            records = [json.loads(line) for line in (output / "lead-screening.jsonl").read_text().splitlines()]
            self.assertEqual(records[0]["record_kind"], "manifest")
            trials = [record for record in records if record["record_kind"] == "trial_begin"]
            blocks = [record for record in records if record["record_kind"] == "block"]
            self.assertEqual([trial["lead_blocks"] for trial in trials], [1, 2, 4, 8])
            self.assertEqual(len(blocks), 4)
            callback = blocks[0]["timings"]["callback_cpu"]
            self.assertEqual(callback["relation"], "direct")
            self.assertEqual(callback["value_ns"], 30)
            self.assertIsNone(callback["uncertainty_ns"])
            self.assertEqual(blocks[0]["timings"]["submit_to_completion"]["availability"], "unavailable")
            self.assertEqual(blocks[0]["gpu_terminal"], "unavailable")
            self.assertEqual(blocks[0]["delivery"], "unavailable")
            self.assertTrue((output / "lead-1" / "receipt.json").is_file())

    def test_missing_probe_artifacts_fail_closed_and_remove_capture(self) -> None:
        bad_probe = "#!/usr/bin/env python3\nraise SystemExit(1)\n"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = self.make_probe(root, bad_probe)
            output = root / "capture"
            args = campaign.parse_args(["--probe", str(probe), "--output-dir", str(output)])
            with self.assertRaises(RuntimeError):
                campaign.run(args)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()

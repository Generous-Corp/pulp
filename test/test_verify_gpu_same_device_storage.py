import csv
import tempfile
import unittest
from pathlib import Path
from verify_gpu_same_device_storage import verify


class ReceiptControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.log = 'schema=same-device-storage-screening-v1 owner=7\n'
        for ordinal in range(6):
            mode = 'staged' if (ordinal % 2) ^ ((ordinal // 2) % 2) else 'shared'
            with (self.path / f'{ordinal}-{mode}.csv').open('w', newline='') as stream:
                writer = csv.writer(stream)
                writer.writerow(('sequence', 'measured', 'scheduled_ns', 'start_ns', 'end_ns', 'deadline_ns', 'accepted', 'delivery', 'max_error'))
                for seq in range(784):
                    t = seq*128*1_000_000_000//48000
                    writer.writerow((seq, int(32 <= seq < 782), t, t, t+100,
                                     (seq+1)*128*1_000_000_000//48000, 1, 0, 0))
            transfers = 784 if mode == 'staged' else 0
            self.log += (f'trial={ordinal} mode={mode} owner=7 gpu=750 fallback_reference=0 '
                         f'callback_deadline_misses=0 rejected=0 failed=0 writes={transfers} '
                         f'copies={transfers} maps={transfers} process_cpu_seconds=.1\n')
        self.log += 'paired_screening=passed performance_verdict=unassigned\n'

    def test_valid_control(self):
        self.assertTrue(verify(self.path, self.log)['passed'])

    def test_owner_replacement_rejected(self):
        with self.assertRaises(AssertionError):
            verify(self.path, self.log.replace('trial=2 mode=staged owner=7', 'trial=2 mode=staged owner=8'))

    def test_planted_shared_copy_rejected(self):
        with self.assertRaises(AssertionError):
            verify(self.path, self.log.replace('writes=0 copies=0 maps=0', 'writes=1 copies=1 maps=1', 1))

    def test_duplicate_sequence_rejected(self):
        file = self.path / '0-shared.csv'
        with file.open() as stream:
            rows = list(csv.reader(stream))
        rows[3][0] = rows[2][0]
        with file.open('w', newline='') as stream:
            csv.writer(stream).writerows(rows)
        with self.assertRaises(AssertionError):
            verify(self.path, self.log)

    def test_fallback_cannot_claim_gpu_delivery(self):
        with self.assertRaises(AssertionError):
            verify(self.path, self.log.replace('gpu=750 fallback_reference=0', 'gpu=749 fallback_reference=1', 1))


if __name__ == '__main__':
    unittest.main()

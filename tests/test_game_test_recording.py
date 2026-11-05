import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import game_test_recording as rec  # noqa: E402


class RecordingIndex(unittest.TestCase):
    def write(self, text):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        path = Path(folder.name) / 'clip_1.idx'
        path.write_text(text, encoding='ascii')
        return path

    def test_reads_size_and_relative_times(self):
        path = self.write('sdgo-recording 1 width=4 height=2 format=bgra qpc_frequency=1000\n0 5000\n1 5033\n2 5100\nend frames=3\n')
        width, height, times = rec.read_index(path)
        self.assertEqual((width, height), (4, 2))
        for got, want in zip(times, [0.0, 0.033, 0.1]):
            self.assertAlmostEqual(got, want, places=9)
        self.assertEqual(len(times), 3)

    def test_rejects_foreign_or_broken_indexes(self):
        with self.assertRaises(ValueError):
            rec.read_index(self.write('not a recording\n'))
        with self.assertRaises(ValueError):
            rec.read_index(self.write('sdgo-recording 1 width=4 height=2 format=bgra qpc_frequency=1000\n0 1\n2 2\n'))

    def test_schedule_keeps_real_speed(self):
        # 10 fps capture with one 0.3 s stall, resampled to 20 fps: the stalled frame repeats, nothing runs fast.
        times = [0.0, 0.1, 0.2, 0.5, 0.6]
        self.assertEqual(rec.schedule(times, 20), [0, 0, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 4])


if __name__ == '__main__':
    unittest.main()

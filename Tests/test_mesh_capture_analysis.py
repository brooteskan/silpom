"""Regression checks for the moving-camera diagnostic classifier (stdlib only)."""
from pathlib import Path
import struct
import tempfile
import unittest

from mesh_capture_analysis import validate_motion_frame, motion_frame_passes


class MotionCaptureTests(unittest.TestCase):
    def classify(self, status, color=(245, 0, 245, 255), view_error=0, visible=True):
        image = bytearray(148)
        image[:4] = b"DDS "
        image[84:88] = b"DX10"
        struct.pack_into("<II", image, 12, 1, 3)
        struct.pack_into("<I", image, 128, 28)
        image += bytes((111, 45, 24, 255, 49, 82, 159, 255) if visible else (0,) * 8)
        image += bytes(color)
        hits = bytearray(160 + 3 * 96)
        struct.pack_into("<I", hits, 0, 1)
        struct.pack_into("<4f", hits, 16 + 64, 3, 1, 1, view_error)
        struct.pack_into("<4I", hits, 16 + 80, 0, 0, 3, 1)
        struct.pack_into("<I", hits, 16 + 96, 160)
        for pixel, state in enumerate((1, 1, status)):
            struct.pack_into("<I", hits, 160 + pixel * 96 + 12, state)
        with tempfile.TemporaryDirectory() as temporary:
            image_path = Path(temporary) / "albedo.dds"
            hit_path = Path(temporary) / "hits.bin"
            image_path.write_bytes(image)
            hit_path.write_bytes(hits)
            return validate_motion_frame(image_path, hit_path, [0])

    def test_certified_frame(self):
        self.assertTrue(motion_frame_passes(self.classify(1, (111, 45, 24, 255))))

    def test_missing_view_diagnostic_is_not_a_hit(self):
        check = self.classify(1)
        self.assertEqual(check["unexplained_magenta"], 1)
        self.assertFalse(motion_frame_passes(check))

    def test_diagnostic_on_miss_is_rejected(self):
        self.assertFalse(motion_frame_passes(self.classify(0)))

    def test_real_exhaustion_is_counted_not_certified(self):
        check = self.classify(2)
        self.assertEqual((check["magenta"], check["exhausted"], check["certified_hits"]), (1, 1, 2))
        self.assertEqual(check["unexplained_magenta"], 0)
        self.assertTrue(motion_frame_passes(check))

    def test_invalid_is_rejected(self):
        self.assertFalse(motion_frame_passes(self.classify(3)))

    def test_view_error_is_rejected(self):
        self.assertFalse(motion_frame_passes(self.classify(2, view_error=2)))

    def test_missing_fixture_is_rejected(self):
        self.assertFalse(motion_frame_passes(self.classify(2, visible=False)))


if __name__ == "__main__":
    unittest.main()

import unittest
from analyze_planar_ab import compare_distributions, percentile, summarize_capture

try:
    import numpy as np
except ImportError:
    np = None


class TimingTests(unittest.TestCase):
    def fixture(self):
        return {"complete": True, "requested": 2, "frames": 2,
                "passes": [{"path": "root", "parent": True}, {"path": "root.depth", "parent": False},
                           {"path": "root.forward", "parent": False}],
                "samples": [[0, 0, 1, 100, 100000], [0, 1, 2, 10, 10000], [0, 2, 12, 20, 20000],
                            [1, 0, 101, 100, 100000], [1, 1, 102, 20, 20000], [1, 2, 122, 30, 30000]]}

    def test_percentile_interpolation(self):
        self.assertAlmostEqual(percentile([1, 2, 3, 4], .95), 3.85)

    def test_parents_not_double_counted(self):
        summary = summarize_capture(self.fixture())
        self.assertAlmostEqual(summary["leaf_work_sum_not_frame_time"]["median_ms"], .04)
        self.assertNotIn("root", summary["passes"])

    def test_stale_readback_rejected(self):
        raw = self.fixture()
        raw["samples"][-1][2] = 12
        with self.assertRaisesRegex(AssertionError, "stale"):
            summarize_capture(raw)

    def test_incomplete_capture_rejected(self):
        raw = self.fixture()
        raw["complete"] = False
        with self.assertRaises(AssertionError):
            summarize_capture(raw)

    def test_distribution_improvement_sign(self):
        result = compare_distributions({"median_ms": 8, "p95_ms": 11},
                                       {"median_ms": 10, "p95_ms": 10})
        self.assertAlmostEqual(result["median_improvement"], .2)
        self.assertAlmostEqual(result["p95_improvement"], -.1)


@unittest.skipIf(np is None, "NumPy is optional for timing-only analysis")
class ImageTests(unittest.TestCase):
    def test_diagnostic_colors_and_roi(self):
        from analyze_planar_ab import diagnostic_pixels
        image = np.array([[[1, 0, 1], [1, 1, 0], [0, 1, 0]]], dtype=float)
        self.assertEqual(diagnostic_pixels(image), {"exhausted_pixels": 1, "invalid_pixels": 1})
        self.assertEqual(diagnostic_pixels(image, np.array([[True, False, True]])),
                         {"exhausted_pixels": 1, "invalid_pixels": 0})

    def test_identical_images(self):
        from analyze_planar_ab import image_metrics
        image = np.ones((24, 24, 3)) * .5
        depth = np.ones((24, 24)) * 3
        result = image_metrics(image, image, depth, depth)
        self.assertAlmostEqual(result["ssim_luma_roi_box11"], 1)
        self.assertEqual(result["depth_p95_metres"], 0)
        self.assertEqual(result["silhouette_p95_pixels"], 0)

    def test_silhouette_translation(self):
        from analyze_planar_ab import silhouette_distance
        a = np.zeros((24, 24), dtype=bool)
        b = a.copy()
        a[4:20, 4:20] = True
        b[4:20, 5:21] = True
        result = silhouette_distance(a, b)
        self.assertEqual(result["silhouette_p95_pixels"], 1)
        self.assertEqual(result["silhouette_max_pixels"], 1)


if __name__ == "__main__":
    unittest.main()

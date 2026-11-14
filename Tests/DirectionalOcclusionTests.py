"""Directional-union, reach, frame and estimator-selection contracts."""
from pathlib import Path
import sys
import unittest
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Tools"))
from directional_occlusion import (AngularOcclusion, Estimator, EstimatorSet, cosine_directions,
                                  directional_field, fit_angular, normalize,
                                  pack_blocked, query_directions)


class DirectionalOcclusion(unittest.TestCase):
    def setUp(self):
        self.directions = cosine_directions(1024)
        self.axis = normalize([[1, 0, .15]])
        self.radius = np.array([[.98, .8, .6, .4]])

    def test_overlapping_occluders_do_not_double_count(self):
        for beta in (0, 32):
            one = AngularOcclusion(self.axis, self.radius, beta).visibility(self.directions)
            twice = AngularOcclusion(np.repeat(self.axis, 2, 0), np.repeat(self.radius, 2, 0), beta).visibility(self.directions)
            np.testing.assert_array_equal(one, twice)

    def test_reach_coverage_is_nested_for_point_and_filtered_targets(self):
        for beta in (0, 32):
            values = AngularOcclusion(self.axis, self.radius, beta).visibility(self.directions)
            self.assertTrue((np.diff(values, axis=0) <= 1e-12).all())
        with self.assertRaises(ValueError):
            AngularOcclusion(self.axis, np.array([[.8, .9, .6, .4]]))

    def test_primitive_count_and_direction_count_are_independent(self):
        model = AngularOcclusion(np.repeat(self.axis, 4, 0), np.repeat(self.radius, 4, 0))
        values = model.visibility(cosine_directions(32))
        self.assertEqual(values.shape, (4, 32))
        mask = pack_blocked(values[-1])
        single = pack_blocked(AngularOcclusion(self.axis, self.radius).visibility(cosine_directions(32))[-1])
        self.assertEqual(mask, single)

    def test_fractional_visibility_is_not_a_packed_integer_target(self):
        with self.assertRaises(ValueError):
            pack_blocked([.25, .75])
        field = np.arange(32, dtype=float)[None] / 31
        np.testing.assert_array_equal(directional_field(field, cosine_directions(32)), field)

    def test_rigid_frame_rotation_does_not_change_visibility(self):
        angle = .714
        rotation = np.array([[np.cos(angle), 0, np.sin(angle)], [0, 1, 0],
                             [-np.sin(angle), 0, np.cos(angle)]])
        original = AngularOcclusion(self.axis, self.radius).visibility(self.directions)
        rotated = AngularOcclusion(self.axis @ rotation, self.radius).visibility(self.directions @ rotation)
        np.testing.assert_array_equal(original, rotated)

    def test_joint_union_fit_generalizes_to_unseen_directions(self):
        source = AngularOcclusion(normalize([[1, 0, .15], [-1, .4, .1]]),
                                  np.repeat(self.radius, 2, 0))
        train = query_directions(512, 983)
        calibration = query_directions(256, 719)
        test = query_directions(512, 381)
        fitted, _ = fit_angular(train, source.visibility(train), calibration,
                               source.visibility(calibration), 2, steps=500, restarts=2)
        self.assertLess(np.abs(fitted.visibility(test) - source.visibility(test)).mean(), .08)

    def test_selection_ids_are_explicit_and_distinct(self):
        self.assertEqual(len(set(Estimator)), 6)
        self.assertNotEqual(Estimator.EXISTING_RAY_NEURAL, Estimator.ANGULAR_OCCLUDERS_4)

    def test_switching_selects_the_requested_model_without_fallback(self):
        models = EstimatorSet(ray_neural=lambda d: np.zeros((4, len(d))), field32=np.ones((4, 32)))
        np.testing.assert_array_equal(models.evaluate(Estimator.EXISTING_RAY_NEURAL, self.directions), 0)
        np.testing.assert_array_equal(models.evaluate(Estimator.DIRECTIONAL_FIELD_32, self.directions), 1)
        with self.assertRaises(ValueError):
            models.evaluate(Estimator.ANGULAR_OCCLUDERS_4, self.directions)
        with self.assertRaises(ValueError):
            models.evaluate(Estimator.DIRECTIONAL_FIELD_32, self.directions, 9)
        with self.assertRaises(ValueError):
            models.evaluate(Estimator.CURRENT_ROUTING, self.directions)

    def test_invalid_representation_is_not_silently_accepted(self):
        with self.assertRaises(ValueError):
            AngularOcclusion([[float('nan'), 0, 1]], self.radius)
        with self.assertRaises(ValueError):
            AngularOcclusion(self.axis, self.radius, -1)
        with self.assertRaises(ValueError):
            EstimatorSet(field32=np.ones((4, 16))).evaluate(Estimator.DIRECTIONAL_FIELD_32, self.directions)


if __name__ == "__main__":
    unittest.main()

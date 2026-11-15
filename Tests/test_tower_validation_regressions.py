"""Regression checks for image gating and CPU-only certificate precision."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import numpy as np
from PIL import Image

from compare_tower_bounds import compare
from test_frustum_depth_bound import certificate_upper_float64

VIEWS=('exterior','lower-facing','lower-glancing','upper-glancing','gameplay')


class ImageGateTests(unittest.TestCase):
    def setUp(self):
        self.scratch=tempfile.TemporaryDirectory(prefix='silpom-validation-')
        root=Path(self.scratch.name).resolve()
        assert root.parent==Path(tempfile.gettempdir()).resolve()
        self.before,self.after=root/'before',root/'after'
        metadata=dict(passed=True,restored=True,samples=2,scene_sha256='fixture',
            effective_tower_configuration_sha256='normals-on',initial_aa_state=[2,0,0],
            controls={'Height-derived normals (off = cheaper)':True})
        self.image=np.full((2,3,4),128,dtype=np.uint8)
        for folder in (self.before,self.after):
            folder.mkdir();(folder/'results.json').write_text(json.dumps(metadata))
            for view in VIEWS:Image.fromarray(self.image).save(folder/(view+'.png'))
        self.hits=np.zeros((2,3,4,4),dtype='<u4');self.hits[...,3]=1
        self.depth=np.zeros((2,3,4),dtype='<u4')
        self.changed_hits=self.hits.copy();self.changed_depth=self.depth.copy()

    def tearDown(self):
        assert Path(self.scratch.name).resolve().parent==Path(tempfile.gettempdir()).resolve()
        self.scratch.cleanup()

    def run_comparison(self):
        # Small arrays isolate gate behavior; full-size DDS decoding is checked
        # separately against the retained real 2x capture files.
        def load(path,channels,format_id):
            if path.parent==self.before:return self.hits if channels==4 else self.depth
            return self.changed_hits if channels==4 else self.changed_depth
        with patch('compare_tower_bounds.atlas',side_effect=load):
            return compare(self.before,self.after)

    def test_unchanged_images_and_samples_pass(self):
        result=self.run_comparison()
        self.assertTrue(result['passed']);self.assertTrue(result['whole_atlas_payloads_identical'])

    def test_one_changed_pixel_fails_each_view_with_unchanged_atlases(self):
        for view in VIEWS:
            with self.subTest(view=view):
                changed=self.image.copy();changed[0,0,0]^=1
                Image.fromarray(changed).save(self.after/(view+'.png'))
                result=self.run_comparison()
                self.assertFalse(result['passed']);self.assertFalse(result['views'][view]['passed'])
                self.assertEqual(result['views'][view]['image_changed_pixels'],1)
                self.assertTrue(result['whole_atlas_payloads_identical'])
                Image.fromarray(self.image).save(self.after/(view+'.png'))

    def test_second_sample_depth_change_fails(self):
        self.changed_depth[0,0,1]=1
        result=self.run_comparison();self.assertFalse(result['passed'])
        self.assertEqual(result['views']['upper-glancing']['samples'][1]['changed_depth_bits'],1)

    def test_second_sample_winner_change_fails(self):
        self.changed_hits[0,0,1,0]=1
        result=self.run_comparison();self.assertFalse(result['passed'])
        self.assertEqual(result['views']['lower-facing']['samples'][1]['changed_winner_records'],1)

    def test_unused_columns_are_in_full_payload_evidence(self):
        self.changed_hits[0,0,3,0]=1
        result=self.run_comparison()
        self.assertTrue(result['passed']) # Unused texels are diagnostic, not visible winners.
        self.assertFalse(result['whole_atlas_payloads_identical'])
        evidence=result['views']['lower-facing']['atlas_payload_identity']['hit_records']
        self.assertFalse(evidence['equal'])
        self.assertNotEqual(evidence['baseline_sha256'],evidence['candidate_sha256'])

    def test_real_sample_count_controls_the_depth_gate(self):
        for count in (1,4):
            with self.subTest(samples=count):
                for folder in (self.before,self.after):
                    path=folder/'results.json';m=json.loads(path.read_text())
                    m.update(samples=count,initial_aa_state=[count,0,0]);path.write_text(json.dumps(m))
                self.assertTrue(self.run_comparison()['passed'])
                self.changed_depth[0,0,count-1]=1
                self.assertFalse(self.run_comparison()['passed'])
                self.changed_depth.fill(0)


class CertificatePrecisionTests(unittest.TestCase):
    def test_row_operand_is_promoted_before_multiplication(self):
        next_one=np.float32(1+2**-23)
        rows=np.zeros((4,4),dtype=np.float32)
        rows[2,3]=np.float32(1+2**-22);rows[3,3]=next_one
        value=certificate_upper_float64(rows,next_one,np.zeros(3,dtype=np.float32),
            np.zeros((3,4),dtype=np.float32),np.zeros(4,dtype=np.float32))
        # Exact binary arithmetic: rounded float32 multiplication would give 0.
        self.assertEqual(value.dtype,np.dtype('float64'))
        self.assertEqual(value,-2**-46)

    def test_error_operand_is_promoted_before_multiplication(self):
        next_one=np.float32(1+2**-23)
        rows=np.zeros((4,4),dtype=np.float32);rows[2,3]=-np.float32(1+2**-22)
        error=np.zeros(4,dtype=np.float32);error[3]=next_one
        value=certificate_upper_float64(rows,next_one,np.zeros(3,dtype=np.float32),
            np.zeros((3,4),dtype=np.float32),error)
        self.assertEqual(value.dtype,np.dtype('float64'))
        self.assertEqual(value,2**-46)


if __name__=='__main__':unittest.main()

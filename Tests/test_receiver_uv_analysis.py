"""Negative controls for receiver-UV image validation (no renderer required)."""
import unittest
import numpy as np

from analyze_receiver_uv_benchmark import compare_no_paint,compare_sample


class ReceiverUvAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.face=7
        self.records=np.zeros((2,3,4),dtype=np.uint32)
        self.records[...,3]=(self.face<<2)|1
        self.records[:,2,3]=(9<<2)|1  # Another receiving face must stay unchanged.
        self.depth=np.full((2,3),123,dtype=np.uint32)
        self.color=np.full((2,3,3),.25,dtype=np.float32)
        self.normal=np.full((2,3),512|(512<<10)|(1023<<20),dtype=np.uint32)
        self.changed_normal=1023|(1023<<20)  # Approximately +X instead of +Z.
        self.usage=np.zeros_like(self.records)
        self.usage[:,:2,0]=1
        self.usage[:,:2,1]=self.face
        self.usage[:,:2,2]=2048

    def compare(self,mode=1,color=None,normal=None,usage=None):
        return compare_sample(self.records,self.records,self.depth,self.depth,self.color,
            self.color if color is None else color,self.normal,self.normal if normal is None else normal,
            self.usage if usage is None else usage,self.face,2048,mode)

    def no_paint(self,color=None,normal=None,usage=None):
        return compare_no_paint(self.records,self.records,self.depth,self.depth,self.color,
            self.color if color is None else color,self.normal,self.normal if normal is None else normal,
            np.zeros_like(self.usage) if usage is None else usage)

    def test_identical_replay_checks_a_nonempty_normal_mask(self):
        report=self.compare()
        self.assertEqual(report['prefix_replay_normal_degrees']['pixels'],4)
        self.assertEqual(report['owned_normal_degrees']['pixels'],4)
        self.assertTrue(report['quality_passed'])

    def test_replay_normal_change_is_rejected_with_unchanged_color(self):
        changed=self.normal.copy();changed[0,0]=self.changed_normal
        with self.assertRaisesRegex(AssertionError,'replay changed native normals'):
            self.compare(normal=changed)

    def test_replay_color_change_is_rejected(self):
        changed=self.color.copy();changed[0,0,0]=.5
        with self.assertRaisesRegex(AssertionError,'replay changed native color'):
            self.compare(color=changed)

    def test_empty_replay_mask_is_rejected(self):
        with self.assertRaisesRegex(AssertionError,'not exercised'):
            self.compare(usage=np.zeros_like(self.usage))

    def test_other_face_normal_change_is_rejected_in_atlas_mode(self):
        use=self.usage.copy();use[:,:2,0]=2
        changed=self.normal.copy();changed[1,2]=self.changed_normal
        with self.assertRaisesRegex(AssertionError,'different receiver face normal'):
            self.compare(mode=2,normal=changed,usage=use)

    def test_atlas_normal_error_fails_quality_gate(self):
        use=self.usage.copy();use[:,:2,0]=2
        changed=self.normal.copy();changed[0,0]=self.changed_normal
        report=self.compare(mode=2,normal=changed,usage=use)
        self.assertEqual(report['cached_normal_degrees']['pixels'],4)
        self.assertGreater(report['cached_normal_degrees']['maximum'],80)
        self.assertFalse(report['quality_passed'])

    def test_usage_face_must_match_the_real_receiving_hit(self):
        use=self.usage.copy();use[0,2]=use[0,0]
        with self.assertRaisesRegex(AssertionError,'disagrees with hit face'):
            self.compare(usage=use)

    def test_no_paint_matches_baseline(self):
        self.assertTrue(self.no_paint()['color_and_normal_bit_exact'])

    def test_orphan_color_is_rejected_despite_zero_usage(self):
        changed=self.color.copy();changed[0,0,0]=.5
        with self.assertRaisesRegex(AssertionError,'retained visible color'):
            self.no_paint(color=changed)

    def test_cleared_normal_is_rejected_despite_zero_usage(self):
        changed=self.normal.copy();changed[1,0]=self.changed_normal
        with self.assertRaisesRegex(AssertionError,'retained visible normals'):
            self.no_paint(normal=changed)

    def test_no_paint_activity_is_rejected(self):
        with self.assertRaisesRegex(AssertionError,'reported cache/replay activity'):
            self.no_paint(usage=self.usage)


if __name__=='__main__':unittest.main()

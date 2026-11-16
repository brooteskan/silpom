"""Analytic and negative controls for the authoritative surface texture oracle."""
import base64
import copy
import math
import struct
import unittest

import numpy as np

from analyze_surface_texture_paint import (compose_normal,decode_chart,face_frame,generate_mips,
    ordered_charts,quad_derivatives,sample_level,sample_mips,compare_sample)
from analyze_surface_texture_paint import neutral_normal_support,predict,validate_shader_uv

F=np.float32


def encode_fixture(image):
    raw=np.asarray(image,dtype='<f4').tobytes();checksum=2166136261
    for byte in raw:checksum=((checksum^byte)*16777619)&0xffffffff
    height,width=image.shape[:2];count=width*height*8
    header=struct.pack('<6I',0x31545053,width,height,count,checksum,count)
    return base64.b64encode(header+raw).decode('ascii')


class SurfaceTextureAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.image=np.zeros((32,32,8),dtype=F)
        self.image[...,:4]=[.4,.1,.2,.5]
        self.detail=np.array([.3,.4,math.sqrt(.75)],dtype=F)
        self.image[...,4:7]=self.detail*F(.6);self.image[...,7]=.6
        self.face={'face':'fixture','hit_face':7,'corners':[
            {'position':[0,0,0],'uv':[0,0]},{'position':[1,0,0],'uv':[1,0]},{'position':[0,1,0],'uv':[0,1]}]}
        self.document={'ClassData':{'TextureAuthoritative':True,'Layers':[{'Id':1}],
            'SurfacePaint':[{'Receiver':123,'Face':'fixture','Layer':1,'Rectangle':[0,0,1,1],
                'Resolution':32,'Texels':encode_fixture(self.image)}]}}
        self.charts,_=ordered_charts(self.document,[self.face],[0,0,1])
        self.records=np.zeros((16,16,4),dtype=np.uint32)
        y,x=np.mgrid[:16,:16]
        uv=np.stack((.25+x/128,.25+y/128),axis=-1).astype('<f4')
        self.records[...,:2]=uv.view('<u4');self.records[...,3]=(7<<2)|1
        self.base_color=np.broadcast_to(np.array([.2,.4,.6],dtype=F),(16,16,3)).copy()
        self.base_normal=np.broadcast_to(np.array([0,0,1],dtype=F),(16,16,3)).copy()
        self.color=np.broadcast_to(np.array([.5,.3,.5],dtype=F),(16,16,3)).copy()
        vector=[.18,.24,.6*math.sqrt(.75)+.4];length=math.sqrt(sum(v*v for v in vector))
        self.normal=np.broadcast_to(np.array([v/length for v in vector],dtype=F),(16,16,3)).copy()
        self.usage=np.zeros_like(self.records);self.usage[...,0]=3;self.usage[...,1]=7;self.usage[...,2]=1;self.usage[...,3]=99

    def compare(self,**overrides):
        arguments={'records':self.records,'baseline_color':self.base_color,'baseline_normal':self.base_normal,
            'actual_color':self.color,'actual_normal':self.normal,'charts':self.charts,'usage':self.usage}
        arguments.update(overrides)
        return compare_sample(**arguments)

    def test_source_payload_roundtrip_and_checksum_rejection(self):
        encoded=self.document['ClassData']['SurfacePaint'][0]['Texels']
        self.assertTrue(np.array_equal(decode_chart(encoded),self.image))
        changed=bytearray(base64.b64decode(encoded));changed[-1]^=1
        with self.assertRaisesRegex(AssertionError,'checksum'):
            decode_chart(base64.b64encode(changed).decode())

    def test_mips_keep_raw_directional_cancellation_and_coverage(self):
        image=np.zeros((32,32,8),dtype=F)
        image[...,7]=1;image[...,6]=.6;image[:,0::2,4]=.8;image[:,1::2,4]=-.8
        mips=generate_mips(image)
        self.assertEqual(len(mips),6)
        self.assertTrue(np.allclose(mips[-1][0,0,4:8],[0,0,.6,1],atol=1e-6))

    def test_bilinear_texel_centers_and_edge_clamp(self):
        image=np.zeros((2,2,8),dtype=F);image[0,0,0]=1;image[0,1,2]=1
        values=sample_level(image,np.array([[.25,.25],[.5,.25],[-1,-1]],dtype=F))
        self.assertTrue(np.allclose(values[0,:3],[1,0,0]))
        self.assertTrue(np.allclose(values[1,:3],[.5,0,.5]))
        self.assertTrue(np.allclose(values[2,:3],[1,0,0]))

    def test_trilinear_filters_complete_premultiplied_tuple(self):
        low=np.full((2,2,8),.2,dtype=F);high=np.full((1,1,8),.8,dtype=F)
        result=sample_mips([low,high],np.array([[.4,.6]],dtype=F),np.array([.25],dtype=F))
        self.assertTrue(np.allclose(result,.35,atol=1e-6))

    def test_exact_neutral_support_excludes_weight_cancellation_and_zero_weight_taps(self):
        image=np.zeros((2,2,8),dtype=F);image[...,7]=1;image[...,6]=.6
        image[:,0,4]=.5;image[:,1,4]=-.5
        mips=generate_mips(image);q=np.array([[.5,.25]],dtype=F);lod=np.array([0],dtype=F)
        self.assertEqual(sample_mips(mips,q,lod)[0,4],0)
        self.assertFalse(neutral_normal_support(mips,q,lod)[0])
        image[...,4:6]=0;image[0,1,4]=.5;mips=generate_mips(image);q[0]=[.25,.25]
        self.assertEqual(sample_mips(mips,q,lod)[0,4],0)
        self.assertFalse(neutral_normal_support(mips,q,lod)[0])
        image[...,4:6]=0;mips=generate_mips(image)
        self.assertTrue(neutral_normal_support(mips,q,lod)[0])

    def test_composition_matches_closed_form_rotation_of_live_normal(self):
        angle=math.pi/4;base=np.array([[math.sin(angle),0,math.cos(angle)]],dtype=F)
        paint=np.array([[.15,.2,.5*math.sqrt(.75),.5]],dtype=F)
        detail=[.15,.2,.5*math.sqrt(.75)+.5];length=math.sqrt(sum(v*v for v in detail));detail=[v/length for v in detail]
        expected=[math.cos(angle)*detail[0]+math.sin(angle)*detail[2],detail[1],
            -math.sin(angle)*detail[0]+math.cos(angle)*detail[2]]
        self.assertTrue(np.allclose(compose_normal(base,paint)[0],expected,atol=2e-6))

    def test_flat_detail_preserves_exact_live_value(self):
        live=np.array([[.1,.2,.7],[-.3,.1,-.9]],dtype=F)
        paint=np.array([[0,0,.3,.3],[0,0,1,1]],dtype=F)
        self.assertTrue(np.array_equal(compose_normal(live,paint),live))

    def test_world_frame_handles_rotated_uv_axes(self):
        face=copy.deepcopy(self.face)
        face['corners'][1]['position']=[0,2,0];face['corners'][2]['position']=[-3,0,0]
        self.assertTrue(np.allclose(face_frame(face),[[0,1,0],[-1,0,0],[0,0,1]]))

    def test_quad_derivatives_are_explicit_coarse_values(self):
        uv,dx,dy,valid=quad_derivatives(self.records)
        self.assertTrue(np.all(valid));self.assertTrue(np.allclose(dx,[1/128,0]))
        self.assertTrue(np.allclose(dy,[0,1/128]))
        changed=self.records.copy();changed[1,1,3]=(9<<2)|1
        self.assertEqual(int((~quad_derivatives(changed)[3]).sum()),4)

    def test_analytic_rendered_texture_passes_with_nonempty_normal_mask(self):
        report=self.compare()
        self.assertTrue(report['quality_passed'])
        self.assertEqual(report['painted_normal_samples'],256)
        self.assertLess(report['normal_degrees']['maximum'],.05)
        self.assertLess(report['color_error']['maximum'],1e-6)

    def test_wrong_textured_normal_fails_quality_limits(self):
        changed=self.normal.copy();changed[0,0]=[0,1,0]
        report=self.compare(actual_normal=changed)
        self.assertFalse(report['quality_passed']);self.assertGreater(report['normal_degrees']['maximum'],60)

    def test_wrong_color_fails_quality_limits(self):
        changed=self.color.copy();changed[0,0,0]+=.25
        report=self.compare(actual_color=changed)
        self.assertFalse(report['quality_passed']);self.assertGreater(report['color_error']['maximum'],.2)

    def test_empty_normal_mask_cannot_claim_quality(self):
        charts=copy.deepcopy(self.charts)
        image=np.zeros_like(self.image);image[...,:4]=self.image[...,:4]
        charts[0]['mips']=generate_mips(image)
        report=self.compare(charts=charts,actual_normal=self.base_normal)
        self.assertEqual(report['painted_normal_samples'],0);self.assertFalse(report['quality_passed'])

    def test_corrupted_neutral_pixel_is_checked_in_full_domain(self):
        charts=copy.deepcopy(self.charts)
        # The first two columns of source texels are neutral. One capture pixel
        # samples their centers while other pixels still exercise detail.
        image=self.image.copy();image[:,:10,4:8]=0;charts[0]['mips']=generate_mips(image)
        records=self.records.copy()
        uv=records[...,:2].view('<f4');uv[:2,:2,0]=F(.25);uv[:2,:2,1]=F(.25)
        # Closed-form interpolation across the step between source columns9
        # and10, followed by composition onto a +Z base normal.
        weight=np.clip(uv[...,0]*F(32)-F(.5)-F(9),F(0),F(1));coverage=F(.6)*weight
        normal=np.stack((coverage*F(.3),coverage*F(.4),coverage*F(math.sqrt(.75))+F(1)-coverage),axis=-1)
        normal/=np.sqrt(np.sum(normal*normal,axis=-1,keepdims=True))
        self.assertTrue(self.compare(records=records,charts=charts,actual_normal=normal)['quality_passed'])
        normal[0,0]=[0,1,0]
        report=self.compare(records=records,charts=charts,actual_normal=normal)
        self.assertFalse(report['quality_passed'])
        self.assertGreater(report['receiving_normal_degrees']['maximum'],80)

    def test_source_neutral_normal_requires_bit_exact_output_within_angular_tolerance(self):
        charts=copy.deepcopy(self.charts)
        image=self.image.copy();image[:,:10,4:8]=0;charts[0]['mips']=generate_mips(image)
        records=self.records.copy();uv=records[...,:2].view('<f4')
        uv[:2,:2,0]=F(.25);uv[:2,:2,1]=F(.25)
        weight=np.clip(uv[...,0]*F(32)-F(.5)-F(9),F(0),F(1));coverage=F(.6)*weight
        normal=np.stack((coverage*F(.3),coverage*F(.4),coverage*F(math.sqrt(.75))+F(1)-coverage),axis=-1)
        normal/=np.sqrt(np.sum(normal*normal,axis=-1,keepdims=True))
        good=self.compare(records=records,charts=charts,actual_normal=normal)
        self.assertTrue(good['quality_passed']);self.assertGreaterEqual(good['source_neutral_normal_samples'],4)
        normal[0,0,0]=F(1e-6)
        report=self.compare(records=records,charts=charts,actual_normal=normal)
        self.assertLess(report['receiving_normal_degrees']['maximum'],5)
        self.assertFalse(report['source_neutral_normals_bit_exact']);self.assertFalse(report['quality_passed'])

    def test_uncharted_receiving_material_requires_bit_exact_color_and_normal(self):
        records=self.records.copy();records[-2:,-2:,3]=(9<<2)|1
        usage=self.usage.copy();usage[-2:,-2:]=0
        color=self.color.copy();color[-2:,-2:]=self.base_color[-2:,-2:]
        normal=self.normal.copy();normal[-2:,-2:]=self.base_normal[-2:,-2:]
        good=self.compare(records=records,usage=usage,actual_color=color,actual_normal=normal)
        self.assertTrue(good['quality_passed']);self.assertEqual(good['uncharted_receiving_samples'],4)
        for channel in ('color','normal'):
            with self.subTest(channel=channel):
                c=color.copy();n=normal.copy()
                (c if channel=='color' else n)[-1,-1,0]+=F(1e-6)
                report=self.compare(records=records,usage=usage,actual_color=c,actual_normal=n)
                self.assertFalse(report['uncharted_material_bit_exact']);self.assertFalse(report['quality_passed'])

    def test_cross_face_quads_are_excluded_and_reported(self):
        changed=self.records.copy();changed[1,1,3]=(9<<2)|1
        use=self.usage.copy();use[1,1]=0
        color=self.color.copy();color[1,1]=self.base_color[1,1]
        normal=self.normal.copy();normal[1,1]=self.base_normal[1,1]
        report=self.compare(records=changed,usage=use,actual_color=color,actual_normal=normal)
        self.assertTrue(report['quality_passed']);self.assertEqual(report['excluded_cross_face_or_invalid_quad_samples'],4)
        self.assertEqual(report['painted_normal_samples'],252)

    def test_chart_edge_quads_are_excluded_and_reported(self):
        charts=copy.deepcopy(self.charts);charts[0]['rectangle']=np.array([.254,0,1,1],dtype=F)
        use=self.usage.copy();use[:,0]=0
        report=self.compare(charts=charts,usage=use)
        self.assertTrue(report['quality_passed']);self.assertEqual(report['excluded_chart_edge_quad_samples'],32)
        self.assertEqual(report['painted_normal_samples'],224)

    def test_missing_texture_use_and_wrong_chart_count_are_rejected(self):
        changed=self.usage.copy();changed[0,0]=0
        with self.assertRaisesRegex(AssertionError,'did not match'):
            self.compare(usage=changed)
        changed=self.usage.copy();changed[0,0,2]=2
        with self.assertRaisesRegex(AssertionError,'chart count'):
            self.compare(usage=changed)

    def test_usage_domain_is_validated_outside_quality_quads(self):
        records=self.records.copy();records[1,1,3]=(9<<2)|1
        usage=self.usage.copy();usage[1,1]=0
        # The whole quad is excluded from interpolation quality, but every
        # receiving sample still has an exact chart-domain usage contract.
        for change in ('count','missing','outside','zero_record'):
            with self.subTest(change=change):
                changed=usage.copy()
                if change=='count':changed[0,0,2]=2
                elif change=='missing':changed[0,0]=0
                elif change=='outside':changed[1,1]=[3,9,1,99]
                else:changed[1,1,3]=99
                with self.assertRaises(AssertionError):
                    self.compare(records=records,usage=changed)

    def test_raw_neutral_identity_is_required_even_when_packed_normals_match(self):
        charts=copy.deepcopy(self.charts);image=self.image.copy();image[:,:10,4:8]=0
        charts[0]['mips']=generate_mips(image)
        records=self.records.copy();uv=records[...,:2].view('<f4')
        uv[:2,:2]=[F(.25),F(.25)]
        weight=np.clip(uv[...,0]*F(32)-F(.5)-F(9),F(0),F(1));coverage=F(.6)*weight
        normal=np.stack((coverage*F(.3),coverage*F(.4),coverage*F(math.sqrt(.75))+F(1)-coverage),axis=-1)
        normal/=np.sqrt(np.sum(normal*normal,axis=-1,keepdims=True))
        packed=np.zeros(records.shape[:2],dtype=np.uint32)
        good=self.compare(records=records,charts=charts,actual_normal=normal,baseline_packed=packed,actual_packed=packed)
        self.assertTrue(good['quality_passed'])
        normal[0,0,0]=F(1e-6)
        report=self.compare(records=records,charts=charts,actual_normal=normal,baseline_packed=packed,actual_packed=packed)
        self.assertTrue(report['source_neutral_packed_normals_bit_exact'])
        self.assertFalse(report['source_neutral_raw_normals_bit_exact'])
        self.assertFalse(report['source_neutral_normals_bit_exact']);self.assertFalse(report['quality_passed'])

    def test_raw_shader_coordinates_and_derivatives_drive_sampling(self):
        image=self.image.copy();image[...,3]=1
        image[...,:3]=np.arange(32,dtype=F)[None,:,None]/F(32)
        charts=copy.deepcopy(self.charts);charts[0]['mips']=generate_mips(image)
        uv=self.records[...,:2].view('<f4').copy();uv[...,0]+=F(.125)
        gradients=np.zeros((16,16,4),dtype=F)
        predicted=predict(self.records,self.base_color,self.base_normal,charts,uv,gradients)
        expected=sample_level(image,uv.reshape(-1,2)).reshape(16,16,8)[...,:3]
        self.assertTrue(np.array_equal(predicted['color'],expected))
        self.assertTrue(np.all(predicted['mip_low']==0))
        gradients[...,0]=1
        predicted=predict(self.records,self.base_color,self.base_normal,charts,uv,gradients)
        self.assertTrue(np.all(predicted['mip_low']==5))
        self.assertTrue(np.allclose(predicted['color'],np.mean(image[...,:3],axis=(0,1))))

    def test_raw_shader_address_roundtrip_is_bounded_independently(self):
        uv=self.records[...,:2].view('<f4').copy()
        uv[...,0]=np.nextafter(uv[...,0],F(1))
        report=validate_shader_uv(self.records,uv,self.charts,self.usage)
        self.assertGreater(report['maximum_absolute'],0)
        changed=uv.copy();changed[...,0]+=F(.001)
        with self.assertRaisesRegex(AssertionError,'changed receiving address'):
            validate_shader_uv(self.records,changed,self.charts,self.usage)
        tight=copy.deepcopy(self.charts);tight[0]['rectangle']=np.array([.25,.25,.251,.251],dtype=F)
        changed=uv.copy();changed[...,0]+=F(1e-6)
        with self.assertRaisesRegex(AssertionError,'changed receiving address'):
            validate_shader_uv(self.records,changed,tight,self.usage)

    def test_raw_audit_cannot_pass_when_real_forward_normal_disagrees(self):
        good=self.compare(forward_normal=self.normal,forward_base_normal=self.base_normal)
        self.assertTrue(good['quality_passed'])
        changed=self.normal.copy();changed[0,0]=[0,1,0]
        report=self.compare(forward_normal=changed,forward_base_normal=self.base_normal)
        self.assertFalse(report['quality_passed'])
        self.assertGreater(report['forward_normal_degrees']['maximum'],60)
        self.assertGreater(report['raw_to_forward_normal_degrees']['maximum'],60)
        base=self.base_normal.copy();base[0,0]=[0,1,0]
        report=self.compare(forward_normal=self.normal,forward_base_normal=base)
        self.assertFalse(report['quality_passed'])
        self.assertGreater(report['base_material_normal_degrees']['maximum'],80)


if __name__=='__main__':unittest.main()

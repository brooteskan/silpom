// SPDX-License-Identifier: MIT
// Fixed-cell interval evaluation from the transported binary32 inputs. Unlike
// bounds reconstructed from small child control nets, rounding error is carried
// through the source polynomial before multiplying by the inverse Jacobian.
float SilPomCurved_ProofDown(float value)
{
    if(abs(value)<1.175494351e-38f)return -1.175494351e-38f; // include shader flush-to-zero
    if(!isfinite(value))return value;
    return asfloat(value>0?asuint(value)-1:asuint(value)+1);
}
float SilPomCurved_ProofUp(float value)
{
    if(abs(value)<1.175494351e-38f)return 1.175494351e-38f;
    if(!isfinite(value))return value;
    return asfloat(value>0?asuint(value)+1:asuint(value)-1);
}
float2 SilPomCurved_ProofAdd(float2 a,float2 b)
{
    precise float lo=a.x+b.x,hi=a.y+b.y;
    return float2(SilPomCurved_ProofDown(lo),SilPomCurved_ProofUp(hi));
}
float2 SilPomCurved_ProofSubtract(float2 a,float2 b) {return SilPomCurved_ProofAdd(a,-b.yx);}
float2 SilPomCurved_ProofMultiply(float2 a,float2 b)
{
    precise float4 values=float4(a.x*b.x,a.x*b.y,a.y*b.x,a.y*b.y);
    return float2(SilPomCurved_ProofDown(min(min(values.x,values.y),min(values.z,values.w))),
        SilPomCurved_ProofUp(max(max(values.x,values.y),max(values.z,values.w))));
}
float2 SilPomCurved_ProofDivide(float2 a,float b)
{
    precise float lo=(b>0?a.x:a.y)/b,hi=(b>0?a.y:a.x)/b;
    return float2(SilPomCurved_ProofDown(lo),SilPomCurved_ProofUp(hi));
}
struct SilPomCurved_ProofJet {float2 value;float2 du;float2 dv;};
SilPomCurved_ProofJet SilPomCurved_ProofConstant(float value)
{
    SilPomCurved_ProofJet result;result.value=value.xx;result.du=0;result.dv=0;return result;
}
SilPomCurved_ProofJet SilPomCurved_ProofJetAdd(SilPomCurved_ProofJet a,SilPomCurved_ProofJet b)
{
    SilPomCurved_ProofJet result;result.value=SilPomCurved_ProofAdd(a.value,b.value);result.du=SilPomCurved_ProofAdd(a.du,b.du);result.dv=SilPomCurved_ProofAdd(a.dv,b.dv);return result;
}
SilPomCurved_ProofJet SilPomCurved_ProofJetMultiply(SilPomCurved_ProofJet a,SilPomCurved_ProofJet b)
{
    SilPomCurved_ProofJet result;result.value=SilPomCurved_ProofMultiply(a.value,b.value);
    result.du=SilPomCurved_ProofAdd(SilPomCurved_ProofMultiply(a.du,b.value),SilPomCurved_ProofMultiply(a.value,b.du));
    result.dv=SilPomCurved_ProofAdd(SilPomCurved_ProofMultiply(a.dv,b.value),SilPomCurved_ProofMultiply(a.value,b.dv));return result;
}
SilPomCurved_ProofJet SilPomCurved_ProofAffine(float a,float b,float c,SilPomCurved_ProofJet u,SilPomCurved_ProofJet v)
{
    SilPomCurved_ProofJet db=SilPomCurved_ProofConstant(0),dc=db;
    db.value=SilPomCurved_ProofSubtract(b.xx,a.xx);dc.value=SilPomCurved_ProofSubtract(c.xx,a.xx);
    return SilPomCurved_ProofJetAdd(SilPomCurved_ProofConstant(a),SilPomCurved_ProofJetAdd(SilPomCurved_ProofJetMultiply(db,u),SilPomCurved_ProofJetMultiply(dc,v)));
}
void SilPomCurved_ProofSurface(SilPomCurved_Fragment f,float2 uRange,float2 vRange,out SilPomCurved_ProofJet position[3],out float2 qx,out float2 qy)
{
    SilPomCurved_ProofJet u=SilPomCurved_ProofConstant(0),v=u;u.value=uRange;u.du=1;v.value=vRange;v.dv=1;
    SilPomCurved_ProofJet x=SilPomCurved_ProofJetAdd(SilPomCurved_ProofJetMultiply(SilPomCurved_ProofAffine(f.uv[0].x,f.uv[1].x,f.uv[2].x,u,v),
        SilPomCurved_ProofConstant(float(textureWidth))),SilPomCurved_ProofJetAdd(SilPomCurved_ProofConstant(-.5f),SilPomCurved_ProofConstant(-f.cell.x)));
    SilPomCurved_ProofJet y=SilPomCurved_ProofJetAdd(SilPomCurved_ProofJetMultiply(SilPomCurved_ProofAffine(f.uv[0].y,f.uv[1].y,f.uv[2].y,u,v),
        SilPomCurved_ProofConstant(float(textureHeight))),SilPomCurved_ProofJetAdd(SilPomCurved_ProofConstant(-.5f),SilPomCurved_ProofConstant(-f.cell.y)));
    qx=x.value;qy=y.value;
    SilPomCurved_ProofJet h=SilPomCurved_ProofJetAdd(SilPomCurved_ProofConstant(f.height.x),SilPomCurved_ProofJetAdd(SilPomCurved_ProofJetMultiply(SilPomCurved_ProofConstant(f.height.y),x),
        SilPomCurved_ProofJetAdd(SilPomCurved_ProofJetMultiply(SilPomCurved_ProofConstant(f.height.z),y),SilPomCurved_ProofJetMultiply(SilPomCurved_ProofConstant(f.height.w),SilPomCurved_ProofJetMultiply(x,y)))));
    h=SilPomCurved_ProofJetMultiply(SilPomCurved_ProofJetAdd(h,SilPomCurved_ProofConstant(-reference)),SilPomCurved_ProofConstant(amplitude));
    [unroll] for(uint axis=0;axis!=3;++axis)
        position[axis]=SilPomCurved_ProofJetAdd(SilPomCurved_ProofJetMultiply(SilPomCurved_ProofAffine(f.position[0][axis],f.position[1][axis],f.position[2][axis],u,v),
            SilPomCurved_ProofConstant(baseScale)),SilPomCurved_ProofJetMultiply(SilPomCurved_ProofAffine(f.direction[0][axis],f.direction[1][axis],f.direction[2][axis],u,v),h));
}
void SilPomCurved_ProofProject(SilPomCurved_Fragment f,SilPomCurved_Ray ray,SilPomCurved_Projection projection,float2 u,float2 v,out SilPomCurved_ProofJet first,out SilPomCurved_ProofJet second)
{
    SilPomCurved_ProofJet position[3];float2 qx,qy;SilPomCurved_ProofSurface(f,u,v,position,qx,qy);
    SilPomCurved_ProofJet major=SilPomCurved_ProofJetMultiply(SilPomCurved_ProofJetAdd(position[projection.major],SilPomCurved_ProofConstant(-ray.origin[projection.major])),SilPomCurved_ProofConstant(-1));
    first=SilPomCurved_ProofJetAdd(SilPomCurved_ProofJetMultiply(SilPomCurved_ProofJetAdd(position[projection.first],SilPomCurved_ProofConstant(-ray.origin[projection.first])),
        SilPomCurved_ProofConstant(ray.direction[projection.major])),SilPomCurved_ProofJetMultiply(major,SilPomCurved_ProofConstant(ray.direction[projection.first])));
    second=SilPomCurved_ProofJetAdd(SilPomCurved_ProofJetMultiply(SilPomCurved_ProofJetAdd(position[projection.second],SilPomCurved_ProofConstant(-ray.origin[projection.second])),
        SilPomCurved_ProofConstant(ray.direction[projection.major])),SilPomCurved_ProofJetMultiply(major,SilPomCurved_ProofConstant(ray.direction[projection.second])));
}
float SilPomCurved_ProofMagnitude(float2 value) {return max(abs(value.x),abs(value.y));}

// 0: no proof, 1: unique root included, 2: no root in the whole child box.
uint SilPomCurved_ProofRegular(SilPomCurved_Fragment f,SilPomCurved_Ray ray,SilPomCurved_Projection projection,float3 a,float3 b,float3 c,
    out float3 barycentric,out float2 hitDepth)
{
    barycentric=0;hitDepth=0;
    float2 u=float2(min(a.y,min(b.y,c.y)),max(a.y,max(b.y,c.y)));
    float2 v=float2(min(a.z,min(b.z,c.z)),max(a.z,max(b.z,c.z)));
    float padding=max(u.y-u.x,v.y-v.x)*.125f;
    u=SilPomCurved_ProofAdd(u,float2(-padding,padding));v=SilPomCurved_ProofAdd(v,float2(-padding,padding));
    bool included=false;
    [loop] for(uint iteration=0;iteration!=8;++iteration)
    {
        SilPomCurved_ProofJet p,q;SilPomCurved_ProofProject(f,ray,projection,u,v,p,q);
        if(any(!isfinite(p.value))||any(!isfinite(p.du))||any(!isfinite(p.dv))||
            any(!isfinite(q.value))||any(!isfinite(q.du))||any(!isfinite(q.dv)))return 0;
        float a00=(p.du.x+p.du.y)*.5f,a01=(p.dv.x+p.dv.y)*.5f;
        float a10=(q.du.x+q.du.y)*.5f,a11=(q.dv.x+q.dv.y)*.5f;
        precise float determinant=a00*a11-a01*a10;
        if(!isfinite(determinant)||determinant==0)return 0;
        float r00=a11/determinant,r01=-a01/determinant,r10=-a10/determinant,r11=a00/determinant;
        float2 m00=SilPomCurved_ProofSubtract(SilPomCurved_ProofSubtract(float2(1,1),SilPomCurved_ProofMultiply(r00.xx,p.du)),SilPomCurved_ProofMultiply(r01.xx,q.du));
        float2 m01=SilPomCurved_ProofSubtract(-SilPomCurved_ProofMultiply(r00.xx,p.dv).yx,SilPomCurved_ProofMultiply(r01.xx,q.dv));
        float2 m10=SilPomCurved_ProofSubtract(-SilPomCurved_ProofMultiply(r10.xx,p.du).yx,SilPomCurved_ProofMultiply(r11.xx,q.du));
        float2 m11=SilPomCurved_ProofSubtract(SilPomCurved_ProofSubtract(float2(1,1),SilPomCurved_ProofMultiply(r10.xx,p.dv)),SilPomCurved_ProofMultiply(r11.xx,q.dv));
        float middleU=(u.x+u.y)*.5f,middleV=(v.x+v.y)*.5f;
        SilPomCurved_ProofJet centerP,centerQ;SilPomCurved_ProofProject(f,ray,projection,middleU.xx,middleV.xx,centerP,centerQ);
        float2 deltaU=SilPomCurved_ProofSubtract(u,middleU.xx),deltaV=SilPomCurved_ProofSubtract(v,middleV.xx);
        float2 ku=SilPomCurved_ProofAdd(SilPomCurved_ProofSubtract(SilPomCurved_ProofSubtract(middleU.xx,SilPomCurved_ProofMultiply(r00.xx,centerP.value)),SilPomCurved_ProofMultiply(r01.xx,centerQ.value)),
            SilPomCurved_ProofAdd(SilPomCurved_ProofMultiply(m00,deltaU),SilPomCurved_ProofMultiply(m01,deltaV)));
        float2 kv=SilPomCurved_ProofAdd(SilPomCurved_ProofSubtract(SilPomCurved_ProofSubtract(middleV.xx,SilPomCurved_ProofMultiply(r10.xx,centerP.value)),SilPomCurved_ProofMultiply(r11.xx,centerQ.value)),
            SilPomCurved_ProofAdd(SilPomCurved_ProofMultiply(m10,deltaU),SilPomCurved_ProofMultiply(m11,deltaV)));
        if(any(!isfinite(ku))||any(!isfinite(kv)))return 0;
        if(ku.x>u.y||ku.y<u.x||kv.x>v.y||kv.y<v.x)return included?0:2;
        bool contraction=SilPomCurved_ProofUp(SilPomCurved_ProofMagnitude(m00)+SilPomCurved_ProofMagnitude(m01))<1&&
            SilPomCurved_ProofUp(SilPomCurved_ProofMagnitude(m10)+SilPomCurved_ProofMagnitude(m11))<1;
        included=included||(contraction&&ku.x>u.x&&ku.y<u.y&&kv.x>v.x&&kv.y<v.y);
        if(!included)return 0;
        u=float2(max(u.x,ku.x),min(u.y,ku.y));v=float2(max(v.x,kv.x),min(v.y,kv.y));
        float2 w=SilPomCurved_ProofSubtract(SilPomCurved_ProofSubtract(float2(1,1),u),v);
        SilPomCurved_ProofJet position[3];float2 qx,qy;SilPomCurved_ProofSurface(f,u,v,position,qx,qy);
        hitDepth=SilPomCurved_ProofDivide(SilPomCurved_ProofSubtract(position[projection.major].value,ray.origin[projection.major].xx),ray.direction[projection.major]);
        float depthFloor=32.0f*1.192092896e-7f*max(1.0f,SilPomCurved_ProofMagnitude(hitDepth));
        if(u.x>=0&&v.x>=0&&w.x>=0&&u.y<=1&&v.y<=1&&w.y<=1&&
            qx.x>=0&&qx.y<=1&&qy.x>=0&&qy.y<=1&&
            hitDepth.x>=ray.tMin&&hitDepth.y<=ray.tMax&&all(isfinite(hitDepth))&&
            hitDepth.y-hitDepth.x<=max(requiredDepthAccuracy,depthFloor)&&max(u.y-u.x,v.y-v.x)<=5e-5f)
        {
            float rootU=(u.x+u.y)*.5f,rootV=(v.x+v.y)*.5f;
            barycentric=float3(1-rootU-rootV,rootU,rootV);return 1;
        }
    }
    return 0;
}

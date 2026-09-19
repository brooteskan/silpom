// SPDX-License-Identifier: MIT
// Fixed-cell interval evaluation from the transported binary32 inputs. Unlike
// bounds reconstructed from small child control nets, rounding error is carried
// through the source polynomial before multiplying by the inverse Jacobian.
float ProofDown(float value)
{
    if(abs(value)<1.175494351e-38f)return -1.175494351e-38f; // include shader flush-to-zero
    if(!isfinite(value))return value;
    return asfloat(value>0?asuint(value)-1:asuint(value)+1);
}
float ProofUp(float value)
{
    if(abs(value)<1.175494351e-38f)return 1.175494351e-38f;
    if(!isfinite(value))return value;
    return asfloat(value>0?asuint(value)+1:asuint(value)-1);
}
float2 ProofAdd(float2 a,float2 b)
{
    precise float lo=a.x+b.x,hi=a.y+b.y;
    return float2(ProofDown(lo),ProofUp(hi));
}
float2 ProofSubtract(float2 a,float2 b) {return ProofAdd(a,-b.yx);}
float2 ProofMultiply(float2 a,float2 b)
{
    precise float4 values=float4(a.x*b.x,a.x*b.y,a.y*b.x,a.y*b.y);
    return float2(ProofDown(min(min(values.x,values.y),min(values.z,values.w))),
        ProofUp(max(max(values.x,values.y),max(values.z,values.w))));
}
float2 ProofDivide(float2 a,float b)
{
    precise float lo=(b>0?a.x:a.y)/b,hi=(b>0?a.y:a.x)/b;
    return float2(ProofDown(lo),ProofUp(hi));
}
struct ProofJet {float2 value;float2 du;float2 dv;};
ProofJet ProofConstant(float value)
{
    ProofJet result;result.value=value.xx;result.du=0;result.dv=0;return result;
}
ProofJet ProofJetAdd(ProofJet a,ProofJet b)
{
    ProofJet result;result.value=ProofAdd(a.value,b.value);result.du=ProofAdd(a.du,b.du);result.dv=ProofAdd(a.dv,b.dv);return result;
}
ProofJet ProofJetMultiply(ProofJet a,ProofJet b)
{
    ProofJet result;result.value=ProofMultiply(a.value,b.value);
    result.du=ProofAdd(ProofMultiply(a.du,b.value),ProofMultiply(a.value,b.du));
    result.dv=ProofAdd(ProofMultiply(a.dv,b.value),ProofMultiply(a.value,b.dv));return result;
}
ProofJet ProofAffine(float a,float b,float c,ProofJet u,ProofJet v)
{
    ProofJet db=ProofConstant(0),dc=db;
    db.value=ProofSubtract(b.xx,a.xx);dc.value=ProofSubtract(c.xx,a.xx);
    return ProofJetAdd(ProofConstant(a),ProofJetAdd(ProofJetMultiply(db,u),ProofJetMultiply(dc,v)));
}
void ProofSurface(Fragment f,float2 uRange,float2 vRange,out ProofJet position[3],out float2 qx,out float2 qy)
{
    ProofJet u=ProofConstant(0),v=u;u.value=uRange;u.du=1;v.value=vRange;v.dv=1;
    ProofJet x=ProofJetAdd(ProofJetMultiply(ProofAffine(f.uv[0].x,f.uv[1].x,f.uv[2].x,u,v),
        ProofConstant(float(textureWidth))),ProofJetAdd(ProofConstant(-.5f),ProofConstant(-f.cell.x)));
    ProofJet y=ProofJetAdd(ProofJetMultiply(ProofAffine(f.uv[0].y,f.uv[1].y,f.uv[2].y,u,v),
        ProofConstant(float(textureHeight))),ProofJetAdd(ProofConstant(-.5f),ProofConstant(-f.cell.y)));
    qx=x.value;qy=y.value;
    ProofJet h=ProofJetAdd(ProofConstant(f.height.x),ProofJetAdd(ProofJetMultiply(ProofConstant(f.height.y),x),
        ProofJetAdd(ProofJetMultiply(ProofConstant(f.height.z),y),ProofJetMultiply(ProofConstant(f.height.w),ProofJetMultiply(x,y)))));
    h=ProofJetMultiply(ProofJetAdd(h,ProofConstant(-reference)),ProofConstant(amplitude));
    [unroll] for(uint axis=0;axis!=3;++axis)
        position[axis]=ProofJetAdd(ProofJetMultiply(ProofAffine(f.position[0][axis],f.position[1][axis],f.position[2][axis],u,v),
            ProofConstant(baseScale)),ProofJetMultiply(ProofAffine(f.direction[0][axis],f.direction[1][axis],f.direction[2][axis],u,v),h));
}
void ProofProject(Fragment f,Ray ray,Projection projection,float2 u,float2 v,out ProofJet first,out ProofJet second)
{
    ProofJet position[3];float2 qx,qy;ProofSurface(f,u,v,position,qx,qy);
    ProofJet major=ProofJetMultiply(ProofJetAdd(position[projection.major],ProofConstant(-ray.origin[projection.major])),ProofConstant(-1));
    first=ProofJetAdd(ProofJetMultiply(ProofJetAdd(position[projection.first],ProofConstant(-ray.origin[projection.first])),
        ProofConstant(ray.direction[projection.major])),ProofJetMultiply(major,ProofConstant(ray.direction[projection.first])));
    second=ProofJetAdd(ProofJetMultiply(ProofJetAdd(position[projection.second],ProofConstant(-ray.origin[projection.second])),
        ProofConstant(ray.direction[projection.major])),ProofJetMultiply(major,ProofConstant(ray.direction[projection.second])));
}
float ProofMagnitude(float2 value) {return max(abs(value.x),abs(value.y));}

// 0: no proof, 1: unique root included, 2: no root in the whole child box.
uint ProofRegular(Fragment f,Ray ray,Projection projection,float3 a,float3 b,float3 c,
    out float3 barycentric,out float2 hitDepth)
{
    barycentric=0;hitDepth=0;
    float2 u=float2(min(a.y,min(b.y,c.y)),max(a.y,max(b.y,c.y)));
    float2 v=float2(min(a.z,min(b.z,c.z)),max(a.z,max(b.z,c.z)));
    float padding=max(u.y-u.x,v.y-v.x)*.125f;
    u=ProofAdd(u,float2(-padding,padding));v=ProofAdd(v,float2(-padding,padding));
    bool included=false;
    [loop] for(uint iteration=0;iteration!=8;++iteration)
    {
        ProofJet p,q;ProofProject(f,ray,projection,u,v,p,q);
        if(any(!isfinite(p.value))||any(!isfinite(p.du))||any(!isfinite(p.dv))||
            any(!isfinite(q.value))||any(!isfinite(q.du))||any(!isfinite(q.dv)))return 0;
        float a00=(p.du.x+p.du.y)*.5f,a01=(p.dv.x+p.dv.y)*.5f;
        float a10=(q.du.x+q.du.y)*.5f,a11=(q.dv.x+q.dv.y)*.5f;
        precise float determinant=a00*a11-a01*a10;
        if(!isfinite(determinant)||determinant==0)return 0;
        float r00=a11/determinant,r01=-a01/determinant,r10=-a10/determinant,r11=a00/determinant;
        float2 m00=ProofSubtract(ProofSubtract(float2(1,1),ProofMultiply(r00.xx,p.du)),ProofMultiply(r01.xx,q.du));
        float2 m01=ProofSubtract(-ProofMultiply(r00.xx,p.dv).yx,ProofMultiply(r01.xx,q.dv));
        float2 m10=ProofSubtract(-ProofMultiply(r10.xx,p.du).yx,ProofMultiply(r11.xx,q.du));
        float2 m11=ProofSubtract(ProofSubtract(float2(1,1),ProofMultiply(r10.xx,p.dv)),ProofMultiply(r11.xx,q.dv));
        float middleU=(u.x+u.y)*.5f,middleV=(v.x+v.y)*.5f;
        ProofJet centerP,centerQ;ProofProject(f,ray,projection,middleU.xx,middleV.xx,centerP,centerQ);
        float2 deltaU=ProofSubtract(u,middleU.xx),deltaV=ProofSubtract(v,middleV.xx);
        float2 ku=ProofAdd(ProofSubtract(ProofSubtract(middleU.xx,ProofMultiply(r00.xx,centerP.value)),ProofMultiply(r01.xx,centerQ.value)),
            ProofAdd(ProofMultiply(m00,deltaU),ProofMultiply(m01,deltaV)));
        float2 kv=ProofAdd(ProofSubtract(ProofSubtract(middleV.xx,ProofMultiply(r10.xx,centerP.value)),ProofMultiply(r11.xx,centerQ.value)),
            ProofAdd(ProofMultiply(m10,deltaU),ProofMultiply(m11,deltaV)));
        if(any(!isfinite(ku))||any(!isfinite(kv)))return 0;
        if(ku.x>u.y||ku.y<u.x||kv.x>v.y||kv.y<v.x)return included?0:2;
        bool contraction=ProofUp(ProofMagnitude(m00)+ProofMagnitude(m01))<1&&
            ProofUp(ProofMagnitude(m10)+ProofMagnitude(m11))<1;
        included=included||(contraction&&ku.x>u.x&&ku.y<u.y&&kv.x>v.x&&kv.y<v.y);
        if(!included)return 0;
        u=float2(max(u.x,ku.x),min(u.y,ku.y));v=float2(max(v.x,kv.x),min(v.y,kv.y));
        float2 w=ProofSubtract(ProofSubtract(float2(1,1),u),v);
        ProofJet position[3];float2 qx,qy;ProofSurface(f,u,v,position,qx,qy);
        hitDepth=ProofDivide(ProofSubtract(position[projection.major].value,ray.origin[projection.major].xx),ray.direction[projection.major]);
        float depthFloor=32.0f*1.192092896e-7f*max(1.0f,ProofMagnitude(hitDepth));
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

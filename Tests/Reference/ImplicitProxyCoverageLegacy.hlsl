// SPDX-License-Identifier: MIT
// Frozen pre-cache coverage expression and per-vertex candidate selection.
// Only parameter/return plumbing changed to make it independently dispatchable.
// Shared output coordinates are precise because the original shader's
// precise SV_Position propagates that contract through this exact expression.
#define SILPOM_TOWER_LOD_NEAR 1
#define SILPOM_NEURAL_NO_MARCH 1
#define SILPOM_HIT_FORWARD 0
static const uint SP_TOWER_LOD_4PX_RESOLVE=65525u;
static const uint SP_TOWER_RECTANGLE_CONTROL=65531u;
bool SilPomTowerNoMarch(uint mode){return mode==65534u||mode==65531u||mode==65530u||(mode>=65525u&&mode<=65529u);}
bool SilPomIsTowerWallModel(uint model){return model==1u||model==4u;}
float SilPomLegacyLodDepth(SilPomImplicitProfile profile,float projectionY)
{
    if(profile.proxyLodEnabled)return 1e20f;
    if(profile.maxCells==65526u)return 0;
    const uint mode=profile.maxCells==65528u||profile.maxCells==65525u?65528u:profile.maxCells;
    const float budget=2.0f*float(1u<<(65529u-mode));
    return SilPomQualityLodDepth(profile.scale,profile.reference,projectionY,budget,profile.pixelBudget,profile.maxDistance);
}
#if SILPOM_NEURAL_NO_MARCH || SILPOM_HIT_FORWARD
// With all six extrema in front of the near plane, their projected convex
// hull bounds the displaced surface. Move the three barycentric half-planes
// out to enclose that hull with one triangle.
bool SilPomTowerCoverage(float4 corners[6], float2 quadCorner, inout float proxyArea, out float2 ndc)
{
    ndc=0;
    const float2 origin=corners[0].xy/corners[0].w;
    const float2 edgeB=corners[2].xy/corners[2].w-origin;
    const float2 edgeC=corners[4].xy/corners[4].w-origin;
    const float area=edgeB.x*edgeC.y-edgeB.y*edgeC.x;
    if(abs(area)<1e-6f) return false;

    const float2 gradB=float2(edgeC.y,-edgeC.x)/area;
    const float2 gradC=float2(-edgeB.y,edgeB.x)/area;
    float3 lower=0;
    [unroll] for(uint i=1;i<6;i+=2)
    {
        const float2 delta=corners[i].xy/corners[i].w-origin;
        const float b=dot(delta,gradB),c=dot(delta,gradC);
        lower=min(lower,float3(1.0f-b-c,b,c));
    }
    const float2 gradA=gradB+gradC;
    lower-=2e-5f*float3(abs(gradA.x)+abs(gradA.y),abs(gradB.x)+abs(gradB.y),abs(gradC.x)+abs(gradC.y));
    const float scale=1.0f-lower.x-lower.y-lower.z;
    const float triangleArea=.5f*abs(area)*scale*scale;
    if(triangleArea>=proxyArea) return false;
    proxyArea=triangleArea;

    // Keep the rectangle's winding and collapse its second triangle.
    const bool useB=area>0.0f ? quadCorner.x>0 && quadCorner.y<=0 : quadCorner.y>0;
    const bool useC=area>0.0f ? quadCorner.y>0 : quadCorner.x>0 && quadCorner.y<=0;
    ndc=origin+edgeB*(lower.y+(useB?scale:0.0f))+edgeC*(lower.z+(useC?scale:0.0f));
    return true;
}
#endif
#if SILPOM_TOWER_LOD_NEAR
struct SilPomWallProxyBounds
{
    float3 axisX, axisY;
    float3 minU, minV, maxU, maxV;
};

float2 SilPomProjectedEdge(float4 a, float4 b)
{
    // Use the homogeneous line so the axis remains defined across the near cut.
    const float2 edge=b.xy*a.w-a.xy*b.w;
    const float lengthSq=dot(edge,edge);
    return lengthSq>1e-12f ? edge*rsqrt(lengthSq) : float2(1,0);
}

void SilPomGrowWallProxy(inout SilPomWallProxyBounds bounds, float2 xy)
{
    const float3 u=bounds.axisX*xy.x+bounds.axisY*xy.y;
    const float3 v=bounds.axisX*xy.y-bounds.axisY*xy.x;
    bounds.minU=min(bounds.minU,u); bounds.maxU=max(bounds.maxU,u);
    bounds.minV=min(bounds.minV,v); bounds.maxV=max(bounds.maxV,v);
}

float SilPomVisibleQuadArea(float2 origin, float2 u, float2 v)
{
    float2 polygon[8];
    polygon[0]=origin; polygon[1]=origin+u;
    polygon[2]=origin+u+v; polygon[3]=origin+v;
    uint count=4;
    // A convex quadrilateral clipped to the viewport has at most eight vertices.
    [loop] for(uint plane=0;plane<4;++plane)
    {
        float2 clipped[8];
        uint nextCount=0;
        const uint axis=plane>>1;
        const float sign=(plane&1)==0 ? 1.0f : -1.0f;
        float2 a=polygon[count-1];
        float da=1.0f-sign*a[axis];
        [loop] for(uint i=0;i<count;++i)
        {
            const float2 b=polygon[i];
            const float db=1.0f-sign*b[axis];
            if((da<0)!=(db<0)) clipped[nextCount++]=lerp(a,b,da/(da-db));
            if(db>=0) clipped[nextCount++]=b;
            a=b; da=db;
        }
        if(nextCount<3) return 0;
        count=nextCount;
        [loop] for(uint i=0;i<count;++i) polygon[i]=clipped[i];
    }
    float twiceArea=0;
    float2 a=polygon[count-1];
    [loop] for(uint i=0;i<count;++i)
    {
        const float2 b=polygon[i];
        twiceArea+=a.x*b.y-a.y*b.x;
        a=b;
    }
    return .5f*abs(twiceArea);
}

float2 SilPomChooseWallProxy(SilPomWallProxyBounds bounds, float2 corner, float area, float2 ndc)
{
    [loop] for(uint i=0;i<3;++i)
    {
        const float2 axisU=float2(bounds.axisX[i],bounds.axisY[i]);
        const float2 axisV=float2(-axisU.y,axisU.x);
        const float2 lo=float2(bounds.minU[i],bounds.minV[i])-2e-5f;
        const float2 hi=float2(bounds.maxU[i],bounds.maxV[i])+2e-5f;
        const float2 origin=lo.x*axisU+lo.y*axisV;
        const float2 u=(hi.x-lo.x)*axisU, v=(hi.y-lo.y)*axisV;
        const float visibleArea=SilPomVisibleQuadArea(origin,u,v);
        if(visibleArea<area)
        {
            area=visibleArea;
            ndc=origin+corner.x*u+corner.y*v;
        }
    }
    return ndc;
}
#endif
SilPomImplicitCoverage SilPomLegacyImplicitCoverage(SilPomMeshFace f,SilPomImplicitProfile profile,
    float4x4 model,float4x4 vp,float projectionY,bool proxySelected,float2 quadCorner)
{
    SilPomImplicitCoverage result=(SilPomImplicitCoverage)0;result.status=SP_IMPLICIT_CACHE_EMPTY;
    if(proxySelected)return result;
    float2 low=float2(1,1),high=float2(-1,-1);
    float4 corners[6];
    bool clipped=false;
#if SILPOM_TOWER_LOD_NEAR
    const float cutoff=SilPomLegacyLodDepth(profile,projectionY);
    if(cutoff<=0)
    {
        return result;
    }
    const float nearLimit=cutoff+SilPomQualityLodOverlap(profile.scale,profile.maxDistance);
#endif
    for(uint i=0;i<6;++i)
    {
        float2 uv=i/2==0?f.uv0:(i/2==1?f.uv1:f.uv2);
        float h=profile.scale*((i&1)-profile.reference);
        float3 D=i/2==0?f.d0:(i/2==1?f.d1:f.d2);
        float3 p=f.origin+f.du*(uv.x-f.uv0.x)+f.dv*(uv.y-f.uv0.y)+D*h;
        corners[i]=mul(vp,mul(model,float4(p,1)));
    }
#if SILPOM_TOWER_LOD_NEAR
    const bool tightWall=SilPomIsTowerWallModel(profile.neuralModel) && profile.maxCells==SP_TOWER_LOD_4PX_RESOLVE;
    SilPomWallProxyBounds wallBounds=(SilPomWallProxyBounds)0;
    if(tightWall)
    {
        const float2 a=SilPomProjectedEdge(corners[0],corners[2]);
        const float2 b=SilPomProjectedEdge(corners[2],corners[4]);
        const float2 c=SilPomProjectedEdge(corners[4],corners[0]);
        wallBounds.axisX=float3(a.x,b.x,c.x); wallBounds.axisY=float3(a.y,b.y,c.y);
        wallBounds.minU=wallBounds.minV=1e30f;
        wallBounds.maxU=wallBounds.maxV=-1e30f;
    }
#endif
    for(uint i=0;i<6;++i)
    {
        const float4 clipPosition=corners[i];
        if(clipPosition.w>1e-6 && clipPosition.z>=0 && clipPosition.z<=clipPosition.w
#if SILPOM_TOWER_LOD_NEAR
            && clipPosition.w<=nearLimit
#endif
        )
        {
            float2 xy=clipPosition.xy/clipPosition.w;
            low=min(low,xy); high=max(high,xy);
#if SILPOM_TOWER_LOD_NEAR
            if(tightWall) SilPomGrowWallProxy(wallBounds,xy);
#endif
        }
        else
            clipped=true;
    }
    if(clipped)
    {
        // The relief is enclosed by the convex hull of these six positions.
        // Intersect every corner pair to include all hull edges, even when the
        // varying displacement directions make the hull non-prismatic.
        // Atom uses reverse depth. The near plane is w-z=0 and the far plane is z=0.
        for(uint i=0;i<6;++i)
        {
            float4 a=corners[i];
            for(uint j=i+1;j<6;++j)
            {
                float4 b=corners[j];
#if SILPOM_TOWER_LOD_NEAR
                const uint planeCount=3;
#else
                const uint planeCount=2;
#endif
                for(uint plane=0;plane<planeCount;++plane)
                {
                    float da=plane==0?a.w-a.z:a.z;
                    float db=plane==0?b.w-b.z:b.z;
#if SILPOM_TOWER_LOD_NEAR
                    if(plane==2)
                    {
                        da=nearLimit-a.w;
                        db=nearLimit-b.w;
                    }
#endif
                    if((da<0)!=(db<0))
                    {
                        float4 edge=lerp(a,b,da/(da-db));
                        // At either cut, w>0 also places the point inside the opposite plane.
                        if(edge.w>1e-6
#if SILPOM_TOWER_LOD_NEAR
                            && (plane==2 || edge.w<=nearLimit)
#endif
                        )
                        {
                            float2 xy=edge.xy/edge.w;
                            low=min(low,xy); high=max(high,xy);
#if SILPOM_TOWER_LOD_NEAR
                            if(tightWall) SilPomGrowWallProxy(wallBounds,xy);
#endif
                        }
                    }
                }
            }
        }
    }
    if(any(low>high))
    {
        return result;
    }
    low=max(low-1e-5,float2(-1,-1)); high=min(high+1e-5,float2(1,1));
    const float2 corner=quadCorner*.5+.5;
    result.ndc[0]=lerp(low,high,corner);
#if SILPOM_NEURAL_NO_MARCH || SILPOM_HIT_FORWARD
    const float2 extent=max(high-low,0.0f);
    float proxyArea=extent.x*extent.y;
    float2 triangleNdc;
    if(!clipped && SilPomTowerNoMarch(profile.maxCells) && profile.maxCells!=SP_TOWER_RECTANGLE_CONTROL
        && SilPomTowerCoverage(corners,quadCorner,proxyArea,triangleNdc))
    {
        result.ndc[0]=triangleNdc;
    }
#endif
#if SILPOM_TOWER_LOD_NEAR
    if(tightWall && proxyArea>0)
        result.ndc[0]=SilPomChooseWallProxy(wallBounds,corner,proxyArea,result.ndc[0]);
#endif
    result.status=SP_IMPLICIT_CACHE_LIVE;return result;
}
void SilPomLegacyFaceBounds(SilPomMeshFace f,float scale,float reference,out float3 boundsLow,out float3 boundsHigh)
{
    boundsLow=float3(1e30f,1e30f,1e30f);boundsHigh=float3(-1e30f,-1e30f,-1e30f);
    for(uint i=0;i<6;++i)
    {
        float2 uv=i/2==0?f.uv0:(i/2==1?f.uv1:f.uv2);
        float3 D=i/2==0?f.d0:(i/2==1?f.d1:f.d2);
        float h=scale*(float(i%2)-reference);
        float3 cornerPosition=f.origin+f.du*(uv.x-f.uv0.x)+f.dv*(uv.y-f.uv0.y)+D*h;
        boundsLow=min(boundsLow,cornerPosition);boundsHigh=max(boundsHigh,cornerPosition);
    }
}
float SilPomLegacyNearestDepth(SilPomMeshFace face,SilPomImplicitProfile profile,float4x4 model,float4x4 vp)
{
    float3 low, high;
    SilPomLegacyFaceBounds(face, profile.scale, profile.reference, low, high);

    // Include the expanded AABB used by the ray test, so fallback depths are
    // bounded too. The projection is reverse Z, so the nearest depth is largest.
    const float3 extent = (high - low) * .5f + 2e-5f;
    const float4x4 localToClip = mul(vp, model);
    const float4 center = mul(localToClip, float4((high + low) * .5f, 1.0f));
    const float minW = center.w - dot(abs(localToClip[3].xyz), extent);
    const float maxZ = center.z + dot(abs(localToClip[2].xyz), extent);
    const float legacy = minW > 1e-6f ? saturate(maxZ / minW + 1e-5f) : 1.0f;
    if (legacy < 1.0f) return legacy;
    return SilPomFrustumDepthBound((high + low) * .5f, extent,
        model, vp);
}

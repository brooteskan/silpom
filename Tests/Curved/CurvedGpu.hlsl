// SPDX-License-Identifier: MIT
// Standalone feasibility shader. Its buffer layout is test-only and is not an asset ABI.
Texture2D<float> heightImage : register(t0);

struct Ray
{
    float3 origin;
    float3 direction;
    float tMin;
    float tMax;
};
StructuredBuffer<Ray> rays : register(t1);
StructuredBuffer<float4> fragmentData : register(t2);

struct Result
{
    uint status;
    uint primitiveId;
    uint nodes;
    uint maximumDepth;
    float t;
    float tError;
    float3 barycentric;
    float unresolvedT;
    uint singular;
};
RWStructuredBuffer<Result> results : register(u0);

cbuffer Parameters : register(b0)
{
    float baseScale;
    float amplitude;
    float reference;
    uint addressMode;
    uint textureWidth;
    uint textureHeight;
    uint fragmentCount;
    uint maxNodes;
    uint maxDepth;
    uint rayCount;
    uint firstRay;
    uint reserved0;
    uint reserved1;
};

static const uint Miss = 0;
static const uint Hit = 1;
static const uint Exhausted = 2;
static const uint Invalid = 3;

int Address(int value, int size)
{
    return addressMode == 0 ? ((value % size) + size) % size : clamp(value, 0, size - 1);
}

float Height(float2 uv)
{
    float2 q = uv * float2(textureWidth, textureHeight) - .5f;
    int2 cell = int2(floor(q));
    float2 f = q - cell;
    float h00 = heightImage.Load(int3(Address(cell.x, textureWidth), Address(cell.y, textureHeight), 0));
    float h10 = heightImage.Load(int3(Address(cell.x + 1, textureWidth), Address(cell.y, textureHeight), 0));
    float h01 = heightImage.Load(int3(Address(cell.x, textureWidth), Address(cell.y + 1, textureHeight), 0));
    float h11 = heightImage.Load(int3(Address(cell.x + 1, textureWidth), Address(cell.y + 1, textureHeight), 0));
    return lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
}

struct Fragment
{
    float3 position[3];
    float3 direction[3];
    float2 uv[3];
    float3 domain[3];
    uint primitiveId;
};

Fragment LoadFragment(uint index)
{
    uint offset = index * 11;
    Fragment f;
    f.position[0] = fragmentData[offset + 0].xyz;
    f.position[1] = fragmentData[offset + 1].xyz;
    f.position[2] = fragmentData[offset + 2].xyz;
    f.direction[0] = fragmentData[offset + 3].xyz;
    f.direction[1] = fragmentData[offset + 4].xyz;
    f.direction[2] = fragmentData[offset + 5].xyz;
    float4 uv01 = fragmentData[offset + 6];
    f.uv[0] = uv01.xy;
    f.uv[1] = uv01.zw;
    f.uv[2] = fragmentData[offset + 7].xy;
    f.primitiveId = asuint(fragmentData[offset + 7].z);
    f.domain[0] = fragmentData[offset + 8].xyz;
    f.domain[1] = fragmentData[offset + 9].xyz;
    f.domain[2] = fragmentData[offset + 10].xyz;
    return f;
}

float3 SurfacePoint(Fragment f, float3 barycentric)
{
    float3 p = f.position[0] * barycentric.x + f.position[1] * barycentric.y + f.position[2] * barycentric.z;
    float3 d = f.direction[0] * barycentric.x + f.direction[1] * barycentric.y + f.direction[2] * barycentric.z;
    float2 uv = f.uv[0] * barycentric.x + f.uv[1] * barycentric.y + f.uv[2] * barycentric.z;
    return baseScale * p + amplitude * (Height(uv) - reference) * d;
}

float3 DomainPoint(float3 a, float3 b, float3 c, float3 local)
{
    return a * local.x + b * local.y + c * local.z;
}

struct Projection { uint major; uint first; uint second; };
Projection MakeProjection(float3 direction)
{
    Projection p;
    p.major=2;p.first=0;p.second=1;
    if (abs(direction.x) >= abs(direction.y) && abs(direction.x) >= abs(direction.z))
    {p.major=0;p.first=1;p.second=2;}
    else if (abs(direction.y) >= abs(direction.z))
    {p.major=1;p.first=0;p.second=2;}
    return p;
}

float3 Project(Ray ray, Projection p, float3 position)
{
    float3 relative = position - ray.origin;
    return float3(ray.direction[p.major] * relative[p.first] - ray.direction[p.first] * relative[p.major],
        ray.direction[p.major] * relative[p.second] - ray.direction[p.second] * relative[p.major],
        relative[p.major] / ray.direction[p.major]);
}

void Include(inout float3 lo, inout float3 hi, float3 value)
{
    lo = min(lo, value);
    hi = max(hi, value);
}

void EdgeControls(inout float3 lo, inout float3 hi, float3 p0, float3 p3, float3 p13, float3 p23,
    out float3 c1, out float3 c2)
{
    float3 a = 27.0f * p13 - 8.0f * p0 - p3;
    float3 b = 27.0f * p23 - p0 - 8.0f * p3;
    c1 = (2.0f * a - b) / 18.0f;
    c2 = (2.0f * b - a) / 18.0f;
    Include(lo, hi, c1);
    Include(lo, hi, c2);
}

void PatchBounds(Fragment fragment, Ray ray, Projection projection, float3 a, float3 b, float3 c,
    out float3 lo, out float3 hi)
{
    #define EVAL(local) Project(ray, projection, SurfacePoint(fragment, DomainPoint(a,b,c,local)))
    float3 p0 = EVAL(float3(1,0,0));
    float3 p1 = EVAL(float3(0,1,0));
    float3 p2 = EVAL(float3(0,0,1));
    lo = min(p0, min(p1, p2));
    hi = max(p0, max(p1, p2));
    float3 e01a = EVAL(float3(2.0f/3.0f,1.0f/3.0f,0));
    float3 e01b = EVAL(float3(1.0f/3.0f,2.0f/3.0f,0));
    float3 e12a = EVAL(float3(0,2.0f/3.0f,1.0f/3.0f));
    float3 e12b = EVAL(float3(0,1.0f/3.0f,2.0f/3.0f));
    float3 e20a = EVAL(float3(1.0f/3.0f,0,2.0f/3.0f));
    float3 e20b = EVAL(float3(2.0f/3.0f,0,1.0f/3.0f));
    float3 c01a,c01b,c12a,c12b,c20a,c20b;
    EdgeControls(lo, hi, p0, p1, e01a, e01b, c01a, c01b);
    EdgeControls(lo, hi, p1, p2, e12a, e12b, c12a, c12b);
    EdgeControls(lo, hi, p2, p0, e20a, e20b, c20a, c20b);
    float3 center = EVAL(float3(1.0f/3.0f,1.0f/3.0f,1.0f/3.0f));
    float3 centerControl=4.5f*(center-(p0+p1+p2)/27.0f-(c01a+c01b+c12a+c12b+c20a+c20b)/9.0f);
    Include(lo, hi, centerControl);
    float outward = max(1e-6f, 64.0f * 1.192092896e-7f * max(1.0f, max(max(abs(lo.x),abs(hi.x)),
        max(max(abs(lo.y),abs(hi.y)),max(abs(lo.z),abs(hi.z))))));
    lo -= outward;
    hi += outward;
    #undef EVAL
}

float3 DeflatedValue(Fragment fragment, Ray ray, Projection projection, float3 barycentric)
{
    float3 value=Project(ray,projection,SurfacePoint(fragment,barycentric));
    const float epsilon=1e-3f;
    float3 bu=barycentric+float3(-epsilon,epsilon,0);
    float3 bv=barycentric+float3(-epsilon,0,epsilon);
    float2 ju=(Project(ray,projection,SurfacePoint(fragment,bu)).xy-value.xy)/epsilon;
    float2 jv=(Project(ray,projection,SurfacePoint(fragment,bv)).xy-value.xy)/epsilon;
    float scale=max(1.0f,max(max(abs(ju.x),abs(ju.y)),max(abs(jv.x),abs(jv.y))));
    return float3(value.xy,(ju.x*jv.y-jv.x*ju.y)/(scale*scale));
}

bool Newton(Fragment fragment, Ray ray, Projection projection, float3 a, float3 b, float3 c,
    out float t, out float3 barycentric, out bool singular)
{
    float3 local = float3(1.0f/3.0f,1.0f/3.0f,1.0f/3.0f);
    singular = false;
    const float epsilon = 2e-4f;
    [loop] for (uint iteration = 0; iteration != 20; ++iteration)
    {
        barycentric = DomainPoint(a,b,c,local);
        float3 value = Project(ray, projection, SurfacePoint(fragment,barycentric));
        float3 lu = float3(local.x-epsilon,local.y+epsilon,local.z);
        float3 lv = float3(local.x-epsilon,local.y,local.z+epsilon);
        float2 ju = (Project(ray,projection,SurfacePoint(fragment,DomainPoint(a,b,c,lu))).xy-value.xy)/epsilon;
        float2 jv = (Project(ray,projection,SurfacePoint(fragment,DomainPoint(a,b,c,lv))).xy-value.xy)/epsilon;
        float determinant = ju.x*jv.y-jv.x*ju.y;
        float scale = max(1e-20f,max(max(abs(ju.x),abs(ju.y)),max(abs(jv.x),abs(jv.y))));
        if(abs(determinant)<2e-5f*scale*scale) {singular=true;break;}
        float du=(-value.x*jv.y+jv.x*value.y)/determinant;
        float dv=(-ju.x*value.y+value.x*ju.y)/determinant;
        local.y+=du;local.z+=dv;local.x=1.0f-local.y-local.z;
        if(max(abs(du),abs(dv))<2e-6f) break;
    }
    singular=singular||abs(DeflatedValue(fragment,ray,projection,DomainPoint(a,b,c,local)).z)<2e-2f;
    if(singular)
    {
        [loop] for(uint iteration=0;iteration!=20;++iteration)
        {
            float3 residual=DeflatedValue(fragment,ray,projection,DomainPoint(a,b,c,local));
            const float step=1e-2f;
            float3 luPlus=local+float3(-step,step,0),luMinus=local+float3(step,-step,0);
            float3 lvPlus=local+float3(-step,0,step),lvMinus=local+float3(step,0,-step);
            float3 du=(DeflatedValue(fragment,ray,projection,DomainPoint(a,b,c,luPlus))-
                DeflatedValue(fragment,ray,projection,DomainPoint(a,b,c,luMinus)))/(2.0f*step);
            float3 dv=(DeflatedValue(fragment,ray,projection,DomainPoint(a,b,c,lvPlus))-
                DeflatedValue(fragment,ray,projection,DomainPoint(a,b,c,lvMinus)))/(2.0f*step);
            float aa=dot(du,du)+1e-20f,ab=dot(du,dv),bb=dot(dv,dv)+1e-20f;
            float ga=dot(du,residual),gb=dot(dv,residual),determinant=aa*bb-ab*ab;
            if(abs(determinant)<1e-30f)break;
            float deltaU=(-bb*ga+ab*gb)/determinant;
            float deltaV=(ab*ga-aa*gb)/determinant;
            local.y+=deltaU;local.z+=deltaV;local.x=1.0f-local.y-local.z;
            if(max(abs(deltaU),abs(deltaV))<2e-5f)break;
        }
    }
    barycentric=DomainPoint(a,b,c,local);
    float3 value=Project(ray,projection,SurfacePoint(fragment,barycentric));
    t=value.z;
    float determinantResidual=abs(DeflatedValue(fragment,ray,projection,barycentric).z);
    return all(local>=-2e-4f) && all(local<=1.0002f) && max(abs(value.x),abs(value.y))<=2e-4f &&
        (!singular||determinantResidual<=2e-3f) &&
        t>=ray.tMin-2e-5f && t<=ray.tMax+2e-5f;
}

[numthreads(64,1,1)]
void Main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint rayIndex=dispatchId.x+firstRay;
    if(rayIndex>=rayCount) return;
    Ray ray=rays[rayIndex];
    Result result=(Result)0;
    result.status=Miss;
    result.primitiveId=0xffffffffu;
    result.t=ray.tMax;
    result.tError=1e-2f;
    result.unresolvedT=3.402823466e+38f;
    if(any(!isfinite(ray.origin))||any(!isfinite(ray.direction))||dot(ray.direction,ray.direction)==0||ray.tMin>ray.tMax)
    {result.status=Invalid;results[rayIndex]=result;return;}
    Projection projection=MakeProjection(ray.direction);
    bool exhausted=false;
    [loop] for(uint fragmentIndex=0;fragmentIndex<fragmentCount;++fragmentIndex)
    {
        Fragment fragment=LoadFragment(fragmentIndex);
        float3 stackA[128],stackB[128],stackC[128];uint stackDepth[128];
        uint stackSize=1;
        stackA[0]=fragment.domain[0];stackB[0]=fragment.domain[1];stackC[0]=fragment.domain[2];stackDepth[0]=0;
        [loop] while(stackSize)
        {
            if(result.nodes>=maxNodes){exhausted=true;break;}
            --stackSize;float3 a=stackA[stackSize],b=stackB[stackSize],c=stackC[stackSize];uint depth=stackDepth[stackSize];
            ++result.nodes;result.maximumDepth=max(result.maximumDepth,depth);
            float3 lo,hi;PatchBounds(fragment,ray,projection,a,b,c,lo,hi);
            if(lo.x>0||hi.x<0||lo.y>0||hi.y<0||hi.z<ray.tMin||lo.z>ray.tMax||lo.z>result.t+result.tError)continue;
            float diameter=max(length(a-b),max(length(b-c),length(c-a)));
            if(depth>=maxDepth||diameter<2e-3f||hi.z-lo.z<2e-4f)
            {
                float t;float3 barycentric;bool singular;
                if(Newton(fragment,ray,projection,a,b,c,t,barycentric,singular))
                {
                    if(t<result.t-result.tError||(abs(t-result.t)<=result.tError&&fragment.primitiveId<result.primitiveId))
                    {result.status=Hit;result.t=t;result.tError=1e-2f;
                        result.barycentric=barycentric;result.primitiveId=fragment.primitiveId;result.singular=singular;}
                }
                else {exhausted=true;result.unresolvedT=min(result.unresolvedT,lo.z);}
                continue;
            }
            if(stackSize+4>128){exhausted=true;result.unresolvedT=min(result.unresolvedT,lo.z);break;}
            float3 ab=(a+b)*.5f,bc=(b+c)*.5f,ca=(c+a)*.5f;
            stackA[stackSize]=a;stackB[stackSize]=ab;stackC[stackSize]=ca;stackDepth[stackSize++]=depth+1;
            stackA[stackSize]=ab;stackB[stackSize]=b;stackC[stackSize]=bc;stackDepth[stackSize++]=depth+1;
            stackA[stackSize]=ca;stackB[stackSize]=bc;stackC[stackSize]=c;stackDepth[stackSize++]=depth+1;
            stackA[stackSize]=ab;stackB[stackSize]=bc;stackC[stackSize]=ca;stackDepth[stackSize++]=depth+1;
        }
        if(exhausted&&result.unresolvedT<result.t-result.tError)break;
    }
    if(exhausted&&(result.status!=Hit||result.unresolvedT<result.t-result.tError))result.status=Exhausted;
    results[rayIndex]=result;
}

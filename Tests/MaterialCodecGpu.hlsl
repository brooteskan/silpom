// SPDX-License-Identifier: MIT
struct Query {float x[12];};
StructuredBuffer<Query> queries:register(t1);
StructuredBuffer<float> decoder:register(t2);
#define SP_OUT out
#define SP_UNROLL [unroll]
#define SILPOM_MATERIAL_WEIGHT(i) decoder[i]
#include "../Assets/Shaders/SilPOM/MaterialDecode.azsli"
struct Result {float value[12];};
RWStructuredBuffer<Result> results:register(u0);
cbuffer Params:register(b0){uint count;}
[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=count)return;
    Query q=queries[id.x];float value[6];SilPomDecodeMaterial(q.x,0,value);
    SilPomMaterialFootprint f=SilPomMaterialFootprintFor(float2(q.x[0],q.x[1])*16,float2(-q.x[1],q.x[0]));
    Result r;[unroll]for(uint i=0;i<6;++i)r.value[i]=value[i];
    r.value[6]=f.major;r.value[7]=f.minor;r.value[8]=f.lod;
    r.value[9]=f.axis.x*f.axis.x-f.axis.y*f.axis.y;r.value[10]=2*f.axis.x*f.axis.y;r.value[11]=float(f.taps);
    results[id.x]=r;
}

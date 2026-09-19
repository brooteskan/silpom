// SPDX-License-Identifier: MIT
#include "CpuShader.h"
#include "Curved/CurvedPrototype.h"
#include "Curved/CurvedPacking.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
using namespace SilPOM::Cpu;
static void Check(HRESULT hr) {if(FAILED(hr)) throw std::runtime_error("D3D12 HRESULT "+std::to_string(static_cast<unsigned>(hr)));}
static std::vector<char> Read(const char* path)
{std::ifstream f(path,std::ios::binary);if(!f) throw std::runtime_error(path);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv)
{
    try
    {
        if(argc!=4) throw std::runtime_error("Usage: silpom_gpu_tests compute.dxil rays.dxil curved.dxil");
        static_assert(sizeof(SilPomPatch)==48 && sizeof(SilPomRay)==32 && sizeof(SilPomHit)==44);
        ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D12Device5> device;
        for(UINT i=0;factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i)
        {
            DXGI_ADAPTER_DESC1 description{};adapter->GetDesc1(&description);
            if(!(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))
            {std::wcout<<L"GPU: "<<description.Description<<L'\n';break;}
            adapter.Reset();
        }
        if(!device) throw std::runtime_error("No hardware D3D12 device");
        ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC queueDesc{};Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList4> list;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);UINT64 fenceValue=0;
        auto submit=[&]()
        {
            Check(list->Close());ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
            Check(queue->Signal(fence.Get(),++fenceValue));Check(fence->SetEventOnCompletion(fenceValue,event));
            if(WaitForSingleObject(event,60000)!=WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
            Check(allocator->Reset());Check(list->Reset(allocator.Get(),nullptr));
        };
        auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            D3D12_HEAP_PROPERTIES props{};props.Type=heap;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;
            desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
            ComPtr<ID3D12Resource> resource;Check(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)));return resource;
        };
        auto upload=[&](const void* data,UINT64 bytes)
        {
            auto resource=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr;Check(resource->Map(0,nullptr,&mapped));memcpy(mapped,data,size_t(bytes));resource->Unmap(0,nullptr);return resource;
        };
        auto barrier=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);};
        auto uavBarrier=[&](ID3D12Resource* resource)
        {D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=resource;list->ResourceBarrier(1,&b);};

        constexpr UINT count=8192;
        Texture texture{8,8,std::vector<float>(64)};
        std::mt19937 random(40);std::uniform_real_distribution<float> unit(0,1),coord(-1,1);
        for(auto& h:texture.pixels) h=unit(random);
        SilPomPatch patch{2,2,.2f,.5f,1.5f,.75f,.13f,-.31f,8,8,0,4096};
        std::vector<SilPomRay> rays(count);
        for(auto& r:rays) r={{coord(random)*1.4f,coord(random)*1.4f,coord(random)*.25f},normalize({coord(random),coord(random),coord(random)}),0,10};
        auto rayBuffer=upload(rays.data(),rays.size()*sizeof(SilPomRay));
        auto output=buffer(count*sizeof(SilPomHit),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback=buffer(count*sizeof(SilPomHit),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=8;td.Height=8;td.DepthOrArraySize=1;
        td.MipLevels=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.SampleDesc.Count=1;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> image;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image)));
        std::vector<char> padded(256*8);for(int y=0;y<8;++y) memcpy(padded.data()+y*256,texture.pixels.data()+y*8,32);
        auto imageUpload=upload(padded.data(),padded.size());
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=imageUpload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,8,8,1,256};
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=image.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(image.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        D3D12_ROOT_PARAMETER parameters[5]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={1,&range};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor={1,0};
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor={0,0};
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={0,0,13};
        parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[4].Descriptor={2,0};
        D3D12_ROOT_SIGNATURE_DESC rd{5,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> signature,error;Check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error));
        ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)));
        auto bind=[&]()
        {
            ID3D12DescriptorHeap* heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
            list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
            list->SetComputeRootShaderResourceView(1,rayBuffer->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
            list->SetComputeRoot32BitConstants(3,12,&patch,0);list->SetComputeRoot32BitConstant(3,count,12);
        };
        auto verify=[&](const char* backend)
        {
            barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(readback.Get(),output.Get());barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);submit();
            SilPomHit* values=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&values)));
            UINT hitCount=0;
            for(UINT i=0;i<count;++i)
            {
                auto reference=SilPomIntersect(texture,patch,rays[i]);const auto& actual=values[i];
                if(reference.status!=actual.status || (actual.status==SP_HIT &&
                    (std::abs(reference.t-actual.t)>2e-4f || std::abs(reference.uv.x-actual.uv.x)>2e-4f || dot(reference.normal,actual.normal)<.9999f)))
                    throw std::runtime_error(std::string(backend)+" mismatch at ray "+std::to_string(i));
                if(actual.status==SP_HIT) ++hitCount;
            }
            readback->Unmap(0,nullptr);std::cout<<backend<<": "<<count<<" ray comparisons passed ("<<hitCount<<" hits).\n";
        };
        auto cs=Read(argv[1]);D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={cs.data(),cs.size()};
        ComPtr<ID3D12PipelineState> pipeline;Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
        bind();list->SetPipelineState(pipeline.Get());list->Dispatch((count+63)/64,1,1);verify("Compute");

        using Float4 = SilPOM::Curved::PackedFloat4;
        struct CurvedConstants
        {
            float baseScale,amplitude,reference;uint addressMode;
            uint textureWidth,textureHeight,fragmentCount,maxNodes,maxDepth,rayCount,firstRay;float requiredDepthAccuracy;uint packedTriangleCount;
        };
        struct CurvedResult
        {
            uint status,primitiveId,nodes,maximumDepth;
            float t,tError;
            float barycentric[3];
            float unresolvedT;
            float unresolvedUpper;
            uint singular;
            uint exhaustionReasons;
            float uv[2];
            float normal[3];
        };
        static_assert(sizeof(CurvedConstants)==52 && sizeof(CurvedResult)==72 && sizeof(Float4)==16);
        namespace Curved=SilPOM::Curved;
        Curved::Texture curvedTexture{texture.width,texture.height,std::vector<double>(texture.width*texture.height)};
        for(uint y=0;y<texture.height;++y)for(uint x=0;x<texture.width;++x)
            curvedTexture.pixels[y*texture.width+x]=double(x)/8.0;
        const Curved::Vec3 a{0,0,0},b{1,0,0},c{0,1,0},d{2,0,.25};
        const Curved::Vec3 up{0,0,1},tilted{1,0,0};
        std::vector<Curved::Triangle> curvedTriangles{
            Curved::Triangle{{a,b,c},{Curved::Vec2{.125,.125},Curved::Vec2{.375,.125},Curved::Vec2{.125,.375}},{up,tilted,up},7},
            Curved::Triangle{{b,d,c},{Curved::Vec2{.375,.125},Curved::Vec2{.625,.125},Curved::Vec2{.125,.375}},{tilted,up,up},11}};
        Curved::Surface curvedSurface{1,.5,.5,0};
        std::vector<Float4> packedFragments;
        uint curvedFragmentCount=0;
        auto pack=[](Curved::Vec3 value,float w=0.f)
        {return Float4{float(value.x),float(value.y),float(value.z),w};};
        for(const auto& triangle:curvedTriangles)
        {
            for(const auto& fragment:Curved::BuildFragments(triangle,curvedTexture))
            {
                ++curvedFragmentCount;
                packedFragments.push_back(pack(triangle.position[0]));packedFragments.push_back(pack(triangle.position[1]));packedFragments.push_back(pack(triangle.position[2]));
                packedFragments.push_back(pack(triangle.direction[0]));packedFragments.push_back(pack(triangle.direction[1]));packedFragments.push_back(pack(triangle.direction[2]));
                packedFragments.push_back({float(triangle.uv[0].x),float(triangle.uv[0].y),float(triangle.uv[1].x),float(triangle.uv[1].y)});
                float primitiveBits;memcpy(&primitiveBits,&triangle.primitiveId,sizeof(primitiveBits));
                packedFragments.push_back({float(triangle.uv[2].x),float(triangle.uv[2].y),primitiveBits,0});
                packedFragments.push_back(pack(fragment.domain[0]));packedFragments.push_back(pack(fragment.domain[1]));packedFragments.push_back(pack(fragment.domain[2]));
            }
        }
        const auto compactMesh=Curved::PackMesh(curvedTriangles,curvedSurface,curvedTexture,1048576,true);
        if(compactMesh.fragmentCount!=curvedFragmentCount)throw std::runtime_error("Packed fragment count changed");
        const auto reversedPackedFragments=compactMesh.ReversedFragments();
        constexpr uint curvedCount=288;
        static_assert(curvedCount*sizeof(CurvedResult)<=count*sizeof(SilPomHit), "Curved output exceeds readback allocation");
        constexpr uint tangentIndex=248;
        std::vector<SilPomRay> curvedRays(curvedCount);
        for(uint index=0;index<curvedCount;++index)
        {
            const auto& triangle=curvedTriangles[0];
            const double u=(double(index%24)+.371)/58.0;
            const double v=(double((index/24)%8)+.613)/58.0;
            const auto sample=Curved::Evaluate(triangle,curvedSurface,curvedTexture,{1-u-v,u,v});
            const auto point=sample.position;
            Curved::Vec3 direction{.15,.07,-1};
            const auto origin=point-direction;
            curvedRays[index]={{float(origin.x),float(origin.y),float(origin.z)},
                {float(direction.x),float(direction.y),float(direction.z)},0,4};
            if(index>=192&&index<208)
                curvedRays[index]={{3.0f+float(index-192)*.05f,3,1},{0,0,-1},0,4};
            else if(index>=208&&index<224)
                curvedRays[index].tMax=.5f;
            else if(index>=224&&index<240)
            {
                const Curved::Vec3 tangentOrigin=point-sample.du+sample.normal*(2e-4*double(index-223));
                curvedRays[index]={{float(tangentOrigin.x),float(tangentOrigin.y),float(tangentOrigin.z)},
                    {float(sample.du.x),float(sample.du.y),float(sample.du.z)},0,2};
            }
            else if(index>=240&&index<248)
            {
                const Curved::Vec3 twoHitOrigin=point-sample.du-sample.normal*(2e-4*double(index-239));
                curvedRays[index]={{float(twoHitOrigin.x),float(twoHitOrigin.y),float(twoHitOrigin.z)},
                    {float(sample.du.x),float(sample.du.y),float(sample.du.z)},0,2};
            }
        }
        {
            const auto sample=Curved::Evaluate(curvedTriangles[0],curvedSurface,curvedTexture,{.5,.25,.25});
            const auto origin=sample.position-sample.du;
            curvedRays[tangentIndex]={{float(origin.x),float(origin.y),float(origin.z)},
                {float(sample.du.x),float(sample.du.y),float(sample.du.z)},0,2};
        }
        curvedRays[249].direction={0,0,0};
        curvedRays[250].origin.x=std::numeric_limits<float>::quiet_NaN();
        curvedRays[251].tMin=3;curvedRays[251].tMax=2;
        uint multipleIntersectionFixtures=0;
        for(uint index=240;index<248;++index)
        {
            const auto& input=curvedRays[index];
            Curved::Ray ray{{input.origin.x,input.origin.y,input.origin.z},
                {input.direction.x,input.direction.y,input.direction.z},input.tMin,input.tMax};
            multipleIntersectionFixtures+=Curved::ReferenceAll(curvedTriangles,curvedSurface,curvedTexture,ray,256).size()>1;
        }
        if(multipleIntersectionFixtures==0)throw std::runtime_error("Curved corpus failed to construct multiple intersections");
        auto curvedRayBuffer=upload(curvedRays.data(),curvedRays.size()*sizeof(SilPomRay));
        auto legacyFragmentBuffer=upload(packedFragments.data(),packedFragments.size()*sizeof(Float4));
        auto curvedFragmentBuffer=upload(compactMesh.data.data(),compactMesh.data.size()*sizeof(Float4));
        auto curvedReverseFragmentBuffer=upload(reversedPackedFragments.data(),reversedPackedFragments.size()*sizeof(Float4));
        CurvedConstants curvedConstants{1,.5f,.5f,0,texture.width,texture.height,curvedFragmentCount,32768,12,curvedCount,0,5e-4f,compactMesh.ShaderHeader()};
        auto curvedCs=Read(argv[3]);pd.CS={curvedCs.data(),curvedCs.size()};
        ComPtr<ID3D12PipelineState> curvedPipeline;Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&curvedPipeline)));
        ComPtr<ID3D12Resource> curvedImage;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&curvedImage)));
        std::vector<char> curvedPadded(256*texture.height);
        for(uint y=0;y<texture.height;++y)
        {
            std::vector<float> row(texture.width);
            for(uint x=0;x<texture.width;++x)row[x]=float(curvedTexture.pixels[y*texture.width+x]);
            memcpy(curvedPadded.data()+y*256,row.data(),row.size()*sizeof(float));
        }
        auto curvedImageUpload=upload(curvedPadded.data(),curvedPadded.size());
        src.pResource=curvedImageUpload.Get();dst.pResource=curvedImage.Get();
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(curvedImage.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        device->CreateShaderResourceView(curvedImage.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_QUERY_HEAP_DESC queryDescription{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,2,0};
        ComPtr<ID3D12QueryHeap> queryHeap;Check(device->CreateQueryHeap(&queryDescription,IID_PPV_ARGS(&queryHeap)));
        auto queryReadback=buffer(16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12DescriptorHeap* curvedHeaps[]={heap.Get()};list->SetDescriptorHeaps(1,curvedHeaps);list->SetComputeRootSignature(root.Get());
        list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootShaderResourceView(1,curvedRayBuffer->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(3,13,&curvedConstants,0);
        list->SetComputeRootShaderResourceView(4,curvedFragmentBuffer->GetGPUVirtualAddress());
        list->SetPipelineState(curvedPipeline.Get());
        list->EndQuery(queryHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        list->Dispatch((curvedCount+63)/64,1,1);
        list->EndQuery(queryHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        list->ResolveQueryData(queryHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,queryReadback.Get(),0);
        barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(readback.Get(),output.Get());barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);submit();
        UINT64* timestamps=nullptr;Check(queryReadback->Map(0,nullptr,reinterpret_cast<void**>(&timestamps)));
        UINT64 frequency=0;Check(queue->GetTimestampFrequency(&frequency));
        const double curvedMilliseconds=1000.0*double(timestamps[1]-timestamps[0])/double(frequency);queryReadback->Unmap(0,nullptr);
        CurvedResult* curvedValues=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&curvedValues)));
        uint curvedHits=0,curvedMisses=0,curvedInvalid=0,curvedExhausted=0,curvedMaxNodes=0;
        for(uint index=0;index<curvedCount;++index)
        {
            const auto& input=curvedRays[index];
            Curved::Ray referenceRay{{input.origin.x,input.origin.y,input.origin.z},{input.direction.x,input.direction.y,input.direction.z},input.tMin,input.tMax};
            const auto reference=Curved::Intersect(curvedTriangles,curvedSurface,curvedTexture,referenceRay);
            const auto& actual=curvedValues[index];curvedMaxNodes=std::max(curvedMaxNodes,actual.nodes);
            const bool tangent=index==tangentIndex;
            const bool diagnosedNearMiss=index>=224&&index<240&&actual.status==uint(Curved::Status::Exhausted);
            const double referenceT=(reference.tLower+reference.tUpper)*.5;
            const bool diagnosedHit=reference.status==Curved::Status::Hit&&actual.status==uint(Curved::Status::Exhausted)&&
                actual.unresolvedT<=referenceT+1e-3&&actual.unresolvedUpper>=referenceT-1e-3&&actual.exhaustionReasons!=0;
            if((tangent&&actual.status!=uint(Curved::Status::Exhausted)) ||
                (!tangent&&!diagnosedNearMiss&&!diagnosedHit&&actual.status!=uint(reference.status)) ||
                (actual.status==uint(Curved::Status::Hit)&&
                    std::abs(actual.t-referenceT)>actual.tError))
                throw std::runtime_error("Curved compute mismatch at ray "+std::to_string(index)+
                    " status="+std::to_string(actual.status)+" t="+std::to_string(actual.t)+
                    " reference="+std::to_string((reference.tLower+reference.tUpper)*.5)+
                    " nodes="+std::to_string(actual.nodes)+" depth="+std::to_string(actual.maximumDepth)+
                    " unresolved=["+std::to_string(actual.unresolvedT)+","+std::to_string(actual.unresolvedUpper)+"] reasons="+
                    std::to_string(actual.exhaustionReasons));
            if((tangent||diagnosedNearMiss||diagnosedHit) &&
                (actual.exhaustionReasons&Curved::ExhaustionUncertifiedLeaf)==0)
                throw std::runtime_error("Curved tangent ray lacks an uncertified-singular diagnostic");
            if(actual.status==uint(Curved::Status::Hit))
            {
                const float numericalFloor=32*std::numeric_limits<float>::epsilon()*std::max(1.f,std::abs(actual.t)+actual.tError);
                if(actual.primitiveId!=reference.primitiveId||actual.tError>std::max(curvedConstants.requiredDepthAccuracy,numericalFloor)*1.001f)
                    throw std::runtime_error("Curved hit ownership/depth bound mismatch at ray "+std::to_string(index)+
                        " primitive="+std::to_string(actual.primitiveId)+" reference="+std::to_string(reference.primitiveId)+
                        " error="+std::to_string(actual.tError));
                Curved::Vec3 actualBarycentric{actual.barycentric[0],actual.barycentric[1],actual.barycentric[2]};
                if(Curved::Length(actualBarycentric-reference.barycentric)>3e-3)
                    throw std::runtime_error("Curved barycentric mismatch at ray "+std::to_string(index));
                const Curved::Vec3 actualNormal{actual.normal[0],actual.normal[1],actual.normal[2]};
                if(std::abs(actual.uv[0]-reference.uv.x)>1e-3||std::abs(actual.uv[1]-reference.uv.y)>1e-3||
                    std::abs(Curved::Length(actualNormal)-1)>2e-5||Curved::Dot(actualNormal,reference.normal)<.999f)
                    throw std::runtime_error("Curved UV/normal mismatch at ray "+std::to_string(index));
            }
            curvedHits+=actual.status==uint(Curved::Status::Hit);
            curvedMisses+=actual.status==uint(Curved::Status::Miss);
            curvedInvalid+=actual.status==uint(Curved::Status::Invalid);
            curvedExhausted+=actual.status==uint(Curved::Status::Exhausted);
        }
        if(curvedHits<128||curvedMisses<24||curvedInvalid!=3||curvedExhausted==0)
            throw std::runtime_error("Curved corpus lacks required hit/miss/invalid/exhausted coverage");
        std::vector<CurvedResult> forwardResults(curvedValues,curvedValues+curvedCount);
        readback->Unmap(0,nullptr);

        auto dispatchCurvedPass=[&](ID3D12Resource* fragments,const CurvedConstants& constants,uint dispatchCount,bool timed=false)
        {
            ID3D12DescriptorHeap* heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
            list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
            list->SetComputeRootShaderResourceView(1,curvedRayBuffer->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
            list->SetComputeRoot32BitConstants(3,13,&constants,0);
            list->SetComputeRootShaderResourceView(4,fragments->GetGPUVirtualAddress());
            list->SetPipelineState(curvedPipeline.Get());
            if(timed)list->EndQuery(queryHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
            list->Dispatch((dispatchCount+63)/64,1,1);
            if(timed)
            {
                list->EndQuery(queryHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
                list->ResolveQueryData(queryHeap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,queryReadback.Get(),0);
            }
            barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(readback.Get(),output.Get());
            barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);submit();
            double milliseconds=0;
            if(timed)
            {
                UINT64* values=nullptr;Check(queryReadback->Map(0,nullptr,reinterpret_cast<void**>(&values)));
                milliseconds=1000.0*double(values[1]-values[0])/double(frequency);queryReadback->Unmap(0,nullptr);
            }
            return milliseconds;
        };

        CurvedConstants legacyConstants=curvedConstants;legacyConstants.packedTriangleCount=0;
        dispatchCurvedPass(legacyFragmentBuffer.Get(),legacyConstants,curvedCount);
        CurvedResult* legacyValues=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&legacyValues)));
        uint newlyResolved=0;
        for(uint index=0;index<curvedCount;++index)
        {
            const auto& legacy=legacyValues[index];const auto& compact=forwardResults[index];
            if(legacy.status==uint(Curved::Status::Hit)&&compact.status==uint(Curved::Status::Hit)&&
                (legacy.primitiveId!=compact.primitiveId||std::abs(legacy.t-compact.t)>legacy.tError+compact.tError))
                throw std::runtime_error("Compact/legacy hit differs at ray "+std::to_string(index));
            if((legacy.status==uint(Curved::Status::Hit)&&compact.status==uint(Curved::Status::Miss))||
                (legacy.status==uint(Curved::Status::Miss)&&compact.status==uint(Curved::Status::Hit)))
                throw std::runtime_error("Compact/legacy surface classification differs at ray "+std::to_string(index));
            newlyResolved+=legacy.status==uint(Curved::Status::Exhausted)&&compact.status!=uint(Curved::Status::Exhausted);
        }
        readback->Unmap(0,nullptr);

        dispatchCurvedPass(curvedReverseFragmentBuffer.Get(),curvedConstants,curvedCount);
        CurvedResult* reversedValues=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&reversedValues)));
        for(uint index=0;index<curvedCount;++index)
        {
            const auto& forward=forwardResults[index];const auto& reversed=reversedValues[index];
            if(forward.status!=reversed.status||
                (forward.status==uint(Curved::Status::Hit)&&(forward.primitiveId!=reversed.primitiveId||
                    std::abs(forward.t-reversed.t)>forward.tError+reversed.tError)))
                throw std::runtime_error("Curved reversed candidate order mismatch at ray "+std::to_string(index));
        }
        readback->Unmap(0,nullptr);

        CurvedConstants exhaustedConstants=curvedConstants;exhaustedConstants.maxNodes=1;exhaustedConstants.rayCount=1;
        dispatchCurvedPass(curvedFragmentBuffer.Get(),exhaustedConstants,1);
        CurvedResult* exhaustedValues=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&exhaustedValues)));
        const CurvedResult forced=exhaustedValues[0];readback->Unmap(0,nullptr);
        if(forced.status!=uint(Curved::Status::Exhausted)||(forced.exhaustionReasons&Curved::ExhaustionNodeBudget)==0||
            !std::isfinite(forced.unresolvedT)||forced.unresolvedUpper<forced.unresolvedT)
                throw std::runtime_error("Curved forced exhaustion failed to account for every pending candidate");

        uint exhaustedAfterCandidate=0;
        for(uint budget: {16u,64u,256u})
        {
            CurvedConstants limited=curvedConstants;limited.maxNodes=budget;
            dispatchCurvedPass(curvedFragmentBuffer.Get(),limited,curvedCount);
            CurvedResult* values=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&values)));
            for(uint index=0;index<curvedCount;++index)
            {
                const auto& actual=values[index];const auto& complete=forwardResults[index];
                if(actual.status==uint(Curved::Status::Hit)&&
                    (complete.status!=uint(Curved::Status::Hit)||actual.primitiveId!=complete.primitiveId||
                     std::abs(actual.t-complete.t)>actual.tError+complete.tError))
                    throw std::runtime_error("Work-budget exit promoted an uncertified nearest hit");
                exhaustedAfterCandidate+=actual.status==uint(Curved::Status::Exhausted)&&actual.primitiveId!=0xffffffffu;
                if(actual.status==uint(Curved::Status::Exhausted)&&
                    (!std::isfinite(actual.unresolvedT)||actual.unresolvedT>actual.unresolvedUpper||actual.exhaustionReasons==0))
                    throw std::runtime_error("Work-budget exit lost pending interval diagnostics at ray "+std::to_string(index)+
                        " budget="+std::to_string(budget)+" reasons="+std::to_string(actual.exhaustionReasons));
            }
            readback->Unmap(0,nullptr);
        }
        if(exhaustedAfterCandidate==0)throw std::runtime_error("Budget sweep did not exercise exhaustion after a candidate hit");
        std::cout<<"Curved budget sweep: "<<exhaustedAfterCandidate<<" candidate hits correctly retained as Exhausted.\n";

        auto benchmark=[&](const char* label,ID3D12Resource* fragments,CurvedConstants constants,uint first,uint samples)
        {
            constants.firstRay=first;constants.rayCount=first+samples;
            dispatchCurvedPass(fragments,constants,samples);
            std::vector<double> times;
            for(int iteration=0;iteration!=5;++iteration)times.push_back(dispatchCurvedPass(fragments,constants,samples,true));
            std::sort(times.begin(),times.end());
            std::cout<<"Curved benchmark "<<label<<": "<<samples<<" rays, median="<<times[2]<<" ms, max="<<times.back()<<" ms (5 warm runs).\n";
        };
        benchmark("compact/front",curvedFragmentBuffer.Get(),curvedConstants,0,192);
        benchmark("compact/grazing",curvedFragmentBuffer.Get(),curvedConstants,224,25);
        benchmark("legacy/front",legacyFragmentBuffer.Get(),legacyConstants,0,192);
        benchmark("legacy/grazing",legacyFragmentBuffer.Get(),legacyConstants,224,25);
        CurvedConstants fineConstants=curvedConstants;fineConstants.packedTriangleCount|=0x40000000u;
        benchmark("compact/fine-grazing",curvedFragmentBuffer.Get(),fineConstants,224,25);

        // Exercise nonzero bilinear cross terms, clamp addressing, signed height
        // and instance-resolved bounds. Compare actual shader outputs, not CPU
        // reconstruction of UV/normal from the returned barycentrics.
        Curved::Texture noisyTexture{texture.width,texture.height,std::vector<double>(texture.pixels.begin(),texture.pixels.end())};
        device->CreateShaderResourceView(image.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
        for(double baseScale: {.125,8.})for(double amplitude: {-.04,.04})
        {
            const Curved::Surface surface{baseScale,amplitude,.25,1};
            const auto mesh=Curved::PackMesh(curvedTriangles,surface,noisyTexture,1048576,true);
            const auto fragments=upload(mesh.data.data(),mesh.data.size()*sizeof(Float4));
            std::vector<SilPomRay> inputs(48);
            for(uint index=0;index<inputs.size();++index)
            {
                const double u=(index%8+.371)/18.,v=(index/8+.613)/18.;
                const auto sample=Curved::Evaluate(curvedTriangles[index%2],surface,noisyTexture,{1-u-v,u,v});
                const Curved::Vec3 direction{.15,.07,-1},origin=sample.position-direction;
                inputs[index]={{float(origin.x),float(origin.y),float(origin.z)},{float(direction.x),float(direction.y),float(direction.z)},0,4};
            }
            curvedRayBuffer=upload(inputs.data(),inputs.size()*sizeof(SilPomRay));
            CurvedConstants constants{float(baseScale),float(amplitude),.25f,1,texture.width,texture.height,
                mesh.fragmentCount,32768,12,uint(inputs.size()),0,5e-4f,mesh.ShaderHeader(true)};
            dispatchCurvedPass(fragments.Get(),constants,uint(inputs.size()));
            CurvedResult* values=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&values)));
            uint hits=0,exhaustions=0;
            for(uint index=0;index<inputs.size();++index)
            {
                const auto& input=inputs[index];const auto& actual=values[index];
                const Curved::Ray ray{{input.origin.x,input.origin.y,input.origin.z},{input.direction.x,input.direction.y,input.direction.z},input.tMin,input.tMax};
                const auto expected=Curved::Intersect(curvedTriangles,surface,noisyTexture,ray);
                if(actual.status==uint(Curved::Status::Exhausted))
                {
                    if(actual.exhaustionReasons==0||!std::isfinite(actual.unresolvedT))throw std::runtime_error("Missing scaled-case exhaustion diagnostic");
                    ++exhaustions;continue;
                }
                if(actual.status!=uint(expected.status))throw std::runtime_error("Scaled cubic classification differs at ray "+std::to_string(index));
                if(actual.status==uint(Curved::Status::Hit))
                {
                    const double t=(expected.tLower+expected.tUpper)*.5;
                    const float floor=32*std::numeric_limits<float>::epsilon()*std::max(1.f,std::abs(actual.t)+actual.tError);
                    if(actual.primitiveId!=expected.primitiveId||std::abs(actual.t-t)>actual.tError+2e-6||
                        actual.tError>std::max(constants.requiredDepthAccuracy,floor)*1.001f||
                        std::abs(actual.uv[0]-expected.uv.x)>1e-3||std::abs(actual.uv[1]-expected.uv.y)>1e-3||
                        Curved::Dot({actual.normal[0],actual.normal[1],actual.normal[2]},expected.normal)<.999f)
                        throw std::runtime_error("Scaled cubic hit attributes differ at ray "+std::to_string(index));
                    ++hits;
                }
            }
            readback->Unmap(0,nullptr);
            if(hits<8)throw std::runtime_error("Too few certified hits in scaled cubic corpus");
            std::cout<<"Curved cubic/scale="<<baseScale<<" amplitude="<<amplitude<<": "<<hits<<" hits, "<<exhaustions<<" exhausted.\n";
        }

        device->CreateShaderResourceView(image.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
        std::cout<<"Curved compute: "<<curvedCount<<" mixed rays passed ("<<curvedHits<<" certified hits, "
            <<curvedMisses<<" misses, "<<curvedInvalid<<" invalid, "<<curvedExhausted<<" exhausted), "
            <<multipleIntersectionFixtures<<" multiple-intersection fixtures plus reversed order and forced exhaustion, "
            <<curvedMilliseconds<<" ms, max "<<curvedMaxNodes<<" nodes.\n";
        std::cout<<"Curved storage: "<<curvedTriangles.size()<<" shared triangles, "<<curvedFragmentCount
            <<" fragments, "<<packedFragments.size()*sizeof(Float4)<<" legacy bytes -> "
            <<compactMesh.data.size()*sizeof(Float4)<<" compact bytes; "<<newlyResolved<<" additional rays resolved.\n";

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)));
        if(options.RaytracingTier==D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        {std::cout<<"Procedural RT: SKIPPED (device has no DXR).\n";CloseHandle(event);return 0;}
        D3D12_RAYTRACING_AABB bounds{-1,-1,-.10001f,1,1,.10001f};auto aabb=upload(&bounds,sizeof(bounds));
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometry.AABBs.AABBCount=1;geometry.AABBs.AABBs={aabb->GetGPUVirtualAddress(),sizeof(bounds)};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bi{};bi.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bi.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;bi.NumDescs=1;bi.pGeometryDescs=&geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};device->GetRaytracingAccelerationStructurePrebuildInfo(&bi,&info);
        auto blas=buffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto scratch=buffer(info.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=bi;build.DestAccelerationStructureData=blas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);uavBarrier(blas.Get());
        D3D12_RAYTRACING_INSTANCE_DESC instance{};instance.Transform[0][0]=instance.Transform[1][1]=instance.Transform[2][2]=1;
        instance.InstanceMask=255;instance.AccelerationStructure=blas->GetGPUVirtualAddress();auto instances=upload(&instance,sizeof(instance));
        bi.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;bi.InstanceDescs=instances->GetGPUVirtualAddress();
        device->GetRaytracingAccelerationStructurePrebuildInfo(&bi,&info);
        auto tlas=buffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto topScratch=buffer(info.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        build.Inputs=bi;build.DestAccelerationStructureData=tlas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=topScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build,0,nullptr);uavBarrier(tlas.Get());
        auto library=Read(argv[2]);D3D12_DXIL_LIBRARY_DESC lib{};lib.DXILLibrary={library.data(),library.size()};
        D3D12_HIT_GROUP_DESC group{};group.HitGroupExport=L"HeightfieldHit";group.Type=D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        group.IntersectionShaderImport=L"Intersection";group.ClosestHitShaderImport=L"ClosestHit";
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{sizeof(SilPomHit),32};D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
        ID3D12RootSignature* rootPointer=root.Get();
        D3D12_STATE_SUBOBJECT objects[]={
            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&lib},{D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&group},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shaderConfig},{D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&rootPointer},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipelineConfig}};
        D3D12_STATE_OBJECT_DESC sd{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,5,objects};
        ComPtr<ID3D12StateObject> state;Check(device->CreateStateObject(&sd,IID_PPV_ARGS(&state)));
        ComPtr<ID3D12StateObjectProperties> stateProps;Check(state.As(&stateProps));
        char tableData[192]{};memcpy(tableData,stateProps->GetShaderIdentifier(L"RayGeneration"),32);
        memcpy(tableData+64,stateProps->GetShaderIdentifier(L"Miss"),32);memcpy(tableData+128,stateProps->GetShaderIdentifier(L"HeightfieldHit"),32);
        auto table=upload(tableData,sizeof(tableData));D3D12_DISPATCH_RAYS_DESC dispatch{};
        dispatch.RayGenerationShaderRecord={table->GetGPUVirtualAddress(),32};dispatch.MissShaderTable={table->GetGPUVirtualAddress()+64,32,32};
        dispatch.HitGroupTable={table->GetGPUVirtualAddress()+128,32,32};dispatch.Width=count;dispatch.Height=1;dispatch.Depth=1;
        bind();list->SetComputeRootShaderResourceView(4,tlas->GetGPUVirtualAddress());list->SetPipelineState1(state.Get());list->DispatchRays(&dispatch);verify("Procedural RT");
        CloseHandle(event);return 0;
    }
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}

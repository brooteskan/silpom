// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../Include/SilPOM/SurfacePaintImage.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <random>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
using namespace SilPOM::SurfacePaintMath;
namespace Image=SilPOM::SurfacePaintImage;
void Check(HRESULT hr)
{if(FAILED(hr))throw std::runtime_error("D3D12 HRESULT "+std::to_string(static_cast<unsigned>(hr)));}
void Require(bool pass,const char* text){if(!pass)throw std::runtime_error(text);}
double Angle(Vec3 a,Vec3 b)
{
    const double dot=double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z;
    const double length=std::sqrt((double(a.x)*a.x+double(a.y)*a.y+double(a.z)*a.z)*(double(b.x)*b.x+double(b.y)*b.y+double(b.z)*b.z));
    return std::acos(std::clamp(dot/length,-1.,1.))*57.29577951308232;
}
struct Query {float baseMode[4],paint[4],uvLod[4];};
struct Result {float normal[4],paint[4];};
static_assert(sizeof(Query)==48&&sizeof(Result)==32);
int main(int argc,char** argv)
{
    try
    {
        Require(argc==2||argc==3,"Usage: silpom_surface_normal_gpu_tests shader.dxil [report.json]");
        ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;
        std::wstring adapterName;
        for(UINT i=0;factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i)
        {
            DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);
            if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))
            {adapterName=desc.Description;break;}
            adapter.Reset();
        }
        Require(bool(device),"No hardware D3D12 adapter");std::wcout<<L"GPU: "<<adapterName<<L'\n';
        ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC queueDesc{};
        Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            D3D12_HEAP_PROPERTIES props{};props.Type=heap;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;
            desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
            ComPtr<ID3D12Resource> resource;
            Check(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)));return resource;
        };
        auto upload=[&](const void* bytes,UINT64 count)
        {
            auto resource=buffer(count,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr;Check(resource->Map(0,nullptr,&mapped));std::memcpy(mapped,bytes,size_t(count));resource->Unmap(0,nullptr);return resource;
        };
        auto barrier=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
        };
        Image::Image image;image.width=image.height=4;image.pixels.resize(16);
        for(unsigned i=0;i<image.pixels.size();++i)
        {
            const Vec3 detail=Unit({std::sin(i*.7f),std::cos(i*.9f),.2f+float(i%5)*.15f});
            PaintNormal(image.pixels[i].normal,detail,float(i%7)/6);
            if(i%4==0)EraseNormal(image.pixels[i].normal,.5f);
        }
        image.pixels[0].normal={};image.pixels[15].normal={};PaintNormal(image.pixels[15].normal,{0,0,1},.8f);
        const auto mips=Image::GenerateMips(image);Require(mips.size()==3,"Fixture mip generation failed");
        D3D12_RESOURCE_DESC textureDesc{};textureDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width=textureDesc.Height=4;textureDesc.DepthOrArraySize=1;textureDesc.MipLevels=UINT16(mips.size());
        textureDesc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;textureDesc.SampleDesc.Count=1;
        D3D12_HEAP_PROPERTIES defaultHeap{};defaultHeap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        Check(device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&textureDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)));
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips.size());UINT64 textureBytes=0;
        device->GetCopyableFootprints(&textureDesc,0,UINT(mips.size()),0,footprints.data(),nullptr,nullptr,&textureBytes);
        std::vector<uint8_t> textureSource(size_t(textureBytes),0);
        for(size_t mip=0;mip<mips.size();++mip)
        {
            const auto& plane=mips[mip];
            for(uint32_t y=0;y<plane.height;++y)for(uint32_t x=0;x<plane.width;++x)
                std::memcpy(textureSource.data()+footprints[mip].Offset+size_t(y)*footprints[mip].Footprint.RowPitch+size_t(x)*16,
                    &plane.pixels[size_t(y)*plane.width+x].normal,16);
        }
        auto textureUpload=upload(textureSource.data(),textureBytes);
        for(UINT mip=0;mip<mips.size();++mip)
        {
            D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=texture.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;destination.SubresourceIndex=mip;
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=textureUpload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprints[mip];
            list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        }
        barrier(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heapDesc.NumDescriptors=1+UINT(mips.size());heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;Check(device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)));
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format=textureDesc.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=UINT(mips.size());
        device->CreateShaderResourceView(texture.Get(),&view,heap->GetCPUDescriptorHandleForHeapStart());
        const UINT descriptorStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(UINT mip=0;mip<mips.size();++mip)
        {
            auto mipView=view;mipView.Texture2D.MostDetailedMip=mip;mipView.Texture2D.MipLevels=1;
            auto handle=heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=SIZE_T(mip+1)*descriptorStride;
            device->CreateShaderResourceView(texture.Get(),&mipView,handle);
        }
        D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT(mips.size()),2,0,1}};
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={2,ranges};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor={1,0};
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor={0,0};
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={0,0,2};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{4,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> signature,error;Check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error));
        ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)));
        std::ifstream shaderInput(argv[1],std::ios::binary);Require(bool(shaderInput),"Cannot read compiled shader");
        std::vector<char> shader{std::istreambuf_iterator<char>(shaderInput),{}};
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};pipelineDesc.pRootSignature=root.Get();pipelineDesc.CS={shader.data(),shader.size()};
        ComPtr<ID3D12PipelineState> pipeline;Check(device->CreateComputePipelineState(&pipelineDesc,IID_PPV_ARGS(&pipeline)));
        constexpr UINT directCount=2048,filteredCount=2048,divergentCount=2048,count=directCount+filteredCount+divergentCount;
        std::vector<Query> queries(count);std::vector<NormalTexel> expectedPaint(count);std::vector<Vec3> expected(count);
        std::mt19937 random(0x53494c50);std::uniform_real_distribution<float> signedUnit(-1,1),unit(0,1);
        for(UINT i=0;i<count;++i)
        {
            Vec3 base=Unit({signedUnit(random),signedUnit(random),signedUnit(random)});
            if(i%31==0)base={0,0,-1};if(i%37==0)base={0,0,1};
            auto& q=queries[i];q.baseMode[0]=base.x;q.baseMode[1]=base.y;q.baseMode[2]=base.z;q.baseMode[3]=i<directCount?0.f:(i<directCount+filteredCount?1.f:2.f);
            if(i<directCount)
            {
                NormalTexel paint;PaintNormal(paint,Unit({signedUnit(random),signedUnit(random),.05f+unit(random)}),unit(random));
                if(i%5==0)EraseNormal(paint,.6f);
                if(i%7==0)EraseNormal(paint,1);
                if(i%11==0){paint={};PaintNormal(paint,{0,0,1},unit(random));}
                std::memcpy(q.paint,&paint,16);expectedPaint[i]=paint;
            }
            else
            {
                q.uvLod[0]=unit(random);q.uvLod[1]=unit(random);q.uvLod[2]=unit(random)*2;
                if(i>=directCount+filteredCount)
                    q.uvLod[2]=std::min(float((i-directCount-filteredCount)%3)+unit(random)*.9f,2.f);
                if(i%17==0){q.uvLod[0]=q.uvLod[1]=.125f;q.uvLod[2]=0;} // exact clear texel center
                if(i%19==0){q.uvLod[0]=q.uvLod[1]=.875f;q.uvLod[2]=0;} // exact flat texel center
                const unsigned low=unsigned(q.uvLod[2]),high=std::min(low+1,unsigned(mips.size()-1));const float t=q.uvLod[2]-low;
                const NormalTexel pair[2]={Image::Sample(mips[low],q.uvLod[0],q.uvLod[1]).normal,Image::Sample(mips[high],q.uvLod[0],q.uvLod[1]).normal};
                const float weights[2]={1-t,t};expectedPaint[i]=FilterNormal(pair,weights,2);
            }
            expected[i]=ComposeNormal(base,expectedPaint[i]);
        }
        auto inputs=upload(queries.data(),count*sizeof(Query));
        auto output=buffer(count*sizeof(Result),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback=buffer(count*sizeof(Result),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12DescriptorHeap* heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
        list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRootShaderResourceView(1,inputs->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
        const UINT shaderParams[2]={count,0};list->SetComputeRoot32BitConstants(3,2,shaderParams,0);
        list->SetPipelineState(pipeline.Get());list->Dispatch(count/64,1,1);
        barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyResource(readback.Get(),output.Get());
        Check(list->Close());ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        HANDLE completed=CreateEvent(nullptr,FALSE,FALSE,nullptr);Require(completed!=nullptr,"Cannot create GPU completion event");
        Check(queue->Signal(fence.Get(),1));Check(fence->SetEventOnCompletion(1,completed));
        Require(WaitForSingleObject(completed,60000)==WAIT_OBJECT_0,"GPU normal test timed out");CloseHandle(completed);
        Result* results=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&results)));
        double sumSquared=0,maxDegrees=0,directMaxComponent=0,filteredMaxComponent=0,paintMaxComponent=0;
        double divergentSumSquared=0,divergentMaxDegrees=0,divergentMaxComponent=0,divergentPaintMaxComponent=0;unsigned exactIdentity=0,identityFailures=0;
        for(UINT i=0;i<count;++i)
        {
            const Vec3 actual{results[i].normal[0],results[i].normal[1],results[i].normal[2]};Require(Finite(actual),"Nonfinite GPU normal");
            const double angle=Angle(actual,expected[i]);Require(std::isfinite(angle),"Nonfinite GPU normal angle");sumSquared+=angle*angle;maxDegrees=std::max(maxDegrees,angle);
            const double component=std::max({double(std::abs(actual.x-expected[i].x)),double(std::abs(actual.y-expected[i].y)),double(std::abs(actual.z-expected[i].z))});
            if(i<directCount)directMaxComponent=std::max(directMaxComponent,component);
            else if(i<directCount+filteredCount)filteredMaxComponent=std::max(filteredMaxComponent,component);
            else {divergentSumSquared+=angle*angle;divergentMaxDegrees=std::max(divergentMaxDegrees,angle);divergentMaxComponent=std::max(divergentMaxComponent,component);}
            const float* tuple=reinterpret_cast<const float*>(&expectedPaint[i]);
            for(unsigned c=0;c<4;++c)paintMaxComponent=std::max(paintMaxComponent,double(std::abs(results[i].paint[c]-tuple[c])));
            if(i>=directCount+filteredCount)for(unsigned c=0;c<4;++c)
                divergentPaintMaxComponent=std::max(divergentPaintMaxComponent,double(std::abs(results[i].paint[c]-tuple[c])));
            if(expectedPaint[i].coverage==0||(expectedPaint[i].premultiplied.x==0&&expectedPaint[i].premultiplied.y==0))
            {if(std::memcmp(results[i].normal,queries[i].baseMode,12)!=0)++identityFailures;++exactIdentity;}
        }
        readback->Unmap(0,nullptr);const double rms=std::sqrt(sumSquared/count);
        const bool passed=identityFailures==0&&directMaxComponent<=2.e-5&&rms<=1&&maxDegrees<=5&&paintMaxComponent<=.005;
        const Vec3 targets[2]={Unit({.8f,.1f,.4f}),Unit({-.3f,.7f,.5f})};
        auto old=[&](Vec3 base){return Unit(Unit(base*.6f+targets[0]*.4f)*.4f+targets[1]*.6f);};
        const Vec3 base0{0,0,1},base1=Unit({-.8f,-.2f,.4f});const float transmittance=.24f;
        const double oldError=Angle(Unit(base1*transmittance+old(base0)-base0*transmittance),old(base1));
        Require(oldError>1,"Legacy affine counterexample must demonstrate the mismatch");
        std::cout<<std::setprecision(10)<<"Authoritative normal GPU: "<<count<<" cases, RMS="<<rms<<" degrees, max="<<maxDegrees
            <<" degrees, direct_max_component="<<directMaxComponent<<", filtered_max_component="<<filteredMaxComponent
            <<", divergent_queries="<<divergentCount<<", divergent_RMS="<<std::sqrt(divergentSumSquared/divergentCount)
            <<", divergent_max_degrees="<<divergentMaxDegrees<<", divergent_max_component="<<divergentMaxComponent
            <<", divergent_tuple_max_component="<<divergentPaintMaxComponent
            <<", mip_tuple_max_component="<<paintMaxComponent<<", exact_clear_flat="<<exactIdentity-identityFailures
            <<", clear_flat_failures="<<identityFailures<<", old_affine_error="<<oldError<<" degrees\n";
        if(argc==3)
        {
            std::ofstream report(argv[2]);Require(bool(report),"Cannot create GPU report");
            report<<std::setprecision(12)<<"{\n  \"passed\": "<<(passed?"true":"false")<<",\n  \"queries\": "<<count<<",\n  \"direct_queries\": "<<directCount<<",\n  \"hardware_filtered_queries\": "<<filteredCount
                <<",\n  \"divergent_descriptor_queries\": "<<divergentCount
                <<",\n  \"divergent_normal_rms_degrees\": "<<std::sqrt(divergentSumSquared/divergentCount)
                <<",\n  \"divergent_normal_max_degrees\": "<<divergentMaxDegrees
                <<",\n  \"divergent_normal_max_component_error\": "<<divergentMaxComponent
                <<",\n  \"divergent_mip_tuple_max_component_error\": "<<divergentPaintMaxComponent
                <<",\n  \"normal_rms_degrees\": "<<rms<<",\n  \"normal_max_degrees\": "<<maxDegrees<<",\n  \"direct_max_component_error\": "<<directMaxComponent
                <<",\n  \"filtered_max_component_error\": "<<filteredMaxComponent<<",\n  \"mip_tuple_max_component_error\": "<<paintMaxComponent
                <<",\n  \"exact_clear_flat_queries\": "<<exactIdentity-identityFailures<<",\n  \"clear_flat_failures\": "<<identityFailures
                <<",\n  \"old_affine_counterexample_degrees\": "<<oldError<<"\n}\n";
        }
        Require(identityFailures==0,"Untouched or flat texture changed the exact live normal");
        Require(directMaxComponent<=2.e-5,"Direct normal math differs between CPU and GPU");
        Require(rms<=1&&maxDegrees<=5,"Authoritative texture normal exceeds 1 degree RMS / 5 degree max");
        Require(paintMaxComponent<=.005,"GPU premultiplied texture/mip sampling differs excessively from CPU filtering");
        return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

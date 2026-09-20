// SPDX-License-Identifier: MIT
#include "CpuShader.h"
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
        if(argc!=2 && argc!=3) throw std::runtime_error("Usage: silpom_gpu_tests compute.dxil [rays.dxil]");
        static_assert(sizeof(SilPomPatch)==48 && sizeof(SilPomRay)==32 && sizeof(SilPomHit)==44);
        ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
#ifdef SILPOM_DXR_TESTS
        ComPtr<ID3D12Device5> device;
#else
        ComPtr<ID3D12Device> device;
#endif
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
#ifdef SILPOM_DXR_TESTS
        ComPtr<ID3D12GraphicsCommandList4> list;
#else
        ComPtr<ID3D12GraphicsCommandList> list;
#endif
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
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

        if(argc==2) {std::cout<<"Raster/compute conformance: no DXR used.\n";CloseHandle(event);return 0;}
#ifdef SILPOM_DXR_TESTS
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
#else
        throw std::runtime_error("DXR tests were not enabled at configure time");
#endif
        CloseHandle(event);return 0;
    }
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}

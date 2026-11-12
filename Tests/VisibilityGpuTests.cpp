// SPDX-License-Identifier: MIT
#include "../Tools/VisibilityCpu.h"
#include "../Include/SilPOM/HeightPrecision.h"
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
int main(int argc,char** argv)
{
    try
    {
        if(argc!=2 && argc!=3) throw std::runtime_error("Usage: silpom_visibility_gpu_tests shader.dxil [only]");
        const bool neuralOnly=argc==3;
        static_assert(sizeof(SilPomPatch)==48);
        ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D12Device> device;

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
        ComPtr<ID3D12GraphicsCommandList> list;

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

        std::ifstream input(argv[1],std::ios::binary);
        if(!input) throw std::runtime_error("Cannot open shader");
        std::vector<char> shader{std::istreambuf_iterator<char>(input),{}};
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={1,&range};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor={1,0};
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor={0,0};
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={0,0,16};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{4,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> signature,error;Check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error));
        ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader.data(),shader.size()};
        ComPtr<ID3D12PipelineState> pipeline;Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));


        constexpr UINT count=1024;
        struct Request {float2 uv;float3 light;float reach;uint steps;};
        struct Result {float values[4];uint counts[12];};
        static_assert(sizeof(Request)==28 && sizeof(Result)==64);
        auto output=buffer(count*sizeof(Result),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback=buffer(count*sizeof(Result),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        std::mt19937 random(971);std::uniform_real_distribution<float> coordinate(-.25f,1.25f),unit(0,1);
        std::vector<Request> requests(count);
        for(auto& q:requests)
        {
            float angle=unit(random)*6.2831853f;
            q={{coordinate(random),coordinate(random)},normalize({cos(angle),sin(angle),.02f+unit(random)}),
                unit(random)>.3f?.3f:0.f,0};
        }
        SilPomMeshFace face{};
        face.origin={0,0,0};face.du={1,0,0};face.dv={0,1,0};face.normal={0,0,1};
        face.uv0={0,0};face.uv1={1,0};face.uv2={0,1};
        face.d0={0,0,1};face.d1={.2f,0,1};face.d2={0,.1f,1};
        UINT comparisons=0,inferences=0,accepted=0,topBoundaryQuads=0;float maxProbabilityError=0;
        for(UINT model:{4u,5u,6u,7u}) for(UINT steps:{0u,9u,64u}) for(UINT clamp:{0u,1u})
        for(UINT mode:{1u,2u}) for(UINT pattern:{0u,1u})
        {
            if(neuralOnly && mode!=2) continue;
            for(auto& q:requests) q.steps=steps;
            auto inputs=upload(requests.data(),requests.size()*sizeof(Request));
            Texture texture{32,16,std::vector<float>(32*16,.6f)};
            if(pattern) for(auto& value:texture.pixels) value=unit(random);
            std::vector<char> padded(256*16);
            for(UINT y=0;y<16;++y) memcpy(padded.data()+y*256,texture.pixels.data()+y*32,128);
            D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=32;td.Height=16;
            td.DepthOrArraySize=1;td.MipLevels=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.SampleDesc.Count=1;
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> image;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image)));
            auto imageUpload=upload(padded.data(),padded.size());
            D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=imageUpload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,32,16,1,256};
            D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=image.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=td.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(image.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
            SilPomPatch patch{1,1,.2f,.5f,1,1,0,0,32,16,clamp,SP_TOWER_NO_MARCH};
            ID3D12DescriptorHeap* heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
            list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
            list->SetComputeRootShaderResourceView(1,inputs->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
            list->SetComputeRoot32BitConstants(3,12,&patch,0);list->SetComputeRoot32BitConstant(3,count,12);
            list->SetComputeRoot32BitConstant(3,model,13);list->SetComputeRoot32BitConstant(3,mode,14);
            float confidence=.995f;list->SetComputeRoot32BitConstants(3,1,&confidence,15);
            list->SetPipelineState(pipeline.Get());list->Dispatch(count/64,1,1);
            barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(readback.Get(),output.Get());
            barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);submit();
            Result* values=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&values)));
            for(UINT i=0;i<count;++i)
            {
                visibilityStats={};visibilityProbability=.5f;
                const auto q=requests[i];
                float expected=neuralOnly
                    ?SilPomNeuralOnlyVisibility(texture,patch,face,q.uv,{0,0},q.light,q.steps,q.reach,model,SilPomNeuralOnlyThreshold(model))
                    :SilPomLearnedReliefVisibility(texture,patch,face,q.uv,{0,0},q.light,q.steps,q.reach,model,mode==2,confidence);
                if(neuralOnly && (visibilityStats[5]||visibilityStats[6]||visibilityStats[0]>5))
                    throw std::runtime_error("Neural-only violated its no-march/fixed-probe contract");
                const auto& result=values[i];
                float delta=abs(visibilityProbability-result.values[1]);
                // Physical reach can land exactly at the global height bound.
                // CPU division and GPU reciprocal rounding can differ by one
                // final, non-blocking height query. Branch decisions stay exact.
                int quadDelta=int(result.counts[0])-int(visibilityStats[0]);
                bool topBoundary=quadDelta==0 || (q.reach>0 && expected==1 && abs(quadDelta)==1);
                bool eventMismatch=false;
                for(UINT k=1;k<12;++k) eventMismatch|=result.counts[k]!=visibilityStats[k];
                if(expected!=result.values[0] || delta>2e-5f || !topBoundary || eventMismatch)
                {
                    std::cerr<<"Visibility CPU="<<expected<<" GPU="<<result.values[0]<<" probability delta="<<delta;
                    for(UINT k=0;k<12;++k) if(result.counts[k]!=visibilityStats[k])
                        std::cerr<<" slot "<<k<<" CPU="<<visibilityStats[k]<<" GPU="<<result.counts[k];
                    throw std::runtime_error(" CPU/GPU mismatch model="+std::to_string(model)+" mode="+std::to_string(mode)
                        +" steps="+std::to_string(steps)+" clamp="+std::to_string(clamp)+" pattern="+std::to_string(pattern)+" ray="+std::to_string(i));
                }
                if(mode==1 && expected!=SilPomReliefVisibility(texture,patch,face,q.uv,{0,0},q.light,q.steps,q.reach))
                    throw std::runtime_error("Gather changed baseline visibility");
                maxProbabilityError=std::max(maxProbabilityError,delta);
                topBoundaryQuads+=quadDelta!=0;
                inferences+=visibilityStats[7];accepted+=visibilityStats[3]+visibilityStats[4];++comparisons;
            }
            readback->Unmap(0,nullptr);
        }
        CloseHandle(event);
        if(!inferences || !accepted) throw std::runtime_error("Neural branch was not exercised");
        std::cout<<comparisons<<" CPU/GPU visibility comparisons passed; "<<inferences<<" inferences, "<<accepted
            <<" accepted; max probability error="<<maxProbabilityError<<"; "<<topBoundaryQuads<<" one-query top-boundary differences.\n";
        return 0;
    }
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

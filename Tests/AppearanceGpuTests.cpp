// SPDX-License-Identifier: MIT
#include "../Tools/LevelOneCpu.h"
#include "../Include/SilPOM/AppearanceCodec.h"
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
        D3D12_ROOT_PARAMETER parameters[5]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={1,&range};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor={1,0};
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor={0,0};
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={0,0,1};
        parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[4].Descriptor={2,0};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{5,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> signature,error;Check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error));
        ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader.data(),shader.size()};
        ComPtr<ID3D12PipelineState> pipeline;Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));



        constexpr UINT count=4096;
        std::mt19937 rng(5714);std::uniform_real_distribution<float> uniform(-1,1);
        struct Query {float x[22];};struct Result {float value[9];};
        std::vector<Query> queries(count);for(auto& q:queries)for(float& x:q.x)x=uniform(rng);
        queries[0].x[0]=queries[0].x[1]=0;
        std::vector<float> weights(757);for(float& w:weights)w=uniform(rng)*.15f;
        if(argc==3)
        {
            std::ifstream model(argv[2],std::ios::binary);model.seekg(sizeof(SilPOM::AppearanceCodec::Header));
            model.read(reinterpret_cast<char*>(weights.data()),3028);if(!model)throw std::runtime_error("Cannot read material model");
        }
        auto inputs=upload(queries.data(),queries.size()*sizeof(Query));auto coefficients=upload(weights.data(),3028);
        auto output=buffer(count*sizeof(Result),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback=buffer(count*sizeof(Result),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        list->SetComputeRootSignature(root.Get());list->SetComputeRootShaderResourceView(1,inputs->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(4,coefficients->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());list->SetComputeRoot32BitConstant(3,count,0);
        list->SetPipelineState(pipeline.Get());list->Dispatch(count/64,1,1);
        barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyResource(readback.Get(),output.Get());submit();
        Result* values=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&values)));
        float maxError=0;
        for(UINT i=0;i<count;++i)
        {
            float expected[5];
            SilPOM::AppearanceCodec::Decode(queries[i].x,weights.data(),18,3,expected);
            SilPOM::AppearanceCodec::Decode(queries[i].x,weights.data()+355,22,2,expected+3);
            expected[3]*=2;expected[4]*=2;
            for(UINT c=0;c<5;++c)
            {
                const float error=std::abs(expected[c]-values[i].value[c]);maxError=std::max(maxError,error);
                if(!std::isfinite(values[i].value[c])||error>4e-5f)throw std::runtime_error("Appearance CPU/GPU mismatch");
            }
            const double u=queries[i].x[0],v=queries[i].x[1],epsilon=1e-4;
            // Independently intersect the projected plane at displaced screen
            // coordinates, then take a finite difference in UV.
            for(unsigned axis=0;axis<2;++axis)
            {
                const double x=u+(axis==0?2./640*epsilon:0),y=v+(axis==1?-2./400*epsilon:0);
                const double a=2-x*.2,b=-.3-x*.1,c=1-y*.2,d=3-y*.1;
                const double r=(x-u)*5,s=(y-v)*5,det=a*d-b*c;
                const double expectedUv[]={(r*d-b*s)/det/epsilon,(a*s-r*c)/det/epsilon};
                for(unsigned k=0;k<2;++k)if(std::abs(expectedUv[k]-values[i].value[5+axis*2+k])>2e-7)
                    throw std::runtime_error("Proxy UV Jacobian differs from projected-plane finite difference");
            }
        }
        readback->Unmap(0,nullptr);CloseHandle(event);
        std::cout<<count<<" appearance decoder CPU-GPU comparisons passed; max decoder error="<<maxError<<"\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

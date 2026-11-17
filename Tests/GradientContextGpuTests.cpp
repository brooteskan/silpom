// SPDX-License-Identifier: MIT
#include "GradientContextTestData.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
using namespace SilPOM::GradientCpu;
static void Check(HRESULT hr)
{if(FAILED(hr))throw std::runtime_error("D3D12 HRESULT "+std::to_string(static_cast<unsigned>(hr)));}
int main(int argc,char** argv)
{
    try
    {
        if(argc!=2)throw std::runtime_error("Usage: silpom_gradient_context_gpu_tests shader.dxil");
        ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;
        for(UINT i=0;factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i)
        {
            DXGI_ADAPTER_DESC1 description{};adapter->GetDesc1(&description);
            if(!(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))
            {std::wcout<<L"GPU: "<<description.Description<<L'\n';break;}adapter.Reset();
        }
        if(!device)throw std::runtime_error("No hardware D3D12 device");
        ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC queueDesc{};
        Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Cannot create GPU fence event");
        auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            D3D12_HEAP_PROPERTIES props{};props.Type=heap;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;
            desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
            ComPtr<ID3D12Resource> resource;Check(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)));return resource;
        };
        std::ifstream shaderFile(argv[1],std::ios::binary);if(!shaderFile)throw std::runtime_error("Cannot open shader");
        std::vector<char> shader{std::istreambuf_iterator<char>(shaderFile),{}};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[0].Descriptor={0,0};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[1].Descriptor={0,0};
        D3D12_ROOT_SIGNATURE_DESC rd{2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> signature,error;Check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error));
        ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader.data(),shader.size()};
        ComPtr<ID3D12PipelineState> pipeline;Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
        constexpr UINT count=8192;const auto inputs=MakeGradientCases(count);
        auto input=buffer(inputs.size()*sizeof(GradientInput),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr;Check(input->Map(0,nullptr,&mapped));memcpy(mapped,inputs.data(),inputs.size()*sizeof(GradientInput));input->Unmap(0,nullptr);
        auto output=buffer(count*sizeof(GradientOutput),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback=buffer(count*sizeof(GradientOutput),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        list->SetComputeRootSignature(root.Get());list->SetComputeRootShaderResourceView(0,input->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());list->SetPipelineState(pipeline.Get());list->Dispatch(count/64,1,1);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&barrier);list->CopyResource(readback.Get(),output.Get());Check(list->Close());
        ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        Check(queue->Signal(fence.Get(),1));Check(fence->SetEventOnCompletion(1,event));
        if(WaitForSingleObject(event,60000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");
        GradientOutput* values=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&values)));
        std::size_t comparisons=0;float largestError=0;
        for(UINT i=0;i<count;++i)
        {
            const auto expected=EvaluateGradientCase(inputs[i]);const auto* actual=reinterpret_cast<const float*>(&values[i]);
            const auto* wanted=reinterpret_cast<const float*>(&expected);
            for(std::size_t axis=0;axis<sizeof(GradientOutput)/sizeof(float);++axis)
            {
                const float errorValue=std::abs(actual[axis]-wanted[axis]);
                // Metadata and policy choices are exact; differential math permits float FMA rounding.
                const bool metadata=axis==6||axis==7||axis==11||axis==15||axis==22||axis==23;
                const float limit=metadata?0.f:2e-6f+4e-5f*std::abs(wanted[axis]);
                if(!std::isfinite(actual[axis])||errorValue>limit)
                    throw std::runtime_error("Gradient GPU mismatch case="+std::to_string(i)+" component="+std::to_string(axis)
                        +" GPU="+std::to_string(actual[axis])+" CPU="+std::to_string(wanted[axis]));
                largestError=std::max(largestError,errorValue);++comparisons;
            }
        }
        readback->Unmap(0,nullptr);CloseHandle(event);
        std::cout<<comparisons<<" CPU/GPU gradient comparisons passed across "<<count<<" regular, singular and repaired contexts; max absolute error="<<largestError<<".\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D12 HRESULT "+std::to_string(unsigned(hr)));}
static std::vector<char> Read(const char* path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error(path);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv)
{
    try{
        if(argc!=4)throw std::runtime_error("shader.dxil requests.f32 expected.f32");
        auto shader=Read(argv[1]),queries=Read(argv[2]),expected=Read(argv[3]);
        if(queries.size()%104||expected.size()!=queries.size()/26)throw std::runtime_error("Bad request sizes");
        const UINT count=UINT(queries.size()/104);if(!count)throw std::runtime_error("No requests");
        ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;
        for(UINT i=0;factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i){
            DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);
            if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))break;
            adapter.Reset();}
        if(!device)throw std::runtime_error("No hardware D3D12 device");
        auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags){
            D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
            ComPtr<ID3D12Resource> r;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;};
        auto input=buffer(queries.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE);
        void* ptr;Check(input->Map(0,nullptr,&ptr));memcpy(ptr,queries.data(),queries.size());input->Unmap(0,nullptr);
        auto output=buffer(expected.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback=buffer(expected.size(),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[0].Descriptor={0,0};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[1].Descriptor={0,0};
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[2].Constants={0,0,1};
        D3D12_ROOT_SIGNATURE_DESC rd{3,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> signature,error;Check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error));
        ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader.data(),shader.size()};
        ComPtr<ID3D12PipelineState> pipeline;Check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
        ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};Check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)));
        list->SetComputeRootSignature(root.Get());list->SetComputeRootShaderResourceView(0,input->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());list->SetComputeRoot32BitConstant(2,count,0);list->Dispatch((count+63)/64,1,1);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&barrier);list->CopyResource(readback.Get(),output.Get());Check(list->Close());
        ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));Check(queue->Signal(fence.Get(),1));
        HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);Check(fence->SetEventOnCompletion(1,event));
        if(WaitForSingleObject(event,60000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");CloseHandle(event);
        Check(readback->Map(0,nullptr,&ptr));auto* got=static_cast<const float*>(ptr);auto* want=reinterpret_cast<const float*>(expected.data());
        float maximum=0;for(UINT i=0;i<count;++i){if(!std::isfinite(got[i]))throw std::runtime_error("Nonfinite GPU visibility");maximum=std::max(maximum,std::abs(got[i]-want[i]));}
        readback->Unmap(0,nullptr);std::cout<<count<<" trained-model CPU/GPU queries, maximum error "<<maximum<<'\n';
        if(maximum>2e-4f)throw std::runtime_error("GPU decoder differs from frozen neural model");return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

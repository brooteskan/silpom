// SPDX-License-Identifier: MIT
#include "FaceCoefficientTestData.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iomanip>
#include <iostream>
#include <string>
using Microsoft::WRL::ComPtr;
using namespace SilPOM::Cpu;
static void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D12 HRESULT "+std::to_string(unsigned(hr)));}
struct GpuResult {uint4 flags;float4 oldDuArea,newDuArea,oldDvTolerance,newDvTolerance,oldNewton,newNewton;};
static_assert(sizeof(GpuResult)==112);
int main(int argc,char** argv)
{
    try
    {
        if(argc!=3&&argc!=4)throw std::runtime_error("Usage: silpom_face_coefficient_gpu_tests prepare.dxil compare.dxil [packed144Bfaces.bin]");
        auto faces=MakeCoefficientFaces();const size_t synthetic=faces.size();if(argc==4)AppendPackedCoefficientFaces(faces,argv[3]);
        const UINT count=UINT(faces.size()),cases=count*4;
        ComPtr<IDXGIFactory6> factory;Hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;
        for(UINT i=0;factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i)
        {
            DXGI_ADAPTER_DESC1 description{};adapter->GetDesc1(&description);
            if(!(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))
            {std::wcout<<L"GPU: "<<description.Description<<L'\n';break;}adapter.Reset();
        }
        if(!device)throw std::runtime_error("No hardware D3D12 device");
        ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};Hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        ComPtr<ID3D12Fence> fence;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Fence event failed");
        auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;D3D12_RESOURCE_DESC d{};
            d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;
            d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
            ComPtr<ID3D12Resource> r;Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;
        };
        auto upload=[&](const void* data,size_t bytes)
        {auto r=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* p=nullptr;Hr(r->Map(0,nullptr,&p));memcpy(p,data,bytes);r->Unmap(0,nullptr);return r;};
        auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);};
        D3D12_DESCRIPTOR_RANGE textureRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,2,0,0};
        D3D12_ROOT_PARAMETER params[6]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[0].Descriptor={0,0};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[1].Descriptor={1,0};
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[2].Descriptor={0,0};
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[3].Descriptor={1,0};
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[4].DescriptorTable={1,&textureRange};
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[5].Constants={0,0,1};
        D3D12_ROOT_SIGNATURE_DESC rd{6,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> signature,error;Hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error));
        ComPtr<ID3D12RootSignature> root;Hr(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)));
        auto pipeline=[&](const char* path)
        {
            std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot open DXIL");
            std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={bytes.data(),bytes.size()};
            ComPtr<ID3D12PipelineState> p;Hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&p)));return p;
        };
        const auto prepare=pipeline(argv[1]),compare=pipeline(argv[2]);
        const auto input=upload(faces.data(),faces.size()*sizeof(PackedCoefficientFace));
        const auto coefficients=buffer(count*48ull,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto output=buffer(cases*sizeof(GpuResult),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto readback=buffer(cases*sizeof(GpuResult),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        const float texels[16]={.2f,.6f,.3f,.8f,.7f,.4f,.9f,.1f,.3f,.8f,.2f,.6f,.9f,.1f,.7f,.4f};
        char padded[1024]{};for(UINT y=0;y<4;++y)memcpy(padded+y*256,texels+y*4,16);
        const auto imageUpload=upload(padded,sizeof(padded));
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=4;td.Height=4;td.DepthOrArraySize=1;
        td.MipLevels=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.SampleDesc.Count=1;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> image;Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image)));
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=imageUpload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,4,4,1,256};D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=image.Get();
        to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        barrier(image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;Hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=td.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(image.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
        ID3D12DescriptorHeap* heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
        list->SetComputeRootShaderResourceView(0,input->GetGPUVirtualAddress());list->SetComputeRootShaderResourceView(1,coefficients->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(2,coefficients->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(3,output->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(4,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRoot32BitConstant(5,count,0);
        list->SetPipelineState(prepare.Get());list->Dispatch((count+63)/64,1,1);
        // Actual stored coefficients cross a UAV->SRV dependency between the
        // two dispatches; this cannot be optimized into one inline expression.
        barrier(coefficients.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->SetPipelineState(compare.Get());list->Dispatch((cases+63)/64,1,1);
        barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyResource(readback.Get(),output.Get());
        Hr(list->Close());ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        Hr(queue->Signal(fence.Get(),1));Hr(fence->SetEventOnCompletion(1,event));
        if(WaitForSingleObject(event,60000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");
        GpuResult* values=nullptr;Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(&values)));
        size_t failures=0,newton=0,gradient=0,hits=0,frozenFailures=0;uint combined=0;
        for(UINT i=0;i<cases;++i)
        {
            const auto& r=values[i];newton+=r.flags.y;gradient+=r.flags.z;hits+=r.flags.w;
            if(r.flags.x)
            {
                ++failures;combined|=r.flags.x;if(i/4>=synthetic)++frozenFailures;
                if(failures<=5)
                    std::cerr<<std::setprecision(9)<<"case="<<i<<" face="<<i/4<<" mask="<<r.flags.x
                        <<" oldDu="<<r.oldDuArea.x<<','<<r.oldDuArea.y<<','<<r.oldDuArea.z<<','<<r.oldDuArea.w
                        <<" newDu="<<r.newDuArea.x<<','<<r.newDuArea.y<<','<<r.newDuArea.z<<','<<r.newDuArea.w
                        <<" oldN="<<r.oldNewton.x<<','<<r.oldNewton.y<<','<<r.oldNewton.z<<','<<r.oldNewton.w
                        <<" newN="<<r.newNewton.x<<','<<r.newNewton.y<<','<<r.newNewton.z<<','<<r.newNewton.w<<'\n';
            }
        }
        readback->Unmap(0,nullptr);CloseHandle(event);
        std::cout<<cases<<" stored-vs-inline coefficient cases across "<<count<<" canonical faces ("<<faces.size()-synthetic
            <<" frozen input records); "<<newton<<" Newton comparisons ("<<hits<<" hits), "<<gradient
            <<" gradient comparisons; mismatches="<<failures<<" (frozen="<<frozenFailures<<") mask="<<combined<<".\n";
        return failures?1:0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

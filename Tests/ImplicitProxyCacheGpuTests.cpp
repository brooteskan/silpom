// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Hr(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D12 HRESULT "+std::to_string(unsigned(hr)));}
struct Input {std::array<float,80> values{};};
struct Result {std::array<uint32_t,24> values{};};
static_assert(sizeof(Input)==320&&sizeof(Result)==96);
static void UInt(Input& input,size_t index,uint32_t value){memcpy(&input.values[index],&value,4);}
static std::vector<Input> MakeCases()
{
    std::vector<Input> result;std::mt19937 random(17117);
    std::uniform_real_distribution<float> unit(-1,1),positive(.01f,3);
    for(unsigned i=0;i<2048;++i)
    {
        Input input;auto& v=input.values;
        v[0]=unit(random)*4;v[1]=unit(random)*4;v[2]=-.1f-positive(random)*12;
        v[4]=positive(random);v[5]=unit(random)*.3f;v[6]=unit(random)*.3f;
        v[8]=unit(random)*.3f;v[9]=positive(random);v[10]=unit(random)*.3f;
        v[14]=1;
        v[16]=unit(random);v[17]=unit(random);v[18]=v[16]+positive(random);v[19]=v[17]+unit(random)*.1f;
        v[20]=v[16]+unit(random)*.1f;v[21]=v[17]+positive(random);
        if(i%2){std::swap(v[18],v[20]);std::swap(v[19],v[21]);}
        for(unsigned corner=0;corner<3;++corner)
        {v[24+corner*4]=unit(random)*.4f;v[25+corner*4]=unit(random)*.4f;v[26+corner*4]=.4f+positive(random);}
        const float ax=unit(random)*3.14f,ay=unit(random)*3.14f;
        const float cx=std::cos(ax),sx=std::sin(ax),cy=std::cos(ay),sy=std::sin(ay);
        const float model[16]={cy,sy*sx,sy*cx,unit(random)*2,0,cx,-sx,unit(random)*2,-sy,cy*sx,cy*cx,unit(random)*2,0,0,0,1};
        memcpy(v.data()+36,model,sizeof(model));
        const float projection[16]={.7330642343f,0,0,0,0,1.3032252789f,0,0,0,0,.000195350658f,.200039073825f,0,0,-1,0};
        memcpy(v.data()+52,projection,sizeof(projection));
        v[68]=.01f+positive(random)*.2f;v[69]=(unit(random)+1)*.5f;v[70]=(i%3)?4.f:1.f;v[71]=(i%5)?0.f:15.f;
        UInt(input,72,65525);UInt(input,77,8); // legacy sentinel, ReliefMode::Lod4pxResolve
        UInt(input,73,(i%3)?4:6);UInt(input,74,i%7==0?1u:0u);UInt(input,75,i%21==0?1u:0u);
        v[76]=projection[5];
        if(i<16)
        {
            const float z[8]={-.2f,-.20001f,-.19999f,0.f,.01f,-1024.f,-1500.f,-.001f};
            v[0]=0;v[1]=0;v[2]=z[i%8];
            const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};memcpy(v.data()+36,identity,sizeof(identity));
        }
        if(i%53==0)for(unsigned row=0;row<16;++row)v[52+row]=0; // degenerate projection => conservative fallback
        result.push_back(input);
    }
    return result;
}
int main(int argc,char** argv)
{
    try
    {
        if(argc!=3&&argc!=4)throw std::runtime_error("Usage: silpom_implicit_proxy_gpu_tests prepare.dxil compare.dxil [--empty-table]");
        const bool emptyTable=argc==4&&std::string(argv[3])=="--empty-table";
        if(argc==4&&!emptyTable)throw std::runtime_error("Unknown argument");
        const auto cases=MakeCases();const UINT count=UINT(cases.size());
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
        auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
        {
            D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
            ComPtr<ID3D12Resource> value;Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&value)));return value;
        };
        const auto input=buffer(cases.size()*sizeof(Input),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr;Hr(input->Map(0,nullptr,&mapped));memcpy(mapped,cases.data(),cases.size()*sizeof(Input));input->Unmap(0,nullptr);
        const auto cache=buffer((count+2ull)*48ull,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto output=buffer(count*sizeof(Result),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto readback=buffer(count*sizeof(Result),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        const uint32_t tableValues[16]={12345u,count+2u,0u,54321u,0xffffffffu,1u,0u,1u,
            12346u,count+2u,0u,54321u,0xffffffffu,2u,0u,0u};
        const auto table=buffer(sizeof(tableValues),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        Hr(table->Map(0,nullptr,&mapped));memcpy(mapped,tableValues,sizeof(tableValues));table->Unmap(0,nullptr);
        D3D12_ROOT_PARAMETER parameters[6]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[0].Descriptor={0,0};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor={1,0};
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor={0,0};
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor={1,0};
        parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[4].Constants={0,0,1};
        parameters[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[5].Descriptor={2,0};
        D3D12_ROOT_SIGNATURE_DESC rd{6,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> signature,error;
        Hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error));
        ComPtr<ID3D12RootSignature> root;Hr(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)));
        auto pipeline=[&](const char* path)
        {
            std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot read shader");
            std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature=root.Get();pd.CS={bytes.data(),bytes.size()};ComPtr<ID3D12PipelineState> value;
            Hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&value)));return value;
        };
        const auto prepare=pipeline(argv[1]),compare=pipeline(argv[2]);
        list->SetComputeRootSignature(root.Get());list->SetComputeRootShaderResourceView(0,input->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1,cache->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(2,cache->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3,output->GetGPUVirtualAddress());list->SetComputeRoot32BitConstant(4,count,0);
        list->SetComputeRootShaderResourceView(5,emptyTable?0:table->GetGPUVirtualAddress());
        list->SetPipelineState(prepare.Get());list->Dispatch((count+63)/64,1,1);
        D3D12_RESOURCE_BARRIER transition{};transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition={cache.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        list->ResourceBarrier(1,&transition);list->SetPipelineState(compare.Get());list->Dispatch((count+63)/64,1,1);
        transition.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&transition);list->CopyResource(readback.Get(),output.Get());Hr(list->Close());
        ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);ComPtr<ID3D12Fence> fence;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Fence event failed");
        Hr(queue->Signal(fence.Get(),1));Hr(fence->SetEventOnCompletion(1,event));
        if(WaitForSingleObject(event,60000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");CloseHandle(event);
        const Result* values=nullptr;Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(const_cast<Result**>(&values))));
        unsigned geometry=0,depth=0,empty=0,lookup=0,conservative=0;
        for(UINT i=0;i<count;++i)
        {
            geometry+=values[i].values[0]!=0;depth+=values[i].values[1]!=0;empty+=values[i].values[2]==2;
            lookup+=values[i].values[22]!=0;
            conservative+=values[i].values[21]==0x3f800000u;
            if((values[i].values[0]||values[i].values[1])&&geometry+depth<16)
            {
                std::cout<<"Mismatch case "<<i<<": corners="<<values[i].values[0]<<", depth="<<values[i].values[1]<<", depthbits="<<values[i].values[20]<<'/'<<values[i].values[21]<<'\n';
                for(unsigned component=0;component<8;++component)
                    if(values[i].values[4+component]!=values[i].values[12+component])
                        std::cout<<"  component "<<component<<" bits "<<values[i].values[4+component]<<'/'<<values[i].values[12+component]<<'\n';
            }
        }
        readback->Unmap(0,nullptr);
        std::cout<<count<<" GPU face/view cases ("<<empty<<" collapsed, "<<conservative<<" conservative depth=1), "<<count*4<<" corner comparisons; geometry mismatches="<<geometry<<", depth mismatches="<<depth<<", lookup failures="<<lookup<<", empty table="<<emptyTable<<"\n";
        return geometry||depth||lookup||conservative==0?1:0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

// SPDX-License-Identifier: MIT
#include <SilPOM/Mesh.h>
#include <SilPOM/HeightPrecision.h>
#include <SilPOM/AppearanceCodec.h>
#include <SilPOM/ReliefLod.h>
#include <SilPOM/DirectionalEstimatorContract.h>
#include <Atom/Feature/PostProcessing/SMAAFeatureProcessorInterface.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/ViewportContext.h>
#include <Atom/RPI.Public/ViewportContextBus.h>
#include <Atom/RPI.Public/Pass/PassAttachment.h>
#include <Atom/RPI.Public/Shader/ShaderResourceGroup.h>
#include <Atom/RPI.Public/Shader/ShaderSystemInterface.h>
#include <AtomCore/Instance/InstanceDatabase.h>
#include <cmath>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/RPISystemInterface.h>
#if defined(AZ_PLATFORM_WINDOWS)
#include <Atom/RHI.Interface/DX12/RHIDX12Interface.h>
#undef near
#undef far
#endif
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/Pass/Pass.h>
#include <Atom/RPI.Public/Pass/PassSystemInterface.h>
#include <Atom/RPI.Reflect/Pass/RasterPassData.h>
#include <Atom/RPI.Reflect/Pass/PassTemplate.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/RPI.Public/Image/ImageSystemInterface.h>
#include <Atom/RPI.Public/Image/StreamingImage.h>
#include <Atom/RPI.Public/Image/StreamingImagePool.h>
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <Atom/RPI.Reflect/Material/MaterialPropertiesLayout.h>
#include <Atom/RPI.Reflect/Material/MaterialAssetCreator.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RHI/Factory.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/Serialization/EditContext.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/RTTI/BehaviorContext.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <AzFramework/Asset/AssetCatalogBus.h>
#include <AzCore/std/containers/map.h>
#include <AzCore/std/containers/set.h>
#include <AzCore/std/parallel/atomic.h>

namespace SilPOM
{
namespace
{
AZ_CVAR(bool, r_silpomReuseDepthHits, false, nullptr, AZ::ConsoleFunctorFlags::Null,
    "Use the experimental DX12 SilPOM hit buffer in MainPipeline (1x, or opt-in r_silpomMsaa4).");
AZ_CVAR(bool,r_silpomMsaa4,false,nullptr,AZ::ConsoleFunctorFlags::Null,
    "Opt into sample-frequency 4x SilPOM. Enabling selects r_multiSampleCount 4; disabling restores 1x. Models/settings are unchanged.");
AZ_CVAR(bool,r_silpomMsaaRawCoverage,true,nullptr,AZ::ConsoleFunctorFlags::Null,
    "Inspect unrepaired 4x sample coverage. Does not affect the approved 1x repair path.");
AZ_CVAR(bool,r_silpomMsaaInspect,false,nullptr,AZ::ConsoleFunctorFlags::Null,
    "Enable diagnostic raw/final hit, depth and shaded-color sample atlases. Adds GPU/readback work; normally off.");

bool SupportsHitMsaa4()
{
#if defined(AZ_PLATFORM_WINDOWS)
    if(AZ::RHI::Factory::Get().GetName()!=AZ::Name("dx12"))return false;
    auto* rhi=AZ::RHI::RHISystemInterface::Get();
    if(!rhi||rhi->GetDeviceCount()==0)return false;
    // Query the actual rendering devices, including each active device.
    for(int index=0;index<rhi->GetDeviceCount();++index)
    {
        auto* native=AZ::DX12::GetDeviceNativeHandle(*rhi->GetDevice(index));
        if(!native)return false;
        const DXGI_FORMAT colors[]={DXGI_FORMAT_R32G32B32A32_UINT,DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM};
        for(auto format:colors)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
            constexpr UINT needed=D3D12_FORMAT_SUPPORT1_RENDER_TARGET|D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET
                |D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format,4,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
            if(FAILED(native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))
                ||(support.Support1&needed)!=needed
                ||FAILED(native->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&quality,sizeof(quality)))
                ||quality.NumQualityLevels==0)return false;
        }
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS depth{DXGI_FORMAT_D32_FLOAT_S8X24_UINT,4,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
        D3D12_FEATURE_DATA_FORMAT_SUPPORT depthLoad{DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
        if(FAILED(native->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&depth,sizeof(depth)))||depth.NumQualityLevels==0
            ||FAILED(native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&depthLoad,sizeof(depthLoad)))
            ||!(depthLoad.Support1&D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD))return false;
    }
    return true;
#else
    return false;
#endif
}

struct HitSampleTransition
    : AZ::RPI::SceneNotificationBus::MultiHandler
    , AZ::TickBus::Handler
{
    AZStd::vector<AZStd::pair<AZ::RPI::RenderPipelinePtr,bool>> paused;
    AZ::u64 resumeTick=0;
    void Resume()
    {
        const bool resume=AZ::RPI::RPISystemInterface::Get()->GetCurrentTick()>=resumeTick;
        for(const auto& entry:paused)
            if(entry.first->GetScene()&&entry.first->GetRootPass()->GetParent())
                entry.first->GetRootPass()->SetEnabled(resume&&entry.second);
        if(resume){paused.clear();AZ::RPI::SceneNotificationBus::MultiHandler::BusDisconnect();AZ::TickBus::Handler::BusDisconnect();}
    }
    void OnTick(float,AZ::ScriptTimePoint) override { Resume(); }
    void OnRenderPipelineChanged(AZ::RPI::RenderPipeline* pipeline,AZ::RPI::SceneNotification::RenderPipelineChangeType type) override
    {
        if(type==AZ::RPI::SceneNotification::RenderPipelineChangeType::Removed)
        {
            AZStd::erase_if(paused,[&](const auto& entry){return entry.first.get()==pipeline;});
            return;
        }
        // MSAA recreates roots. Apply the pause to the new root before drawing.
        for(const auto& entry:paused)if(entry.first.get()==pipeline)pipeline->GetRootPass()->SetEnabled(false);
    }
};
HitSampleTransition& SampleTransition()
{
    static HitSampleTransition transition;
    return transition;
}

unsigned UpdateHitSamplePolicy(AZ::RPI::Scene* scene)
{
    static bool requested=false,raw=true,inspect=false;static unsigned version=0;
    auto& transition=SampleTransition();auto& paused=transition.paused;auto& resumeTick=transition.resumeTick;
    auto* rpi=AZ::RPI::RPISystemInterface::Get();
    if(!paused.empty())
    {
        // Pass attachments update before MeshFeatureProcessor has rebuilt all
        // scene draw packets. Submit no scene draws across that transition.
        transition.Resume();
    }
    if(requested!=r_silpomMsaa4)
    {
        if(r_silpomMsaa4&&!SupportsHitMsaa4())
        {
            AZ_Error("SilPOM",false,"4x MSAA requires supported render, load and sample-count capabilities; retaining 1x.");
            r_silpomMsaa4=false;
        }
        else
        {
            // The application sample count also changes preview/editor scenes.
            // Pause all existing pipeline roots, not only the tower's scene.
            AZStd::set<AZ::RPI::Scene*> scenes;
            if(scene)scenes.insert(scene);
            for(const auto& root:AZ::RPI::PassSystemInterface::Get()->GetRootPass()->GetChildren())
                if(auto* owner=root->GetScene())scenes.insert(owner);
            transition.AZ::RPI::SceneNotificationBus::MultiHandler::BusDisconnect();
            for(auto* owner:scenes)
            {
                for(const auto& pipeline:owner->GetRenderPipelines())
                {
                    const auto found=AZStd::find_if(paused.begin(),paused.end(),[&](const auto& entry){return entry.first==pipeline;});
                    if(found==paused.end())paused.push_back({pipeline,pipeline->GetRootPass()->IsEnabled()});
                    pipeline->GetRootPass()->SetEnabled(false);
                }
                transition.AZ::RPI::SceneNotificationBus::MultiHandler::BusConnect(owner->GetId());
            }
            resumeTick=rpi->GetCurrentTick()+3;
            // Resume even when the last SilPOM component was just disabled.
            if(!transition.AZ::TickBus::Handler::BusIsConnected())transition.AZ::TickBus::Handler::BusConnect();
            requested=r_silpomMsaa4;++version;
            AZ::Interface<AZ::IConsole>::Get()->PerformCommand(requested?"r_multiSampleCount 4":"r_multiSampleCount 1");
        }
    }
    if(raw!=r_silpomMsaaRawCoverage){raw=r_silpomMsaaRawCoverage;++version;}
    if(inspect!=r_silpomMsaaInspect){inspect=r_silpomMsaaInspect;++version;}
    static unsigned previousSamples=1;
    const unsigned samples=AZ::RPI::RPISystemInterface::Get()->GetApplicationMultisampleState().m_samples;
    if(samples!=previousSamples)
    {
        previousSamples=samples;++version;
        AZ::RPI::ShaderSystemInterface::Get()->SetGlobalShaderOption(AZ::Name("o_silpomSampleFrequency"),AZ::RPI::ShaderOptionValue{samples>1});
    }
    return version;
}

// AA is a viewport/application setting, not a material setting. Default meshes
// are passive; the most recently changed explicit selection wins. Keep the
// original per-scene settings until the last override is released, including
// editor/game scene hand-offs. Never rewrite the user's tower quality settings.
struct AntiAliasingOverrides
{
    struct Request { AZ::EntityId entity; AntiAliasing mode; };
    struct PipelineState { AZ::RPI::SceneId scene; AZ::RPI::RenderPipelineId pipeline; AZ::RPI::AntiAliasingMode mode; };
    struct SceneState { AZ::RPI::SceneId scene; bool smaa; };
    AZStd::vector<Request> requests;
    AZStd::vector<PipelineState> pipelines;
    AZStd::vector<SceneState> scenes;
    bool originalMsaa4=false;
    bool checkedMsaa4=false,supportsMsaa4=false;
    AZ::u64 lastTick=~AZ::u64(0);

    void Update(AZ::EntityId entity,AntiAliasing mode)
    {
        auto found=AZStd::find_if(requests.begin(),requests.end(),[&](const auto& r){return r.entity==entity;});
        const bool changed=found==requests.end()?mode!=AntiAliasing::CurrentDefault:found->mode!=mode;
        if(changed)
        {
            if(requests.empty())originalMsaa4=r_silpomMsaa4;
            if(found!=requests.end())requests.erase(found);
            if(mode!=AntiAliasing::CurrentDefault)requests.push_back({entity,mode});
        }
        auto* rpi=AZ::RPI::RPISystemInterface::Get();
        if(requests.empty())
        {
            if(!changed)return;
            for(const auto& state:pipelines)
                if(auto* scene=rpi->GetScene(state.scene))
                    if(auto pipeline=scene->GetRenderPipeline(state.pipeline))
                        pipeline->SetActiveAAMethod(AZ::RPI::RenderPipeline::GetAAMethodNameByIndex(state.mode));
            for(const auto& state:scenes)
                if(auto* scene=rpi->GetScene(state.scene))
                    if(auto* fp=scene->GetFeatureProcessor<AZ::Render::SMAAFeatureProcessorInterface>())fp->SetEnable(state.smaa);
            pipelines.clear();scenes.clear();r_silpomMsaa4=originalMsaa4;
            lastTick=~AZ::u64(0);return;
        }
        if(!changed&&lastTick==rpi->GetCurrentTick())return;
        lastTick=rpi->GetCurrentTick();
        const auto active=requests.back().mode;
        const auto method=active==AntiAliasing::SMAA?AZ::RPI::AntiAliasingMode::SMAA:
            active==AntiAliasing::TAA?AZ::RPI::AntiAliasingMode::TAA:AZ::RPI::AntiAliasingMode::MSAA;
        // Reuse the capability check and guarded sample-count transition below.
        if(active==AntiAliasing::MSAA4&&!checkedMsaa4)
        {
            supportsMsaa4=SupportsHitMsaa4();checkedMsaa4=true;
            AZ_Warning("SilPOM",supportsMsaa4,"4x MSAA is unavailable on this device; retaining single-sample rendering.");
        }
        r_silpomMsaa4=active==AntiAliasing::MSAA4&&supportsMsaa4;
        AZStd::set<AZ::RPI::Scene*> owners;
        for(const auto& root:AZ::RPI::PassSystemInterface::Get()->GetRootPass()->GetChildren())
            if(auto* scene=root->GetScene())owners.insert(scene);
        for(auto* scene:owners)
            for(const auto& pipeline:scene->GetRenderPipelines())
            {
                if(pipeline->GetDescriptor().m_materialPipelineTag!="MainPipeline")continue;
                if(AZStd::none_of(pipelines.begin(),pipelines.end(),[&](const auto& s){return s.scene==scene->GetId()&&s.pipeline==pipeline->GetId();}))
                    pipelines.push_back({scene->GetId(),pipeline->GetId(),pipeline->GetActiveAAMethod()});
                if(pipeline->GetActiveAAMethod()!=method)
                    pipeline->SetActiveAAMethod(AZ::RPI::RenderPipeline::GetAAMethodNameByIndex(method));
                if(auto* fp=scene->GetFeatureProcessor<AZ::Render::SMAAFeatureProcessorInterface>())
                {
                    if(AZStd::none_of(scenes.begin(),scenes.end(),[&](const auto& s){return s.scene==scene->GetId();}))
                        scenes.push_back({scene->GetId(),fp->GetSettings().m_enable});
                    if(fp->GetSettings().m_enable!=(active==AntiAliasing::SMAA))fp->SetEnable(active==AntiAliasing::SMAA);
                }
            }
    }
};
AntiAliasingOverrides& AntiAliasingState() { static AntiAliasingOverrides state;return state; }

AZ::RPI::Ptr<AZ::RPI::Pass> MakeHitPass(const AZ::Name& name, const AZ::Name& templateName, const AZ::RPI::Pass& reference)
{
    auto* system = AZ::RPI::PassSystemInterface::Get();
    const auto passTemplate = system->GetPassTemplate(templateName);
    if (!passTemplate) return {};
    auto data = AZStd::make_shared<AZ::RPI::RasterPassData>(
        *azrtti_cast<const AZ::RPI::RasterPassData*>(passTemplate->m_passData.get()));
    data->m_pipelineViewTag = reference.GetPipelineViewTag();
    AZ::RPI::PassRequest request;
    request.m_passName = name;
    request.m_templateName = templateName;
    request.m_passData = AZStd::move(data);
    return system->CreatePassFromRequest(&request);
}

bool InstallHitPasses(AZ::RPI::Scene& scene, bool required)
{
    if ((!required && !r_silpomReuseDepthHits) || AZ::RHI::Factory::Get().GetName() != AZ::Name("dx12")) return false;
    auto* system = AZ::RPI::PassSystemInterface::Get();
    const AZ::Name hitDepthName("SilPOMHitDepth"), hitForwardName("SilPOMHitForward");
    const AZ::Name classifyName("SilPOMClassifyWallHoles"), fillName("SilPOMFillWallHoles");
    const AZ::Name backupName("SilPOMHitDepthWallBackup"), backupTemplate("SilPOMHitDepthWallBackupTemplate");
    const AZ::Name depthTemplate("SilPOMHitDepthTemplate"), forwardTemplate("SilPOMHitForwardTemplate");
    const AZ::Name clearName("SilPOMClearHits"),clearTemplate("SilPOMClearHitsTemplate"),msaaDepthTemplate("SilPOMHitDepthMsaaTemplate");
    const AZ::Name classifyTemplate("SilPOMClassifyWallHolesTemplate"), fillTemplate("SilPOMFillWallHolesTemplate");
    const AZ::Name msaaClassifyTemplate("SilPOMClassifyWallHolesMsaaTemplate");
    const AZ::Name depthName("DepthPass"), forwardName("Forward");
    bool foundMainPipeline = false;
    for (const auto& pipeline : scene.GetRenderPipelines())
    {
        if (pipeline->GetDescriptor().m_materialPipelineTag != "MainPipeline") continue;
        const auto samples=pipeline->GetRenderSettings().m_multisampleState.m_samples;
        if ((samples!=1 && !(samples==4 && r_silpomMsaa4))
            || !pipeline->FindFirstPass(depthName) || !pipeline->FindFirstPass(forwardName)) return false;
        foundMainPipeline = true;
    }
    if (!foundMainPipeline) return false;
    if ((!system->GetPassTemplate(depthTemplate) || !system->GetPassTemplate(clearTemplate) || !system->GetPassTemplate(msaaDepthTemplate)
        || !system->GetPassTemplate(msaaClassifyTemplate) || (required && !system->GetPassTemplate(backupTemplate))
        ||(r_silpomMsaaInspect&&!system->GetPassTemplate(AZ::Name("SilPOMInspectSamplesTemplate"))))
        && !system->LoadPassTemplateMappings("passes/silpomhittemplates.azasset"))
        return false;

    for (const auto& pipeline : scene.GetRenderPipelines())
    {
        if (pipeline->GetDescriptor().m_materialPipelineTag != "MainPipeline") continue;
        const bool msaa=pipeline->GetRenderSettings().m_multisampleState.m_samples==4;
        if (!pipeline->FindFirstPass(hitDepthName) || !pipeline->FindFirstPass(hitForwardName))
        {
            if(msaa&&!pipeline->FindFirstPass(clearName))
            {
                // Native clearing of this 4x integer RTV hung on the tested
                // DX12 device. A pixel-frequency shader initializes all samples.
                // Keep the established native clear at 1x.
                AZ::RPI::PassRequest request;request.m_passName=clearName;request.m_templateName=clearTemplate;
                auto clear=system->CreatePassFromRequest(&request);
                if(!clear||!pipeline->AddPassAfter(clear,depthName))return false;
            }
            auto depth = MakeHitPass(hitDepthName, msaa?msaaDepthTemplate:depthTemplate, *pipeline->FindFirstPass(depthName));
            auto forward = MakeHitPass(hitForwardName, forwardTemplate, *pipeline->FindFirstPass(forwardName));
            if (!depth || !forward || !pipeline->AddPassAfter(depth, msaa?clearName:depthName)) return false;
            if (!pipeline->AddPassAfter(forward, forwardName))
            {
                depth->QueueForRemoval();
                return false;
            }
        }
        if (required && !pipeline->FindFirstPass(fillName))
        {
            AZ::RPI::PassRequest classifyRequest;
            classifyRequest.m_passName = classifyName;
            classifyRequest.m_templateName = msaa?msaaClassifyTemplate:classifyTemplate;
            AZ::RPI::PassRequest fillRequest;
            fillRequest.m_passName = fillName;
            fillRequest.m_templateName = fillTemplate;
            auto classify = system->CreatePassFromRequest(&classifyRequest);
            auto fill = system->CreatePassFromRequest(&fillRequest);
            if (!classify || !fill || !pipeline->AddPassAfter(classify, hitDepthName)) return false;
            if (!pipeline->AddPassAfter(fill, classifyName))
            {
                classify->QueueForRemoval();
                return false;
            }
        }
        if (required && !pipeline->FindFirstPass(backupName))
        {
            auto backup = MakeHitPass(backupName, backupTemplate, *pipeline->FindFirstPass(depthName));
            if (!backup || !pipeline->AddPassAfter(backup, fillName)) return false;
        }
        if(auto classify=pipeline->FindFirstPass(classifyName))classify->SetEnabled(!msaa||!r_silpomMsaaRawCoverage);
        if(auto fill=pipeline->FindFirstPass(fillName))fill->SetEnabled(!msaa||!r_silpomMsaaRawCoverage);
        // A raster backup plane cannot distinguish a valid subpixel miss from
        // a failed intersection. Never use it to expand 4x silhouette coverage.
        if(auto backup=pipeline->FindFirstPass(backupName))backup->SetEnabled(!msaa);
        const char* inspectionNames[]={"SilPOMInspectRaw","SilPOMInspectFinal","SilPOMInspectColors"};
        for(unsigned i=0;i<3;++i)
        {
            const AZ::Name name(inspectionNames[i]);auto pass=pipeline->FindFirstPass(name);
            if(!pass&&r_silpomMsaaInspect)
            {
                AZ::RPI::PassRequest request;request.m_passName=name;
                request.m_templateName=AZ::Name(i==2?"SilPOMInspectColorsTemplate":"SilPOMInspectSamplesTemplate");
                pass=system->CreatePassFromRequest(&request);
                if(!pass)return false;
                const bool added=i==2?pipeline->AddPassBefore(pass,AZ::Name("MSAAResolveDiffusePass"))
                    :pipeline->AddPassAfter(pass,i==0?hitDepthName:backupName);
                if(!added)return false;
            }
            if(pass)pass->SetEnabled(r_silpomMsaaInspect);
        }
    }
    return true;
}

AZ::u32 AllocateHitFaceBase(size_t faceCount)
{
    // Zero is the cleared/unowned record. Two low bits hold the solver status.
    static AZStd::atomic<AZ::u64> next{1};
    constexpr AZ::u64 limit = AZ::u64(1) << 30;
    AZ::u64 base = next.load(AZStd::memory_order_relaxed);
    do
    {
        if (faceCount > limit - base) return 0;
    } while (!next.compare_exchange_weak(base, base + faceCount, AZStd::memory_order_relaxed));
    return AZ::u32(base);
}

using Material = AZ::Data::Instance<AZ::RPI::Material>;
using Image = AZ::Data::Instance<AZ::RPI::Image>;

Image MakeR16Height(const AZ::Data::Asset<AZ::RPI::StreamingImageAsset>& source, AZStd::span<const AZ::u8> pixels)
{
    // Share across batches and entities, without reusing an older asset revision.
    // Prepare runs on the tick thread.
    const auto key = AZStd::string::format("SilPOM.R16.v1:%s:%d", source.GetId().ToString<AZStd::string>().c_str(),
        source->GetCreationToken());
    const AZ::Uuid id = AZ::Uuid::CreateName(key.c_str());
    auto existing = AZ::Data::InstanceDatabase<AZ::RPI::StreamingImage>::Instance().Find(AZ::Data::InstanceId::CreateUuid(id));
    if (existing) return existing;
    AZStd::vector<AZ::u16> converted(pixels.size() / sizeof(float));
    for (size_t i = 0; i < converted.size(); ++i)
    {
        float value;
        memcpy(&value, pixels.data() + i * sizeof(value), sizeof(value));
        converted[i] = EncodeHeightUnorm16(value);
    }
    const auto& pool = AZ::RPI::ImageSystemInterface::Get()->GetSystemStreamingPool();
    return AZ::RPI::StreamingImage::CreateFromCpuData(*pool, AZ::RHI::ImageDimension::Image2D,
        source->GetImageDescriptor().m_size, AZ::RHI::Format::R16_UNORM,
        converted.data(), converted.size() * sizeof(AZ::u16), id);
}

Image MakeDepthHeightBounds(const AZ::RHI::ImageDescriptor& desc, AZStd::span<const AZ::u8> pixels, bool clamp, bool quantizeR32)
{
    const AZ::u32 width = desc.m_size.m_width;
    const AZ::u32 height = desc.m_size.m_height;
    constexpr AZ::u32 blockSize = 4; // Matches SilPomIntersectMode in Heightfield.azsli.
    const bool r32 = desc.m_format == AZ::RHI::Format::R32_FLOAT;
    const bool r16 = desc.m_format == AZ::RHI::Format::R16_UNORM;
    if ((!r32 && !r16 && desc.m_format != AZ::RHI::Format::R8_UNORM) || width == 0 || height == 0
        || (!clamp && (width != height || width < blockSize || (width & (width - 1)) != 0)))
    {
        return {};
    }
    const AZ::u32 blocksX = (width + blockSize - 1) / blockSize;
    const AZ::u32 blocksY = (height + blockSize - 1) / blockSize;
    struct Range { float low, high; };
    AZStd::vector<Range> ranges;
    ranges.reserve(size_t(blocksX) * blocksY);
    for (AZ::u32 by = 0; by < height; by += blockSize)
    {
        for (AZ::u32 bx = 0; bx < width; bx += blockSize)
        {
            Range range{1.f, 0.f};
            // Four bilinear cells use five texels along each axis. Apply the
            // same edge addressing as the height sampler to the final taps.
            for (AZ::u32 y = by; y <= by + blockSize; ++y)
            {
                const AZ::u32 row = clamp ? AZStd::min(y, height - 1) : y & (height - 1);
                for (AZ::u32 x = bx; x <= bx + blockSize; ++x)
                {
                    const AZ::u32 column = clamp ? AZStd::min(x, width - 1) : x & (width - 1);
                    const size_t index = size_t(row) * width + column;
                    float value;
                    if (r32)
                    {
                        memcpy(&value, pixels.data() + index * sizeof(value), sizeof(value));
                        if (quantizeR32) value = DecodeHeightUnorm16(EncodeHeightUnorm16(value));
                    }
                    else if (r16)
                    {
                        AZ::u16 encoded;
                        memcpy(&encoded, pixels.data() + index * sizeof(encoded), sizeof(encoded));
                        value = DecodeHeightUnorm16(encoded);
                    }
                    else value = float(pixels[index]) / 255.0f;
                    range.low = AZStd::min(range.low, value);
                    range.high = AZStd::max(range.high, value);
                }
            }
            // Allow for CPU/GPU UNORM conversion rounding differences.
            if (!r32 || quantizeR32)
            {
                range.low = AZStd::max(0.f, range.low - 2e-7f);
                range.high = AZStd::min(1.f, range.high + 2e-7f);
            }
            ranges.push_back(range);
        }
    }
    const auto& pool = AZ::RPI::ImageSystemInterface::Get()->GetSystemStreamingPool();
    return AZ::RPI::StreamingImage::CreateFromCpuData(*pool, AZ::RHI::ImageDimension::Image2D,
        AZ::RHI::Size(blocksX, blocksY, 1), AZ::RHI::Format::R32G32_FLOAT,
        ranges.data(), ranges.size() * sizeof(Range));
}
using Handle = AZ::Render::MeshFeatureProcessorInterface::MeshHandle;
struct Vertex { AZ::Vector3 position, normal; AZ::Vector2 uv; };
AZ::Data::Asset<AZ::RPI::BufferAsset> Buffer(const void* data, size_t count, AZ::u32 stride, bool raw = false,bool dynamic=false)
{
    AZ::RPI::BufferAssetCreator creator; creator.Begin(AZ::Uuid::CreateRandom());
    AZ::RHI::BufferDescriptor descriptor;
    descriptor.m_bindFlags = AZ::RHI::BufferBindFlags::InputAssembly | AZ::RHI::BufferBindFlags::ShaderRead;
    if(dynamic)descriptor.m_bindFlags=AZ::RHI::BufferBindFlags::DynamicInputAssembly|AZ::RHI::BufferBindFlags::ShaderRead;
    descriptor.m_byteCount = count * stride;
    creator.SetBuffer(data, descriptor.m_byteCount, descriptor);
    creator.SetBufferViewDescriptor(raw ? AZ::RHI::BufferViewDescriptor::CreateRaw(0, AZ::u32(descriptor.m_byteCount))
        : AZ::RHI::BufferViewDescriptor::CreateStructured(0, AZ::u32(count), stride));
    creator.SetUseCommonPool(dynamic?AZ::RPI::CommonBufferPoolType::DynamicInputAssembly:AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
    AZ::Data::Asset<AZ::RPI::BufferAsset> result; creator.End(result); return result;
}
AZ::Data::Asset<AZ::RPI::ModelAsset> Model(const AZStd::vector<Vertex>& vertices, const AZStd::vector<AZ::u32>& indices, const AZ::Aabb& bounds)
{
    auto indexBuffer = Buffer(indices.data(), indices.size(), 4);
    AZ::RPI::ModelLodAssetCreator lod; lod.Begin(AZ::Uuid::CreateRandom()); lod.SetLodIndexBuffer(indexBuffer); lod.BeginMesh();
    lod.SetMeshAabb(bounds); lod.SetMeshMaterialSlot(0);
    lod.SetMeshIndexBuffer({indexBuffer, AZ::RHI::BufferViewDescriptor::CreateTyped(0, AZ::u32(indices.size()), AZ::RHI::Format::R32_UINT)});
    AZStd::vector<float> positions, normals, uvs, tangents;
    for (const auto& v : vertices)
    {
        positions.insert(positions.end(), {v.position.GetX(), v.position.GetY(), v.position.GetZ()});
        normals.insert(normals.end(), {v.normal.GetX(), v.normal.GetY(), v.normal.GetZ()});
        uvs.insert(uvs.end(), {v.uv.GetX(), v.uv.GetY()});
        auto tangent = v.normal.GetOrthogonalVector().GetNormalizedSafe();
        tangents.insert(tangents.end(), {tangent.GetX(), tangent.GetY(), tangent.GetZ(), 1});
    }
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const auto& a = vertices[indices[i]], &b = vertices[indices[i + 1]], &c = vertices[indices[i + 2]];
        const auto duv1 = b.uv - a.uv, duv2 = c.uv - a.uv;
        const float det = duv1.GetX() * duv2.GetY() - duv1.GetY() * duv2.GetX();
        if (std::abs(det) < 1e-10f) continue;
        const auto raw = ((b.position - a.position) * duv2.GetY() - (c.position - a.position) * duv1.GetY()) / det;
        const auto bitangent = ((c.position - a.position) * duv1.GetX() - (b.position - a.position) * duv2.GetX()) / det;
        for (size_t k = 0; k != 3; ++k)
        {
            const size_t index = indices[i + k]; const auto& n = vertices[index].normal;
            const auto t = (raw - n * raw.Dot(n)).GetNormalizedSafe();
            tangents[index * 4] = t.GetX(); tangents[index * 4 + 1] = t.GetY(); tangents[index * 4 + 2] = t.GetZ();
            tangents[index * 4 + 3] = n.Cross(t).Dot(bitangent) < 0 ? -1.f : 1.f;
        }
    }
    auto stream = [&](const char* name, AZ::u32 semantic, const AZStd::vector<float>& values, AZ::u32 components)
    {
        auto buffer = Buffer(values.data(), vertices.size(), components * 4);
        if (!buffer) return false;
        lod.AddLodStreamBuffer(buffer);
        const auto format = components == 2 ? AZ::RHI::Format::R32G32_FLOAT : components == 3 ? AZ::RHI::Format::R32G32B32_FLOAT : AZ::RHI::Format::R32G32B32A32_FLOAT;
        return lod.AddMeshStreamBuffer(AZ::RHI::ShaderSemantic(AZ::Name(name), semantic), AZ::Name(),
            {buffer, AZ::RHI::BufferViewDescriptor::CreateTyped(0, AZ::u32(vertices.size()), format)});
    };
    if (!stream("POSITION", 0, positions, 3) || !stream("NORMAL", 0, normals, 3)
        || !stream("TANGENT", 0, tangents, 4) || !stream("UV", 0, uvs, 2) || !stream("UV", 1, uvs, 2)) return {};
    lod.EndMesh(); AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset; if (!lod.End(lodAsset)) return {};
    AZ::RPI::ModelAssetCreator model; model.Begin(AZ::Uuid::CreateRandom()); model.SetName("SilPOM mesh generation");
    AZ::RPI::ModelMaterialSlot slot; slot.m_stableId = 0; slot.m_displayName = AZ::Name("Authored material");
    model.AddMaterialSlot(slot); model.AddLodAsset(AZStd::move(lodAsset));
    AZ::Data::Asset<AZ::RPI::ModelAsset> result; model.End(result); return result;
}
template<class T> bool Set(const Material& material, const char* name, const T& value)
{
    auto index = material->FindPropertyIndex(AZ::Name(name));
    if (!index.IsValid() || !material->GetPropertyValue(index).Is<T>()) return false;
    material->SetPropertyValue(index, value); return true;
}
Material Snapshot(const AZ::Data::Asset<AZ::RPI::MaterialAsset>& source,
    const AZ::RPI::MaterialAsset* authored = nullptr, AZ::Data::AssetId replacedHeight = {}, const Image& replacement = {},bool decoded=false,bool keepBaseColor=false)
{
    bool replaceParallax = false;
    AZ::RPI::MaterialAssetCreator creator;
    const auto& type = source->GetMaterialTypeAsset();
    creator.Begin(AZ::Uuid::CreateRandom(), type);
    creator.SetMaterialTypeVersion(type->GetVersion());
    const auto layout = type->GetMaterialPropertiesLayout();
    const auto& values = source->GetPropertyValues();
    for (size_t i = 0; i < values.size(); ++i)
        creator.SetPropertyValue(layout->GetPropertyDescriptor(AZ::RPI::MaterialPropertyIndex(i))->GetName(), values[i]);
    if (authored)
    {
        // Copy asset values (including image assets), before any material functors run.
        // Authored properties can affect PSOs as well as shader constants/options.
        const auto authoredLayout = authored->GetMaterialPropertiesLayout();
        const auto& authoredValues = authored->GetPropertyValues();
        for (size_t i = 0; i < authoredValues.size(); ++i)
        {
            const auto name = authoredLayout->GetPropertyDescriptor(AZ::RPI::MaterialPropertyIndex(i))->GetName();
            if(decoded && ((!keepBaseColor&&name==AZ::Name("baseColor.textureMap"))||name==AZ::Name("normal.textureMap")||name==AZ::Name("roughness.textureMap")||name==AZ::Name("emissive.textureMap"))) continue;
            if (replacement && name == AZ::Name("parallax.textureMap")
                && authoredValues[i].Is<AZ::Data::Asset<AZ::RPI::ImageAsset>>()
                && authoredValues[i].GetValue<AZ::Data::Asset<AZ::RPI::ImageAsset>>().GetId() == replacedHeight)
            {
                // Avoid retaining R32 through the copied PBR parallax property.
                replaceParallax = true;
                continue;
            }
            if (layout->FindPropertyIndex(name).IsValid()) creator.SetPropertyValue(name, authoredValues[i]);
        }
        const AZ::Name doubleSided("general.doubleSided");
        const auto index = layout->FindPropertyIndex(doubleSided);
        if (!index.IsValid() || !values[index.GetIndex()].Is<bool>()) return {};
        // Planar coverage must be double-sided, regardless of the authored value.
        creator.SetPropertyValue(doubleSided, true);
    }
    AZ::Data::Asset<AZ::RPI::MaterialAsset> asset;
    if (!creator.End(asset)) return {};
    auto material = AZ::RPI::Material::Create(asset);
    if (replaceParallax && (!material || !Set(material, "parallax.textureMap", replacement))) return {};
    return material;
}
}
struct MeshController::Generation
{
    struct ProxyPatch {AZStd::vector<AZ::u32> localFaces,sourceFaces;bool protectedBoundary=false,far=false;};
    struct ProxyBatch
    {
        AZ::Data::Instance<AZ::RPI::Buffer> buffer;
        Material material;
        AZ::u32 bufferIndex=0;
        AZStd::vector<AZStd::pair<AZ::u64,AZ::Data::Instance<AZ::RPI::Buffer>>> retired;
        AZStd::vector<AZ::u32> flags;
        AZStd::vector<ProxyPatch> patches;
        AZ::u32 hitFaceBase=0;
        float heightLow=0,heightHigh=0;
    };
    AZStd::vector<ProxyBatch> proxyBatches;
    AZ::u64 proxyFrame=0;
    AZ::Vector3 proxyState=AZ::Vector3::CreateZero();
    using PacketHandler = AZ::Render::ModelDataInstanceInterface::MeshDrawPacketUpdatedEvent::Handler;
    AZStd::vector<Handle> handles;
    AZStd::vector<AZStd::unique_ptr<PacketHandler>> packetHandlers;
    AZStd::vector<Material> materials;
    AZStd::vector<AZ::Data::Asset<AZ::RPI::ModelAsset>> models;
    AZStd::vector<AZ::Data::Instance<AZ::RPI::Buffer>> buffers;
    AZStd::vector<AZ::Data::Instance<AZ::RPI::Image>> heights;
    AZStd::string id;
    size_t faces = 0, bytes = 0;
    size_t sourceHeightBytes = 0, heightBytes = 0;
    bool depthHeightBounds = false;
    bool neuralSeed = false;
    bool neuralCache = false;
    size_t trainedModelBatches = 0;
    size_t learnedMaterialBatches = 0;
    size_t spiralLightBatches = 0;
    AZStd::atomic_bool published{false};
};
MeshController::MeshController() = default;
MeshController::MeshController(const MeshConfig& config) : m_configuration(config) {}
MeshController::~MeshController() = default;
void MeshConfig::Reflect(AZ::ReflectContext* context)
{
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context))
    {
        sc->Class<MeshMaterialBinding>()->Version(3)
            ->Field("MaterialIdentity", &MeshMaterialBinding::m_id)
            ->Field("Material", &MeshMaterialBinding::m_material)
            ->Field("LightingChannelMask", &MeshMaterialBinding::m_lightingChannelMask)
            ->Field("DisableEnvironmentLighting", &MeshMaterialBinding::m_disableEnvironmentLighting)
            ->Field("ReceiveSpiralLight", &MeshMaterialBinding::m_receiveSpiralLight);
        sc->Class<MeshProfileBinding>()->Version(4)->Field("ProfileId", &MeshProfileBinding::m_id)->Field("Height", &MeshProfileBinding::m_height)
            ->Field("NeuralCache", &MeshProfileBinding::m_neuralCache)
            ->Field("NeuralModel", &MeshProfileBinding::m_neuralModel)
            ->Field("UseBaseMesh", &MeshProfileBinding::m_useBaseMesh)
            ->Field("ScaleMetres", &MeshProfileBinding::m_scale)->Field("Reference", &MeshProfileBinding::m_reference)
            ->Field("Tiling", &MeshProfileBinding::m_tiling)->Field("Offset", &MeshProfileBinding::m_offset)->Field("AddressMode", &MeshProfileBinding::m_addressMode);
        sc->Enum<SpiralLightMode>()
            ->Value("Off", SpiralLightMode::Off)
            ->Value("SelectedMaterials", SpiralLightMode::SelectedMaterials)
            ->Value("AllSilPomSurfaces", SpiralLightMode::AllSilPomSurfaces);
        sc->Enum<LocalShadowMethod>()->Value("Baseline",LocalShadowMethod::Baseline)
            ->Value("Gather",LocalShadowMethod::Gather)->Value("Neural",LocalShadowMethod::Neural)
            ->Value("NeuralOnly",LocalShadowMethod::NeuralOnly);
        sc->Enum<LightingEstimator>()->Value("CurrentRouting",LightingEstimator::CurrentRouting)
            ->Value("PhysicalRayNeural",LightingEstimator::PhysicalRayNeural)->Value("DirectionalField32",LightingEstimator::DirectionalField32)
            ->Value("AngularOccluders4",LightingEstimator::AngularOccluders4)->Value("AngularOccluders8",LightingEstimator::AngularOccluders8)
            ->Value("AngularEdges4",LightingEstimator::AngularEdges4);
        sc->Enum<AntiAliasing>()->Value("CurrentDefault",AntiAliasing::CurrentDefault)
            ->Value("SMAA",AntiAliasing::SMAA)->Value("TAA",AntiAliasing::TAA)->Value("MSAA4",AntiAliasing::MSAA4);
        sc->Class<MeshConfig, AZ::ComponentConfig>()->Version(22)->Field("Surface", &MeshConfig::m_surface)
            ->Field("AntiAliasing", &MeshConfig::m_antiAliasing)
            ->Field("LightingEstimator", &MeshConfig::m_lightingEstimator)
            ->Field("DiffuseAccessibility", &MeshConfig::m_diffuseAccessibility)
            ->Field("Materials", &MeshConfig::m_materials)->Field("Profiles", &MeshConfig::m_profiles)
            ->Field("ReliefDepthMultiplier", &MeshConfig::m_reliefDepthMultiplier)
            ->Field("ValidationIterations", &MeshConfig::m_validationIterations)
            ->Field("ValidationRetryCap", &MeshConfig::m_validationRetryCap)
            ->Field("ValidationToleranceMultiplier", &MeshConfig::m_validationToleranceMultiplier)
            ->Field("UseR16HeightTextures", &MeshConfig::m_useR16HeightTextures)
            ->Field("UseValidationGather", &MeshConfig::m_useValidationGather)
            ->Field("UseLevelTwoPredictor", &MeshConfig::m_useLevelTwoPredictor)
            ->Field("UseLevelFourWallMaterial", &MeshConfig::m_useLevelFourWallMaterial)
            ->Field("UseLevelFourShared", &MeshConfig::m_useLevelFourShared)
            ->Field("UseLevelFiveProxy", &MeshConfig::m_useLevelFiveProxy)
            ->Field("UseLevelFiveDepth", &MeshConfig::m_useLevelFiveDepth)
            ->Field("LevelFivePixelBudget", &MeshConfig::m_levelFivePixelBudget)
            ->Field("ReliefLodPixelBudget", &MeshConfig::m_reliefLodPixelBudget)
            ->Field("MaxReliefDistance", &MeshConfig::m_maxReliefDistance)
            ->Field("ValidationDistanceStart", &MeshConfig::m_validationDistanceStart)
            ->Field("ValidationDistanceEnd", &MeshConfig::m_validationDistanceEnd)
            ->Field("DistantValidationIterations", &MeshConfig::m_distantValidationIterations)
            ->Field("DistantValidationRetryCap", &MeshConfig::m_distantValidationRetryCap)
            ->Field("DistantValidationToleranceMultiplier", &MeshConfig::m_distantValidationToleranceMultiplier)
            ->Field("SpiralLightShadowSteps", &MeshConfig::m_spiralLightShadowSteps)
            ->Field("LocalShadowMethod", &MeshConfig::m_localShadowMethod)
            ->Field("NeuralShadowConfidence", &MeshConfig::m_neuralShadowConfidence)
            ->Field("UseHeightDerivedNormals", &MeshConfig::m_useHeightDerivedNormals)
            ->Field("MaxCells", &MeshConfig::m_maxCells)->Field("ReliefShadowSteps", &MeshConfig::m_reliefShadowSteps)
            ->Field("NeuralDirectionalVisibility", &MeshConfig::m_neuralDirectionalVisibility)
            ->Field("ProxyDirectionalResponse", &MeshConfig::m_proxyDirectionalResponse)
            ->Field("SpiralLightMode", &MeshConfig::m_spiralLightMode)
            ->Field("SpiralLightBrightness", &MeshConfig::m_spiralLightBrightness)
            ->Field("SpiralLightPath", &MeshConfig::m_spiralLightPath)
            ->Field("SpiralLightPeakLux", &MeshConfig::m_spiralLightPeakLux)
            ->Field("SpiralLightEnd", &MeshConfig::m_spiralLightEnd)
            ->Field("SpiralLightWindows", &MeshConfig::m_spiralLightWindows)
            ->Field("UseDepthHeightBounds", &MeshConfig::m_useDepthHeightBounds)
            ->Field("UseNeuralSeed", &MeshConfig::m_useNeuralSeed);
    }
}
bool MeshConfig::UsesTrainedProfiles() const
{
    return AZStd::any_of(m_profiles.begin(), m_profiles.end(), [](const auto& profile) { return profile.IsTrained(); });
}
AZ::Crc32 MeshConfig::GetReliefDepthVisibility() const
{
    return UsesTrainedProfiles() ? AZ::Edit::PropertyVisibility::Show : AZ::Edit::PropertyVisibility::Hide;
}
void MeshController::Reflect(AZ::ReflectContext* context)
{
    MeshConfig::Reflect(context);
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context)) sc->Class<MeshController>()->Version(1)->Field("Configuration", &MeshController::m_configuration);
    if (auto* bc = azrtti_cast<AZ::BehaviorContext*>(context))
        bc->EBus<MeshRequestBus>("SilPomMeshRequestBus")->Attribute(AZ::Script::Attributes::Module, "silpom")
            ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Common)
            ->Event("GetStatus", &MeshRequests::GetStatus)->Event("IsReady", &MeshRequests::IsReady)
            ->Event("GetProxyLodState",&MeshRequests::GetProxyLodState)
            ->Event("GetProxyFaceOwners",&MeshRequests::GetProxyFaceOwners)
            ->Event("GetAntiAliasingState",&MeshRequests::GetAntiAliasingState);
}
void MeshComponent::Reflect(AZ::ReflectContext* context)
{
    BaseClass::Reflect(context);
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context)) sc->Class<MeshComponent, BaseClass>()->Version(1);
}
void MeshController::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& s) { s.push_back(AZ_CRC_CE("MeshService")); s.push_back(AZ_CRC_CE("SilPomMeshService")); }
void MeshController::GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& s) { s.push_back(AZ_CRC_CE("MeshService")); s.push_back(AZ_CRC_CE("NonUniformScaleService")); }
void MeshController::GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& s) { s.push_back(AZ_CRC_CE("TransformService")); }
bool MeshController::IsReady() const
{
    // A retained last-known-good generation is visible, but is not readiness
    // for a newly requested configuration which failed validation.
    return m_active && !m_dirty && !m_pending && m_status.starts_with("Ready:");
}
AZ::Vector3 MeshController::GetAntiAliasingState() const
{
    float method=-1,smaa=0;
    if(auto* scene=AZ::RPI::Scene::GetSceneForEntityId(m_entity))
    {
        for(const auto& pipeline:scene->GetRenderPipelines())
            if(pipeline->GetDescriptor().m_materialPipelineTag=="MainPipeline")method=float(pipeline->GetActiveAAMethod());
        if(auto* fp=scene->GetFeatureProcessor<AZ::Render::SMAAFeatureProcessorInterface>())smaa=fp->GetSettings().m_enable?1.f:0.f;
    }
    return AZ::Vector3(float(AZ::RPI::RPISystemInterface::Get()->GetApplicationMultisampleState().m_samples),method,smaa);
}
void MeshController::ConnectAssets()
{
    AZ::Data::AssetBus::MultiHandler::BusDisconnect();
    AZStd::set<AZ::Data::AssetId> connected;
    auto queue = [&](auto& asset)
    {
        if (asset.GetId().IsValid() && connected.insert(asset.GetId()).second)
        { AZ::Data::AssetBus::MultiHandler::BusConnect(asset.GetId()); asset.QueueLoad(); }
    };
    queue(m_configuration.m_surface); queue(m_template);
    if(m_configuration.m_useLevelFourWallMaterial||m_configuration.m_useLevelFourShared)
    {
        AZ::Data::AssetId id;
        AZ::Data::AssetCatalogRequestBus::BroadcastResult(id,&AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
            m_configuration.m_useLevelFourShared?"assets/silpomlearned/wall-shared.silpommaterial":"assets/silpomlearned/wall.silpommaterial",azrtti_typeid<LearnedMaterialAsset>(),false);
        if(id.IsValid())m_learnedMaterial=AZ::Data::AssetManager::Instance().GetAsset<LearnedMaterialAsset>(id,AZ::Data::AssetLoadBehavior::NoLoad);
        queue(m_learnedMaterial);
    }
    if(m_configuration.m_useLevelFiveProxy)
    {
        AZ::Data::AssetId id;
        // The two encodings have different source stems and products.
        AZ::Data::AssetCatalogRequestBus::BroadcastResult(id,&AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
            m_configuration.m_useLevelFiveDepth?"assets/silpomlearned/wall-depth.silpommaterial":"assets/silpomlearned/wall-appearance.silpommaterial",azrtti_typeid<LearnedMaterialAsset>(),false);
        if(id.IsValid())m_appearance=AZ::Data::AssetManager::Instance().GetAsset<LearnedMaterialAsset>(id,AZ::Data::AssetLoadBehavior::NoLoad);
        queue(m_appearance);
    }
    for (auto& binding : m_configuration.m_materials) queue(binding.m_material);
    for (auto& profile : m_configuration.m_profiles)
    {
        if (profile.m_useBaseMesh) continue;
        queue(profile.m_height);
        queue(profile.m_neuralCache);
    }
}
void MeshController::Activate(AZ::EntityId entity)
{
    m_entity = entity; m_dirty = true;
    AZ::TransformBus::EventResult(m_world, entity, &AZ::TransformBus::Events::GetWorldTM);
    AZ::Data::AssetId materialId;
    AZ::Data::AssetCatalogRequestBus::BroadcastResult(materialId, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
        "materials/silpommesh.template.azmaterial", azrtti_typeid<AZ::RPI::MaterialAsset>(), false);
    if (materialId.IsValid()) m_template = AZ::Data::AssetManager::Instance().GetAsset<AZ::RPI::MaterialAsset>(materialId, AZ::Data::AssetLoadBehavior::NoLoad);
    ConnectAssets();
    AZ::TransformNotificationBus::Handler::BusConnect(entity); MeshRequestBus::Handler::BusConnect(entity); AZ::TickBus::Handler::BusConnect();
}
void MeshController::Release(AZStd::unique_ptr<Generation>& generation)
{
    if (generation && m_processor) for (auto& handle : generation->handles) if (handle.IsValid()) m_processor->ReleaseMesh(handle);
    generation.reset();
}
void MeshController::Deactivate()
{
    AntiAliasingState().Update(m_entity,AntiAliasing::CurrentDefault);
    UpdateHitSamplePolicy(AZ::RPI::Scene::GetSceneForEntityId(m_entity));
    AZ::RPI::SceneNotificationBus::Handler::BusDisconnect();
    m_checkHitPasses = false;
    AZ::TickBus::Handler::BusDisconnect(); MeshRequestBus::Handler::BusDisconnect();
    AZ::TransformNotificationBus::Handler::BusDisconnect(); AZ::Data::AssetBus::MultiHandler::BusDisconnect();
    Release(m_pending); Release(m_active); m_processor = nullptr; m_entity.SetInvalid(); m_status = "Inactive";
}
void MeshController::SetConfiguration(const MeshConfig& config)
{
    m_configuration = config; m_dirty = true;
    if (m_entity.IsValid())
    {
        Release(m_pending);
        ConnectAssets();
    }
}
void MeshController::OnTransformChanged(const AZ::Transform&, const AZ::Transform& world)
{
    const bool scaleChanged = m_world.GetUniformScale() != world.GetUniformScale();
    m_world = world;
    if (scaleChanged) { m_dirty = true; return; }
    // Translation/rotation do not change the model-space surface. Scale does,
    // because displacement amplitude is measured in world metres.
    auto rigid = world; rigid.SetUniformScale(1);
    if (m_processor)
        for (auto* generation : {m_active.get(), m_pending.get()})
            if (generation)
            {
                for (const auto& handle : generation->handles) m_processor->SetTransform(handle, rigid);
            }
}
void MeshController::OnAssetReady(AZ::Data::Asset<AZ::Data::AssetData> asset) { OnAssetReloaded(asset); }
void MeshController::OnAssetReloaded(AZ::Data::Asset<AZ::Data::AssetData> asset)
{
    if (asset.GetId() == m_configuration.m_surface.GetId()) m_configuration.m_surface = asset;
    if (asset.GetId() == m_template.GetId()) m_template = asset;
    if (asset.GetId() == m_learnedMaterial.GetId()) m_learnedMaterial = asset;
    if (asset.GetId() == m_appearance.GetId()) m_appearance = asset;
    for (auto& binding : m_configuration.m_materials) if (asset.GetId() == binding.m_material.GetId()) binding.m_material = asset;
    for (auto& profile : m_configuration.m_profiles)
    {
        if (asset.GetId() == profile.m_height.GetId()) profile.m_height = asset;
        if (asset.GetId() == profile.m_neuralCache.GetId()) profile.m_neuralCache = asset;
    }
    m_dirty = true;
}
void MeshController::OnAssetError(AZ::Data::Asset<AZ::Data::AssetData>)
{ Release(m_pending); m_dirty = false; m_status = "Asset load failed; previous complete generation retained. Check Asset Processor."; }

void MeshController::OnRenderPipelineChanged(AZ::RPI::RenderPipeline* pipeline, AZ::RPI::SceneNotification::RenderPipelineChangeType)
{
    if (pipeline && pipeline->GetDescriptor().m_materialPipelineTag == "MainPipeline")
    {
        m_checkHitPasses = true;
    }
}

bool MeshController::Prepare()
{
    auto& c = m_configuration;
    if(static_cast<AZ::u32>(c.m_lightingEstimator)>5u)
    {m_status="Unknown lighting estimator; previous complete generation retained";return false;}
    if(c.m_diffuseAccessibility && static_cast<AZ::u32>(c.m_lightingEstimator)<2u)
    {m_status="Diffuse accessibility requires a directional field or angular occluder model";return false;}
    if(c.m_lightingEstimator!=LightingEstimator::CurrentRouting && c.m_localShadowMethod!=LocalShadowMethod::NeuralOnly)
    {m_status="Lighting estimator comparison requires neural-only local visibility";return false;}
    if(c.m_lightingEstimator!=LightingEstimator::CurrentRouting && !c.m_useLevelFourShared)
    {m_status="Directional field / angular occluders require the canonical wall's Level 4B shared features";return false;}
    if (!c.m_surface.GetId().IsValid()) { m_status = "Assign an imported .silpommesh surface"; return false; }
    if (!c.m_surface.IsReady()) { m_status = "Loading mesh surface"; return true; }
    if (!m_template.GetId().IsValid()) { m_status = "SilPOM mesh shader template is missing; process Gem assets"; return false; }
    if (!m_template.IsReady()) { m_status = "Loading mesh material template"; return true; }
    const auto& asset = *c.m_surface;
    if(c.m_useLevelFourShared&&c.m_useLevelFourWallMaterial)
    {m_status="Choose Level 4A or Level 4B, not both";return false;}
    if(c.m_useLevelFiveDepth&&!c.m_useLevelFiveProxy)
    {m_status="Level 5B bounded depth requires Level 5A proxy selection";return false;}
    if(c.m_useLevelFiveProxy)
    {
        if(c.m_useLevelFourWallMaterial){m_status="Level 5 uses ordinary base color; disable Level 4A for this independent comparison";return false;}
        // Near hits already honor useHeightDerivedNormals in the forward
        // shader. Proxy appearance is decoded independently of that switch.
        if(c.m_localShadowMethod!=LocalShadowMethod::NeuralOnly)
        {m_status="Level 5A requires neural-only local visibility";return false;}
        if(!std::isfinite(c.m_levelFivePixelBudget)||c.m_levelFivePixelBudget<.05f||c.m_levelFivePixelBudget>4.f)
        {m_status="Level 5A pixel error budget must be 0.05-4 pixels";return false;}
        if(!m_appearance.IsReady()){m_status=c.m_useLevelFiveDepth?"Process Assets/SilPOMLearned/wall-depth.l5depth before enabling Level 5B":"Process Assets/SilPOMLearned/wall-appearance.l5app before enabling Level 5A";return m_appearance.GetId().IsValid();}
        if(!m_appearance->Validate()||memcmp(m_appearance->Header().magic,c.m_useLevelFiveDepth?"SILPDEP5":"SILPAPP5",8))
        {m_status="Invalid Level 5A appearance bundle";return false;}
    }
    if(c.m_useLevelFourWallMaterial||c.m_useLevelFourShared)
    {
        if(c.m_localShadowMethod!=LocalShadowMethod::NeuralOnly){m_status="Level 4A requires neural-only local visibility; select it before enabling material decoding";return false;}
        if(!m_learnedMaterial.GetId().IsValid()){m_status=c.m_useLevelFourShared?"Process Assets/SilPOMLearned/wall-shared.l4shared before enabling Level 4B":"Process Assets/SilPOMLearned/wall.l4mat before enabling Level 4A";return false;}
        if(!m_learnedMaterial.IsReady()){m_status="Loading Level 4 material";return true;}
        if(!m_learnedMaterial->Validate()||memcmp(m_learnedMaterial->Header().magic,c.m_useLevelFourShared?"SILPSHR4":"SILPMAT4",8))
        {m_status="Invalid Level 4 material bundle";return false;}
        if(static_cast<AZ::u32>(c.m_lightingEstimator)>=2u && m_learnedMaterial->m_generation!=DirectionalEstimatorContract::SharedGeneration)
        {m_status="Directional estimator features changed; retrain the full-wall heads before comparison";return false;}
    }
    if (asset.m_surfaceVersion != MeshSurfaceAsset::Version || asset.m_faces.empty()) { m_status = "Unsupported/empty mesh surface"; return false; }
    bool initialized = false;
    for (const auto& material : asset.m_materials)
        if (AZStd::none_of(c.m_materials.begin(), c.m_materials.end(), [&](const auto& b) { return b.m_id == material.m_id; }))
        { MeshMaterialBinding binding; binding.m_id = material.m_id; c.m_materials.push_back(binding); initialized = true; }
    for (const auto& face : asset.m_faces)
        if (face.m_region && AZStd::none_of(c.m_profiles.begin(), c.m_profiles.end(), [&](const auto& p) { return p.m_id == face.m_profile; }))
        { MeshProfileBinding profile; profile.m_id = face.m_profile; c.m_profiles.push_back(profile); initialized = true; }
    if (initialized) { m_status = "Material slots and profiles discovered; assign their materials and height images"; return false; }
    AZStd::map<AZStd::string, const MeshMaterialBinding*> materialBindings;
    AZStd::map<AZ::u32, MeshProfileBinding> profiles;
    for (const auto& b : c.m_materials)
    {
        if (!materialBindings.emplace(b.m_id, &b).second) { m_status = "Duplicate material binding"; return false; }
        if (!b.m_material.GetId().IsValid()) { m_status = "Assign material: " + b.m_id; return false; }
        if (!b.m_material.IsReady()) { m_status = "Loading materials"; return true; }
    }
    for (const auto& p : c.m_profiles)
    {
        if (!profiles.emplace(p.m_id, p).second || p.m_id == 0) { m_status = "Duplicate/zero profile ID"; return false; }
        if (p.m_useBaseMesh) continue;
        if (!p.m_height.GetId().IsValid()) { m_status = AZStd::string::format("Assign height texture for profile %u", p.m_id); return false; }
        if (!p.m_height.IsReady()) { m_status = "Loading height textures"; return true; }
        if (c.m_useNeuralSeed && p.m_neuralCache.GetId().IsValid() && !p.m_neuralCache.IsReady())
        {
            m_status = "Loading neural hit cache";
            return true;
        }
    }
    if (static_cast<AZ::u32>(c.m_localShadowMethod)>3 || !std::isfinite(c.m_neuralShadowConfidence)
        || c.m_neuralShadowConfidence<.9f || c.m_neuralShadowConfidence>.99999f)
    { m_status="Invalid local shadow method or confidence (0.9-0.99999)";return false; }
    if (c.m_validationIterations < 1 || c.m_validationIterations > 16 || c.m_validationRetryCap > 4
        || !std::isfinite(c.m_validationToleranceMultiplier) || c.m_validationToleranceMultiplier < 0.01f
        || c.m_validationToleranceMultiplier > 100.0f)
    { m_status = "Invalid neural validation settings: iterations 1-16, retries 0-4, tolerance 0.01-100"; return false; }
    if (!std::isfinite(c.m_reliefLodPixelBudget) || c.m_reliefLodPixelBudget > 64.0f
        || !std::isfinite(c.m_maxReliefDistance) || c.m_maxReliefDistance > 100000.0f
        || !std::isfinite(c.m_validationDistanceStart) || c.m_validationDistanceStart > 100000.0f
        || !std::isfinite(c.m_validationDistanceEnd) || c.m_validationDistanceEnd > 100000.0f
        || c.m_distantValidationIterations < 1 || c.m_distantValidationIterations > 16
        || c.m_distantValidationRetryCap > 4
        || !std::isfinite(c.m_distantValidationToleranceMultiplier) || c.m_distantValidationToleranceMultiplier < 0.01f || c.m_distantValidationToleranceMultiplier > 100.0f
        || c.m_spiralLightShadowSteps > 64
        || c.m_reliefLodPixelBudget < 0.0f || c.m_maxReliefDistance < 0.0f
        || c.m_validationDistanceStart < 0.0f || c.m_validationDistanceEnd < 0.0f
        || (c.m_validationDistanceEnd > 0.0f && c.m_validationDistanceEnd <= c.m_validationDistanceStart))
    { m_status = "Invalid quality settings: LOD 0-64, nonnegative distances, fade end above start, iterations 1-16, retries 0-4, tolerance 0.01-100, shadows 0-64"; return false; }
    const float scale = m_world.GetUniformScale();
    if (!std::isfinite(scale) || scale <= 0 || c.m_maxCells < 1 || c.m_maxCells > 65536 || c.m_reliefShadowSteps > 64)
    { m_status = "Invalid positive uniform scale, cell budget or relief shadow steps (0-64)"; return false; }
    auto* scene = AZ::RPI::Scene::GetSceneForEntityId(m_entity);
    if (!scene) { m_status = "Waiting for render scene"; return true; }
    m_processor = scene->GetFeatureProcessor<AZ::Render::MeshFeatureProcessorInterface>();
    if (!m_processor) { m_status = "Waiting for MeshFeatureProcessor"; return true; }
    const bool requiresTrainedHits = c.UsesTrainedProfiles();
    const float depthMultiplier = requiresTrainedHits ? c.m_reliefDepthMultiplier : 1.0f;
    if (!std::isfinite(depthMultiplier) || depthMultiplier < 0.0f || depthMultiplier > MeshConfig::MaxReliefDepthMultiplier)
    { m_status = "Relief depth multiplier must be between 0 and 2"; return false; }
    const bool reuseDepthHits = InstallHitPasses(*scene, requiresTrainedHits);
    const bool neuralVisibilityOnly = c.m_localShadowMethod == LocalShadowMethod::NeuralOnly;
    if((c.m_neuralDirectionalVisibility||c.m_proxyDirectionalResponse)&&!neuralVisibilityOnly)
    {m_status="Learned directional controls require neural-only visibility; they never enable a marcher";return false;}
    if (neuralVisibilityOnly && (!requiresTrainedHits || !reuseDepthHits || std::abs(depthMultiplier-.5f)>1e-6f))
    { m_status="Neural-only visibility requires trained profiles, calibrated relief depth 0.5 and DX12 MainPipeline hit reuse";return false; }
    if (neuralVisibilityOnly && ((c.m_spiralLightShadowSteps>0 && c.m_spiralLightShadowSteps<5)
        || (c.m_reliefShadowSteps>0 && c.m_reliefShadowSteps<5)))
    { m_status="Neural-only visibility requires shadow schedules of 0 (off) or 5-64; it never switches to marching";return false; }
    if (requiresTrainedHits && (!reuseDepthHits || !c.m_useNeuralSeed || c.m_maxCells != 65525 || c.m_reliefShadowSteps != 0))
    {
        m_status = "Trained profiles require DX12 winning-hit resolve (Maximum cells 65525) and legacy relief steps 0; use the learned directional controls";
        return true;
    }
    if (AZ::Render::r_meshInstancingEnabled)
    { m_status = "Planar face renderer currently requires r_meshInstancingEnabled=false"; return false; }
    auto candidate = AZStd::make_unique<Generation>(); candidate->id = asset.m_generation;
    if ((c.m_useLevelFourWallMaterial || c.m_useLevelFourShared) && m_learnedMaterial.IsReady())
        candidate->id += ":" + m_learnedMaterial->m_generation;
    if (c.m_useLevelFiveProxy && m_appearance.IsReady()) candidate->id += ":" + m_appearance->m_generation;
    auto fail = [&](const AZStd::string& message) { Release(candidate); m_status = message; return false; };
    if (static_cast<AZ::u32>(c.m_spiralLightMode) > static_cast<AZ::u32>(SpiralLightMode::AllSilPomSurfaces))
        return fail("Invalid spiral light mode");
    if (!std::isfinite(c.m_spiralLightBrightness) || c.m_spiralLightBrightness < 0.0f || c.m_spiralLightBrightness > 100.0f)
        return fail("Spiral light master brightness must be between 0 and 100");
    const bool hasSpiralLight = c.m_spiralLightMode != SpiralLightMode::Off
        && c.m_spiralLightBrightness > 0.0f
        && (c.m_spiralLightPeakLux > 0.0f || !c.m_spiralLightWindows.empty());
    const AZ::Vector4 spiralPath = c.m_spiralLightPath * scale;
    const AZ::Vector2 spiralEnd = c.m_spiralLightEnd * scale;
    if (hasSpiralLight && (!spiralEnd.IsFinite() || spiralEnd.GetY() < 0.0f))
        return fail("Spiral light needs a finite end height and nonnegative fade distance");
    if (hasSpiralLight && (!spiralPath.IsFinite() || !std::isfinite(c.m_spiralLightPeakLux)
        || std::abs(spiralPath.GetW()) < 1e-4f || AZ::Vector2(spiralPath.GetX(), spiralPath.GetY()).GetLengthSq() < 1e-8f))
        return fail("Spiral light needs a local window position and a nonzero rise per turn");

    const auto windowCount = hasSpiralLight ? aznumeric_cast<AZ::u32>(c.m_spiralLightWindows.size()) : 0u;
    AZ::u32 windowBufferIndex = 0;
    if (windowCount > 0)
    {
        if (windowCount > MeshConfig::MaxAdditionalSpiralWindows)
            return fail("Spiral light supports up to eight additional windows");
        AZStd::vector<float> packedWindows;
        packedWindows.reserve(2 * windowCount);
        for (const auto& window : c.m_spiralLightWindows)
        {
            if (!window.IsFinite() || window.GetY() < 0.0f)
                return fail("Spiral windows require finite turns and nonnegative peak lux");
            packedWindows.insert(packedWindows.end(), {window.GetX(), window.GetY()});
        }
        auto buffer = AZ::RPI::Buffer::FindOrCreate(Buffer(packedWindows.data(), packedWindows.size(), sizeof(float), true));
        if (!buffer) return fail("Cannot allocate spiral window data");
        buffer->WaitForUpload();
        const auto bindless = buffer->GetBufferView()->GetBindlessReadIndex();
        if (bindless.size() != 1 || bindless.begin()->second == AZ::u32(-1))
            return fail("Spiral windows require one bindless-capable GPU");
        windowBufferIndex = bindless.begin()->second;
        candidate->buffers.push_back(buffer);
    }
    AZ::Transform rigid = m_world; rigid.SetUniformScale(1);
    using Key = AZStd::pair<AZStd::string, AZ::u32>;
    AZStd::map<Key, AZStd::vector<AZ::u32>> groups;
    AZStd::map<AZ::u32, Image> depthHeightBounds;
    AZStd::map<AZ::Data::AssetId, Image> heightImages;
    size_t decodedBatches=0;
    for (AZ::u32 i = 0; i < asset.m_faces.size(); ++i)
    {
        const auto& f = asset.m_faces[i];
        const auto profile = f.m_region ? profiles.find(f.m_profile) : profiles.end();
        if (f.m_corners.size() != 3 || !materialBindings.contains(f.m_materialId) || (f.m_region && profile == profiles.end()))
            return fail("Invalid surface/material/profile binding; reimport the mesh");
        const AZ::u32 displacedProfile = profile != profiles.end() && !profile->second.m_useBaseMesh ? f.m_profile : 0;
        groups[{f.m_materialId, displacedProfile}].push_back(i);
    }
    {
        for (const auto& [key, faces] : groups)
        {
            const auto& binding = *materialBindings.find(key.first)->second;
            auto source = Snapshot(binding.m_material);
            if (!source) return fail("Cannot create authored material");
            Material material = source;
            AZStd::vector<Vertex> vertices; AZStd::vector<AZ::u32> indices;
            AZ::Aabb bounds = AZ::Aabb::CreateNull();
            if (key.second == 0)
            {
                for (auto index : faces) for (const auto& corner : asset.m_faces[index].m_corners)
                {
                    indices.push_back(AZ::u32(vertices.size()));
                    vertices.push_back({corner.m_position * scale, corner.m_normal, corner.m_uv});
                    bounds.AddPoint(corner.m_position * scale);
                }
            }
            else
            {
                AZ::Data::AssetId standardType;
                AZ::Data::AssetCatalogRequestBus::BroadcastResult(standardType, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
                    "materials/types/standardpbr_generated.azmaterialtype", azrtti_typeid<AZ::RPI::MaterialTypeAsset>(), false);
                const auto opacity = source->FindPropertyIndex(AZ::Name("opacity.mode"));
                if (!standardType.IsValid() || binding.m_material->GetMaterialTypeAsset().GetId() != standardType
                    || !opacity.IsValid() || !source->GetPropertyValue(opacity).Is<AZ::u32>()
                    || source->GetPropertyValue(opacity).GetValue<AZ::u32>() != 0)
                    return fail("Displaced batches currently require opaque StandardPBR materials");
                const auto& p = profiles[key.second];
                bool decoded=false,proxy=false;
                if((c.m_useLevelFourWallMaterial||c.m_useLevelFourShared||c.m_useLevelFiveProxy) && p.m_neuralModel==4)
                {
                    const char* names[]={"baseColor.textureMap","normal.textureMap","roughness.textureMap"};
                    const char* files[]={"basecolor.png","normal.png","roughness.png"};
                    const auto layout=binding.m_material->GetMaterialPropertiesLayout();const auto& values=binding.m_material->GetPropertyValues();
                    decoded=true;
                    for(size_t channel=0;channel<3;++channel)
                    {
                        const auto index=layout->FindPropertyIndex(AZ::Name(names[channel]));AZ::Data::AssetId expected;
                        const auto path=AZStd::string::format("assets/silpomplanarab/irishfieldstonetowerwelded/wall/%s.streamingimage",files[channel]);
                        AZ::Data::AssetCatalogRequestBus::BroadcastResult(expected,&AZ::Data::AssetCatalogRequests::GetAssetIdByPath,path.c_str(),AZ::Data::AssetType::CreateNull(),false);
                        decoded=decoded && index.IsValid() && values[index.GetIndex()].Is<AZ::Data::Asset<AZ::RPI::ImageAsset>>()
                            && values[index.GetIndex()].GetValue<AZ::Data::Asset<AZ::RPI::ImageAsset>>().GetId()==expected && expected.IsValid();
                    }
                    if(decoded)
                    {
                        for(size_t i=0;i<values.size();++i)
                        {
                            const auto name=layout->GetPropertyDescriptor(AZ::RPI::MaterialPropertyIndex(i))->GetName();
                            if(name==AZ::Name("emissive.textureMap"))
                            {
                                const auto enabled=layout->FindPropertyIndex(AZ::Name("emissive.enable"));
                                const auto texture=layout->FindPropertyIndex(AZ::Name("emissive.useTexture"));
                                if((enabled.IsValid()&&values[enabled.GetIndex()].Is<bool>()&&!values[enabled.GetIndex()].GetValue<bool>())
                                    ||(texture.IsValid()&&values[texture.GetIndex()].Is<bool>()&&!values[texture.GetIndex()].GetValue<bool>()))continue;
                            }
                            if(values[i].Is<AZ::Data::Asset<AZ::RPI::ImageAsset>>() && values[i].GetValue<AZ::Data::Asset<AZ::RPI::ImageAsset>>().GetId().IsValid()
                                && name!=AZ::Name(names[0])&&name!=AZ::Name(names[1])&&name!=AZ::Name(names[2])&&name!=AZ::Name("parallax.textureMap"))
                                return fail("Level 4A wall decoder does not support additional textured material channels");
                        }
                        proxy=c.m_useLevelFiveProxy;
                        decoded=c.m_useLevelFourWallMaterial||c.m_useLevelFourShared;
                        if(decoded){++decodedBatches;++candidate->learnedMaterialBatches;}
                    }
                }
                if (!std::isfinite(p.m_scale) || !std::isfinite(p.m_reference) || p.m_reference < 0 || p.m_reference > 1
                    || !p.m_tiling.IsFinite() || !p.m_offset.IsFinite() || p.m_addressMode > 1
                    || std::abs(p.m_tiling.GetX()) < 1e-8f || std::abs(p.m_tiling.GetY()) < 1e-8f)
                    return fail("Invalid displacement profile settings");
                const float reliefScale = p.m_scale * depthMultiplier;
                const auto& desc = p.m_height->GetImageDescriptor();
                if (neuralVisibilityOnly)
                {
                    if (p.m_neuralModel<4 || p.m_neuralModel>7 || reliefScale<0)
                        return fail("Neural-only visibility has no model for this profile; assign an open-shaft model 4-7 with nonnegative relief");
                    constexpr const char* images[] = {
                        "assets/silpomplanarab/irishfieldstonetowerwelded/wall/height.png.streamingimage",
                        "assets/silpomplanarab/irishfieldstonetoweropenshaft/sculpted-stair-silpom/height.png.streamingimage",
                        "assets/silpomplanarab/irishfieldstonetoweropenshaft/welded-doorway/trim-blocks/height.png.streamingimage",
                        "assets/silpomplanarab/irishfieldstonetoweropenshaft/welded-doorway/reveal/height.png.streamingimage"};
                    AZ::Data::AssetId expected;
                    AZ::Data::AssetCatalogRequestBus::BroadcastResult(expected,&AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
                        images[p.m_neuralModel-4],azrtti_typeid<AZ::RPI::StreamingImageAsset>(),false);
                    if (!expected.IsValid() || expected!=p.m_height.GetId())
                        return fail("Neural-only visibility requires its trained height asset; retrain for a different material");
                }
                if (requiresTrainedHits)
                {
                    struct TrainedProfile { float scale, reference; AZ::u32 address, width, height, profileId; };
                    constexpr TrainedProfile trained[] = {
                        {.10f, .05f, 0, 6656, 6144, 1},
                        {.12f, 32768.f/65535.f, 1, 2176, 1440, 2},
                        {.045f, 32768.f/65535.f, 1, 1728, 1728, 3},
                        {.10f, .05f, 0, 6656, 6144, 1},
                        {.19f, 32768.f/65535.f, 1, 4096, 4096, 2},
                        {.052f, 32768.f/65535.f, 1, 1728, 1728, 3},
                        {.10f, .05f, 1, 512, 4096, 4}
                    };
                    if (p.m_neuralModel < 1 || p.m_neuralModel > AZ_ARRAY_SIZE(trained)
                        || p.m_neuralCache.GetId().IsValid())
                        return fail("Assign each displaced profile its matching trained model and no legacy cache");
                    const auto& model = trained[p.m_neuralModel - 1];
                    if (model.profileId != key.second || std::abs(p.m_scale - model.scale) > 1e-6f
                        || std::abs(p.m_reference - model.reference) > 1e-6f
                        || p.m_addressMode != model.address || desc.m_size.m_width != model.width || desc.m_size.m_height != model.height
                        || p.m_tiling != AZ::Vector2(1) || p.m_offset != AZ::Vector2(0))
                        return fail("Trained model does not match the authored displacement profile");
                }
                if (desc.m_mipLevels != 1 || desc.m_arraySize != 1 || desc.m_size.m_depth != 1
                    || (desc.m_format != AZ::RHI::Format::R32_FLOAT && desc.m_format != AZ::RHI::Format::R16_UNORM && desc.m_format != AZ::RHI::Format::R8_UNORM))
                    return fail("Height must be normalized linear, single-mip R32_FLOAT/R16_UNORM/R8_UNORM");
                const auto pixels = p.m_height->GetSubImageData(0, 0);
                const size_t stride = desc.m_format == AZ::RHI::Format::R32_FLOAT ? 4 : desc.m_format == AZ::RHI::Format::R16_UNORM ? 2 : 1;
                if (pixels.empty() || pixels.size() != size_t(desc.m_size.m_width)*desc.m_size.m_height*stride)
                    return fail("Height pixels unavailable");
                if (stride == 4) for (size_t i=0; i<pixels.size(); i+=4)
                {
                    float value; memcpy(&value,pixels.data()+i,4);
                    if (!std::isfinite(value) || value<0 || value>1) return fail("Height pixels must be finite and normalized");
                }
                // Nine float4 records per triangle. The base remains planar;
                // directions are generated from connected tagged face fans.
                AZStd::vector<float> packed;
                for (auto index : faces)
                {
                    const auto& corners = asset.m_faces[index].m_corners;
                    const auto a=corners[0].m_position*scale, b=corners[1].m_position*scale, d=corners[2].m_position*scale;
                    const auto e1=b-a, e2=d-a, cross=e1.Cross(e2);
                    const float areaSquared=cross.GetLengthSq();
                    const auto uv0=corners[0].m_uv, uv1=corners[1].m_uv, uv2=corners[2].m_uv;
                    const auto t1=uv1-uv0, t2=uv2-uv0;
                    const float det=t1.GetX()*t2.GetY()-t1.GetY()*t2.GetX();
                    if (!std::isfinite(areaSquared) || areaSquared<1e-16f || std::abs(det)<1e-10f)
                        return fail("Degenerate triangle or UV mapping: "+asset.m_faces[index].m_id);
                    const auto normal=cross/std::sqrt(areaSquared);
                    const auto du=(e1*t2.GetY()-e2*t1.GetY())/det;
                    const auto dv=(e2*t1.GetX()-e1*t2.GetX())/det;
                    auto append=[&](const AZ::Vector3& v) { packed.insert(packed.end(),{v.GetX(),v.GetY(),v.GetZ(),0}); };
                    append(a); append(du); append(dv); append(normal);
                    packed.insert(packed.end(),{uv0.GetX(),uv0.GetY(),uv1.GetX(),uv1.GetY(),uv2.GetX(),uv2.GetY(),0,0});
                    for (const auto& corner : corners) append(corner.m_direction);
                    ++candidate->faces;
                    const float localFace=float(packed.size()/36-1);
                    const AZ::u32 base=AZ::u32(vertices.size());
                    for (const auto& xy : {AZ::Vector2(-1,-1),AZ::Vector2(1,-1),AZ::Vector2(-1,1),AZ::Vector2(1,1)})
                        vertices.push_back({AZ::Vector3(xy.GetX(),xy.GetY(),localFace),AZ::Vector3::CreateAxisZ(),(xy+AZ::Vector2(1))*.5f});
                    indices.insert(indices.end(),{base,base+1,base+2,base+2,base+1,base+3});
                    for (const auto& corner : corners)
                    {
                        const auto point=corner.m_position*scale;
                        bounds.AddPoint(point+corner.m_direction*(-reliefScale*p.m_reference));
                        bounds.AddPoint(point+corner.m_direction*(reliefScale*(1-p.m_reference)));
                    }
                }
                candidate->bytes+=packed.size()*sizeof(float);
                auto data=Buffer(packed.data(),packed.size()/4,16,true);
                auto buffer=AZ::RPI::Buffer::FindOrCreate(data);
                if (!buffer) return fail("Cannot allocate planar face descriptors");
                buffer->WaitForUpload();
                const auto bindless=buffer->GetBufferView()->GetBindlessReadIndex();
                if(bindless.size()!=1 || bindless.begin()->second==AZ::u32(-1)) return fail("One bindless-capable GPU is required");
                auto [heightEntry, newHeight] = heightImages.try_emplace(p.m_height.GetId());
                const bool convertHeight = c.m_useR16HeightTextures && desc.m_format == AZ::RHI::Format::R32_FLOAT;
                if (newHeight)
                {
                    heightEntry->second = convertHeight ? MakeR16Height(p.m_height, pixels)
                        : AZ::RPI::StreamingImage::FindOrCreate(p.m_height);
                    candidate->sourceHeightBytes += pixels.size();
                    candidate->heightBytes += convertHeight ? pixels.size() / 2 : pixels.size();
                }
                Image height = heightEntry->second;
                if (!height) return fail("Cannot create height texture");
                material=Snapshot(m_template,binding.m_material.Get(), p.m_height.GetId(), convertHeight ? height : Image{},decoded,proxy);
                if(!material) return fail("Cannot create planar face material");
                if(proxy)
                {
                    Generation::ProxyBatch batch;batch.flags.resize(faces.size(),0u);
                    batch.heightLow=-p.m_scale*depthMultiplier*p.m_reference;
                    batch.heightHigh=p.m_scale*depthMultiplier*(1-p.m_reference);
                    AZStd::map<AZ::u32,AZ::u32> local;
                    for(AZ::u32 i=0;i<faces.size();++i)local[faces[i]]=i;
                    AZStd::set<AZ::u32> paired;
                    auto normal=[&](const MeshFace& f){return (f.m_corners[1].m_position-f.m_corners[0].m_position).Cross(f.m_corners[2].m_position-f.m_corners[0].m_position).GetNormalizedSafe();};
                    for(AZ::u32 i=0;i<faces.size();++i)
                    {
                        if(paired.contains(i))continue;
                        Generation::ProxyPatch patch;patch.localFaces.push_back(i);patch.sourceFaces.push_back(faces[i]);paired.insert(i);
                        const auto& face=asset.m_faces[faces[i]];
                        float longest=0;AZ::u32 partner=AZ::u32(-1);
                        for(size_t edge=0;edge<face.m_neighbors.size();++edge)
                        {
                            const auto n=face.m_neighbors[edge];if(n<0||!local.contains(AZ::u32(n)))continue;
                            const auto j=local[AZ::u32(n)];
                            if(paired.contains(j)||normal(face).Dot(normal(asset.m_faces[n]))<.99999f)continue;
                            const float length=(face.m_corners[(edge+1)%3].m_position-face.m_corners[edge%3].m_position).GetLengthSq();
                            if(length>longest){longest=length;partner=j;}
                        }
                        if(partner!=AZ::u32(-1)){patch.localFaces.push_back(partner);patch.sourceFaces.push_back(faces[partner]);paired.insert(partner);}
                        for(auto index:patch.sourceFaces)
                        {
                            const auto& f=asset.m_faces[index];if(f.m_neighbors.size()!=3)patch.protectedBoundary=true;
                            for(auto neighbor:f.m_neighbors)
                                if(neighbor<0||AZ::u32(neighbor)>=asset.m_faces.size()||!local.contains(AZ::u32(neighbor)))patch.protectedBoundary=true;
                        }
                        batch.patches.push_back(AZStd::move(patch));
                    }
                    batch.buffer=AZ::RPI::Buffer::FindOrCreate(Buffer(batch.flags.data(),batch.flags.size(),4,true,true));
                    Image first=MakeLearnedMaterialImage(*m_appearance,0),second=MakeLearnedMaterialImage(*m_appearance,1);
                    auto weights=AZ::RPI::Buffer::FindOrCreate(Buffer(m_appearance->Weights(),m_appearance->Header().weights,4,true));
                    if(!batch.buffer||!first||!second||!weights)return fail("Cannot allocate Level 5 appearance resources");
                    batch.buffer->WaitForUpload();weights->WaitForUpload();
                    const auto bi=batch.buffer->GetBufferView()->GetBindlessReadIndex(),wi=weights->GetBufferView()->GetBindlessReadIndex();
                    if(bi.size()!=1||wi.size()!=1||bi.begin()->second==AZ::u32(-1)||wi.begin()->second==AZ::u32(-1))return fail("Level 5 requires one bindless GPU");
                    if(!Set(material,"surface.proxyLodEnabled",true)||!Set(material,"surface.proxyDepthEnabled",c.m_useLevelFiveDepth)||!Set(material,"surface.proxyLodBuffer",bi.begin()->second)
                        ||!Set(material,"surface.appearanceFeatures0",first)||!Set(material,"surface.appearanceFeatures1",second)
                        ||!Set(material,"surface.appearanceDecoderBuffer",wi.begin()->second)||!Set(material,"surface.appearanceFeatureMipCount",m_appearance->Header().mips))
                        return fail("Level 5 appearance material contract mismatch");
                    batch.material=material;batch.bufferIndex=bi.begin()->second;
                    candidate->buffers.push_back(batch.buffer);candidate->buffers.push_back(weights);
                    candidate->heights.push_back(first);candidate->heights.push_back(second);
                    candidate->proxyBatches.push_back(AZStd::move(batch));
                }
                if(decoded)
                {
                    Image first=MakeLearnedMaterialImage(*m_learnedMaterial,0),second=MakeLearnedMaterialImage(*m_learnedMaterial,1);
                    auto weights=AZ::RPI::Buffer::FindOrCreate(Buffer(m_learnedMaterial->Weights(),m_learnedMaterial->Header().weights,4,true));
                    if(!first||!second||!weights)return fail("Cannot allocate Level 4 material resources");
                    weights->WaitForUpload();const auto weightIndices=weights->GetBufferView()->GetBindlessReadIndex();
                    if(weightIndices.size()!=1||weightIndices.begin()->second==AZ::u32(-1))return fail("Level 4 decoder requires one bindless GPU");
                    auto useMap=[&](const char* name){const auto idx=source->FindPropertyIndex(AZ::Name(name));return idx.IsValid()&&source->GetPropertyValue(idx).Is<bool>()&&source->GetPropertyValue(idx).GetValue<bool>();};
                    if(!Set(material,"surface.learnedMaterial",true)
                        ||!Set(material,"surface.sharedVisibility",c.m_useLevelFourShared)
                        ||!Set(material,"surface.materialUseBaseColor",useMap("baseColor.useTexture"))
                        ||!Set(material,"surface.materialUseNormal",useMap("normal.useTexture"))
                        ||!Set(material,"surface.materialUseRoughness",useMap("roughness.useTexture"))
                        ||!Set(material,"surface.materialFeatures0",first)
                        ||!Set(material,"surface.materialFeatures1",second)||!Set(material,"surface.materialDecoderBuffer",weightIndices.begin()->second)
                        ||!Set(material,"surface.materialFeatureMipCount",m_learnedMaterial->Header().mips))return fail("Level 4 material contract mismatch");
                    candidate->heights.push_back(first);candidate->heights.push_back(second);candidate->buffers.push_back(weights);
                    if(c.m_useLevelFourShared)
                    {
                        Image third=MakeLearnedMaterialImage(*m_learnedMaterial,2);
                        if(!third||!Set(material,"surface.materialFeatures2",third))return fail("Cannot bind shared visibility features");
                        candidate->heights.push_back(third);
                    }
                }
                const bool legacyNeural = reuseDepthHits && c.m_useNeuralSeed && p.m_addressMode == 1
                    && std::abs(p.m_scale - .02f) < 1e-6f && std::abs(p.m_reference - .05f) < 1e-6f;
                const bool neuralSeed = requiresTrainedHits || legacyNeural;
                Image neuralCache;
                if (legacyNeural && p.m_neuralCache.GetId().IsValid())
                {
                    const auto& cacheDesc = p.m_neuralCache->GetImageDescriptor();
                    if (cacheDesc.m_size.m_width != 3072 || cacheDesc.m_size.m_height != 1536
                        || cacheDesc.m_size.m_depth != 1 || cacheDesc.m_arraySize != 1 || cacheDesc.m_mipLevels != 1
                        || cacheDesc.m_format != AZ::RHI::Format::R16G16B16A16_FLOAT
                        || desc.m_size.m_width != 6656 || desc.m_size.m_height != 6144
                        || p.m_tiling != AZ::Vector2(1) || p.m_offset != AZ::Vector2(0))
                    {
                        return fail("Tower neural cache requires its authored height atlas and a 3072 x 1536 single-mip RGBA16F cache");
                    }
                    neuralCache = AZ::RPI::StreamingImage::FindOrCreate(p.m_neuralCache);
                    if (!neuralCache) return fail("Cannot create neural hit cache image");
                }
                Image minMax;
                if (reuseDepthHits && !neuralSeed && c.m_useDepthHeightBounds && c.m_maxCells >= 4096)
                {
                    auto [atlasEntry, inserted] = depthHeightBounds.try_emplace(key.second);
                    if (inserted)
                    {
                        atlasEntry->second = MakeDepthHeightBounds(desc, pixels, p.m_addressMode != 0, convertHeight);
                    }
                    minMax = atlasEntry->second;
                }
                const bool valid=Set(material,"mesh.bufferIndex",bindless.begin()->second)
                    && Set(material,"surface.heightMap",height) && Set(material,"surface.scale",reliefScale)
                    && Set(material,"surface.reference",p.m_reference) && Set(material,"surface.tileU",p.m_tiling.GetX())
                    && Set(material,"surface.tileV",p.m_tiling.GetY()) && Set(material,"surface.offsetU",p.m_offset.GetX())
                    && Set(material,"surface.offsetV",p.m_offset.GetY()) && Set(material,"surface.addressMode",p.m_addressMode)
                    && Set(material,"surface.maxCells",c.m_maxCells) && Set(material,"surface.useHierarchy",bool(minMax))
                    && Set(material,"surface.useNeuralSeed",neuralSeed)
                    && Set(material,"surface.neuralModel",p.m_neuralModel)
                    && Set(material,"surface.validationIterations",c.m_validationIterations)
                    && Set(material,"surface.validationGather",c.m_useValidationGather)
                    && Set(material,"surface.levelTwoPredictor",c.m_useLevelTwoPredictor)
                    && Set(material,"surface.validationRetryCap",c.m_validationRetryCap)
                    && Set(material,"surface.validationToleranceMultiplier",c.m_validationToleranceMultiplier)
                    && Set(material,"surface.reliefLodPixelBudget",c.m_reliefLodPixelBudget)
                    && Set(material,"surface.maxReliefDistance",c.m_maxReliefDistance)
                    && Set(material,"surface.validationDistanceStart",c.m_validationDistanceStart)
                    && Set(material,"surface.validationDistanceEnd",c.m_validationDistanceEnd)
                    && Set(material,"surface.distantValidationIterations",c.m_distantValidationIterations)
                    && Set(material,"surface.distantValidationRetryCap",c.m_distantValidationRetryCap)
                    && Set(material,"surface.distantValidationToleranceMultiplier",c.m_distantValidationToleranceMultiplier)
                    && Set(material,"surface.spiralLightShadowSteps",c.m_spiralLightShadowSteps)
                    && Set(material,"surface.localShadowMethod",static_cast<AZ::u32>(c.m_localShadowMethod))
                    // New full-surface heads are trained only for the canonical
                    // wall. Other material profiles retain their current route.
                    && Set(material,"surface.lightingEstimator",p.m_neuralModel==4u
                        ?static_cast<AZ::u32>(c.m_lightingEstimator):0u)
                    && Set(material,"surface.diffuseAccessibility",p.m_neuralModel==4u && c.m_diffuseAccessibility)
                    && Set(material,"surface.neuralDirectionalVisibility",c.m_neuralDirectionalVisibility)
                    && Set(material,"surface.proxyDirectionalResponse",c.m_proxyDirectionalResponse)
                    && Set(material,"surface.neuralShadowConfidence",c.m_neuralShadowConfidence)
                    && Set(material,"surface.useHeightDerivedNormals",c.m_useHeightDerivedNormals)
                    && Set(material,"surface.useNeuralCache",bool(neuralCache))
                    && (!neuralCache || Set(material,"surface.neuralCache",neuralCache))
                    && (!minMax || Set(material,"surface.heightBounds",minMax))
                    && Set(material,"surface.reliefShadowSteps",c.m_reliefShadowSteps);
                if(!valid) return fail("Planar face material contract mismatch");
                if (hasSpiralLight && (c.m_spiralLightMode == SpiralLightMode::AllSilPomSurfaces || binding.m_receiveSpiralLight))
                {
                    if (!Set(material, "spiralLight.path", spiralPath)
                        || !Set(material, "spiralLight.brightness", c.m_spiralLightBrightness)
                        || !Set(material, "spiralLight.peakLux", c.m_spiralLightPeakLux)
                        || !Set(material, "spiralLight.end", spiralEnd)
                        || (windowCount > 0 && (!Set(material, "spiralLight.windowCount", windowCount)
                            || !Set(material, "spiralLight.windowBuffer", windowBufferIndex))))
                        return fail("Spiral light material contract mismatch");
                    ++candidate->spiralLightBatches;
                }
                if (reuseDepthHits)
                {
                    const auto base=AllocateHitFaceBase(packed.size()/36);
                    if(!Set(material,"mesh.hitFaceBase",base))return fail("SilPOM hit material contract mismatch");
                    if(proxy)candidate->proxyBatches.back().hitFaceBase=base;
                }
                candidate->buffers.push_back(buffer); candidate->heights.push_back(height);
                candidate->neuralSeed |= neuralSeed;
                if (p.m_neuralModel != 0) ++candidate->trainedModelBatches;
                if (neuralCache)
                {
                    candidate->heights.push_back(neuralCache);
                    candidate->neuralCache = true;
                }
                if (minMax)
                {
                    candidate->heights.push_back(minMax);
                    candidate->depthHeightBounds = true;
                }
                bounds.Expand(AZ::Vector3(1e-5f));
            }
            if (binding.m_disableEnvironmentLighting && !Set(material, "general.enableIBL", false))
                return fail("Material cannot disable environment lighting");
            material->Compile();
            auto model = Model(vertices, indices, bounds); if (!model) return fail("Cannot create generation geometry");
            AZ::Render::MeshHandleDescriptor descriptor(model, material);
            descriptor.m_entityId = m_entity; descriptor.m_isRayTracingEnabled = false; descriptor.m_excludeFromReflectionCubeMaps = true;
            auto handle = m_processor->AcquireMesh(descriptor); if (!handle.IsValid()) return fail("Mesh acquisition failed");
            m_processor->SetLightingChannelMask(handle, binding.m_lightingChannelMask);
            // Atom does not initialize invisible meshes. Allow initialization,
            // but disable every new draw item in its update notification before
            // the subsequent culling/submission phase. Never draw a partial
            // generation while waiting for the other batches to become ready.
            m_processor->SetTransform(handle, rigid); m_processor->SetLocalAabb(handle, bounds);
            const size_t batchIndex = candidate->handles.size();
            candidate->models.push_back(model); candidate->materials.push_back(material); candidate->handles.push_back(AZStd::move(handle));
            const auto motionTag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("motion"));
            auto handler = AZStd::make_unique<Generation::PacketHandler>(
                [generation = candidate.get(), batchIndex, processor = m_processor, motionTag](
                    const AZ::Render::ModelDataInstanceInterface&, AZ::u32, AZ::u32, const AZ::RPI::MeshDrawPacket& packet)
                {
                    if (const auto* draw = packet.GetRHIDrawPacket())
                        for (size_t i = 0; i < draw->GetDrawItemCount(); ++i)
                        {
                            const auto tag = draw->GetDrawListTag(i);
                            processor->SetDrawItemEnabled(generation->handles[batchIndex], tag,
                                generation->published.load() && tag != motionTag);
                        }
                });
            candidate->handles.back()->ConnectMeshDrawPacketUpdatedHandler(*handler);
            candidate->packetHandlers.push_back(AZStd::move(handler));
        }
    }
    if (requiresTrainedHits && !AZ::RPI::SceneNotificationBus::Handler::BusIsConnected())
    {
        AZ::RPI::SceneNotificationBus::Handler::BusConnect(scene->GetId());
    }
    if((c.m_useLevelFourWallMaterial||c.m_useLevelFourShared) && !decodedBatches)return fail("Level 4 has no matching fieldstone-wall material; no representation was changed");
    if(c.m_useLevelFiveProxy&&candidate->proxyBatches.empty())return fail("Level 5A has no matching canonical wall batches");
    m_pending = AZStd::move(candidate); m_status = "Preparing complete mesh generation"; return false;
}
AZ::Vector3 MeshController::GetProxyLodState() const
{
    return m_active?m_active->proxyState:AZ::Vector3::CreateZero();
}
AZStd::vector<AZ::u32> MeshController::GetProxyFaceOwners() const
{
    AZStd::vector<AZ::u32> owners;if(m_active)for(const auto& batch:m_active->proxyBatches)
        for(size_t i=0;i<batch.flags.size();++i)if(batch.flags[i])owners.push_back(batch.hitFaceBase+aznumeric_cast<AZ::u32>(i));
    return owners;
}
void MeshController::OnBeginPrepareRender(){UpdateProxyLod();}
void MeshController::UpdateProxyLod()
{
    if(!m_active||m_active->proxyBatches.empty()||!m_configuration.m_surface.IsReady())return;
    auto* scene=AZ::RPI::Scene::GetSceneForEntityId(m_entity);
    auto* manager=AZ::Interface<AZ::RPI::ViewportContextRequestsInterface>::Get();
    auto viewport=manager&&scene?manager->GetViewportContextByScene(scene):AZ::RPI::ViewportContextPtr{};
    auto view=viewport?viewport->GetDefaultView():AZ::RPI::ViewPtr{};
    auto size=viewport?viewport->GetViewportSize():AzFramework::WindowSize{};
    size_t mainViews=0;if(scene)for(const auto& pipeline:scene->GetRenderPipelines())
        if(pipeline->GetDescriptor().m_materialPipelineTag=="MainPipeline")
        {
            ++mainViews;
            if(auto pass=pipeline->FindFirstPass(AZ::Name("SilPOMHitDepth")))
                if(auto* binding=pass->FindAttachmentBinding(AZ::Name("Hit"));binding&&binding->GetAttachment())
                {
                    const auto actual=binding->GetAttachment()->m_descriptor.m_image.m_size;
                    // Also cover custom/upscaled render targets. Taking the
                    // larger size is conservative while resize is in flight.
                    size.m_width=std::max(size.m_width,actual.m_width);size.m_height=std::max(size.m_height,actual.m_height);
                }
        }
    const bool valid=view&&size.m_width&&size.m_height&&mainViews==1;
    AZ::Transform rigid=m_world;rigid.SetUniformScale(1);
    const auto camera=view?view->GetViewToWorldMatrix().GetTranslation():AZ::Vector3::CreateZero();
    const auto clip=view?view->GetWorldToClipMatrix():AZ::Matrix4x4::CreateIdentity();
    const auto& source=m_configuration.m_surface->m_faces;
    ++m_active->proxyFrame;
    size_t selected=0,eligible=0;double maxSelected=0;
    for(auto& batch:m_active->proxyBatches)
    {
        batch.retired.erase(AZStd::remove_if(batch.retired.begin(),batch.retired.end(),[&](const auto& value){return value.first<=m_active->proxyFrame;}),batch.retired.end());
        const auto oldFlags=batch.flags;
        bool changed=false;
        for(auto& patch:batch.patches)
        {
            double error=0,facing=1;
            eligible+=patch.localFaces.size();
            for(auto index:patch.sourceFaces)
            {
                const auto& face=source[index];
                std::array<ReliefLod::Clip,3> base;std::array<ReliefLod::Clip,6> extrema;
                const auto normal=rigid.TransformVector((face.m_corners[1].m_position-face.m_corners[0].m_position)
                    .Cross(face.m_corners[2].m_position-face.m_corners[0].m_position).GetNormalizedSafe());
                auto pack=[](const AZ::Vector4& v){return ReliefLod::Clip{v.GetX(),v.GetY(),v.GetZ(),v.GetW()};};
                for(size_t i=0;i<3;++i)
                {
                    const auto& corner=face.m_corners[i];const auto p=m_world.TransformPoint(corner.m_position);
                    const auto D=rigid.TransformVector(corner.m_direction);
                    base[i]=pack(clip*AZ::Vector4(p,1));
                    extrema[2*i]=pack(clip*AZ::Vector4(p+D*batch.heightLow,1));
                    extrema[2*i+1]=pack(clip*AZ::Vector4(p+D*batch.heightHigh,1));
                    const auto toward=(camera-p).GetNormalizedSafe();
                    facing=std::min(facing,double(std::min(normal.Dot(toward),D.GetNormalizedSafe().Dot(toward))));
                }
                error=std::max(error,ReliefLod::PixelError(base,extrema,size.m_width,size.m_height));
            }
            patch.far=valid&&ReliefLod::Select(patch.far,patch.protectedBoundary,facing,error,m_configuration.m_levelFivePixelBudget);
            for(auto local:patch.localFaces){const auto flag=patch.far?1u:0u;changed|=batch.flags[local]!=flag;batch.flags[local]=flag;}
            if(patch.far){selected+=patch.localFaces.size();maxSelected=std::max(maxSelected,error);}
        }
        if(changed)
        {
            // DX12 orphaning reallocates a bindless descriptor. Publish a new
            // immutable snapshot and its actual index together instead.
            auto next=AZ::RPI::Buffer::FindOrCreate(Buffer(batch.flags.data(),batch.flags.size(),4,true,true));
            if(next)next->WaitForUpload();
            const auto indices=next?next->GetBufferView()->GetBindlessReadIndex():AZStd::unordered_map<int,AZ::u32>{};
            if(indices.size()!=1||indices.begin()->second==AZ::u32(-1)
                ||!Set(batch.material,"surface.proxyLodBuffer",indices.begin()->second)||!batch.material->Compile())
            {
                batch.flags=oldFlags;for(auto& patch:batch.patches)patch.far=batch.flags[patch.localFaces.front()]!=0;
                Set(batch.material,"surface.proxyLodBuffer",batch.bufferIndex);batch.material->Compile();
                m_status="Level 5A selection snapshot upload failed";return;
            }
            if(auto srg=batch.material->GetShaderResourceGroup())srg->Compile();
            // The backend permits at most three in-flight frames. Keep eight
            // render preparations, in addition to RHI's deferred destruction.
            batch.retired.push_back({m_active->proxyFrame+8,batch.buffer});
            batch.buffer=next;batch.bufferIndex=indices.begin()->second;
        }
    }
    m_active->proxyState=AZ::Vector3(float(selected),float(eligible),float(maxSelected));
}
void MeshController::OnTick(float, AZ::ScriptTimePoint)
{
    AntiAliasingState().Update(m_entity,m_configuration.m_antiAliasing);
    const unsigned policy=UpdateHitSamplePolicy(AZ::RPI::Scene::GetSceneForEntityId(m_entity));
    if(m_hitSamplePolicy!=policy){m_hitSamplePolicy=policy;m_checkHitPasses=true;}
    if (m_dirty) { Release(m_pending); m_dirty = Prepare(); }
    if (m_checkHitPasses)
    {
        // Play can rebuild the pass tree after the mesh generation is ready.
        // Restore its passes on the tick, outside the pipeline notification.
        m_checkHitPasses = false;
        auto* scene = AZ::RPI::Scene::GetSceneForEntityId(m_entity);
        if (!scene || !InstallHitPasses(*scene, true))
        {
            m_checkHitPasses = true;
        }
    }
    if (m_pending)
    {
        bool ready = true;
        for (const auto& material : m_pending->materials)
        {
            if (material->NeedsCompile()) material->Compile();
            ready = ready && !material->NeedsCompile();
        }
        for (const auto& handle : m_pending->handles)
        {
            ready = ready && bool(m_processor->GetModel(handle));
            const auto& lods = m_processor->GetDrawPackets(handle);
            ready = ready && !lods.empty() && !lods.front().empty();
            for (const auto& lod : lods) for (const auto& packet : lod)
                ready = ready && packet.GetRHIDrawPacket() && packet.GetRHIDrawPacket()->GetDrawItemCount() != 0;
        }
        if (ready)
        {
            // All handles reference private immutable model/buffer assets. Stock
            // source-model reloads cannot replace just the ordinary half.
            if (m_active) for (const auto& handle : m_active->handles) m_processor->SetVisible(handle, false);
            m_pending->published.store(true);
            const auto motionTag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("motion"));
            for (const auto& handle : m_pending->handles)
                for (const auto& lod : m_processor->GetDrawPackets(handle)) for (const auto& packet : lod)
                    for (size_t i = 0; i < packet.GetRHIDrawPacket()->GetDrawItemCount(); ++i)
                    {
                        const auto tag = packet.GetRHIDrawPacket()->GetDrawListTag(i);
                        m_processor->SetDrawItemEnabled(handle, tag, tag != motionTag);
                    }
            Release(m_active); m_active = AZStd::move(m_pending);
            if (!m_active->trainedModelBatches)
            {
                AZ::RPI::SceneNotificationBus::Handler::BusDisconnect();
                m_checkHitPasses = false;
            }
            m_status = AZStd::string::format("Ready: %zu batches, %zu planar tagged triangles, %zu descriptor bytes", m_active->handles.size(), m_active->faces, m_active->bytes);
            m_status += "; active generation " + m_active->id;
            m_status += AZStd::string::format("; %s, height payload %.2f MiB (source %.2f MiB); validation %s",
                m_configuration.m_useR16HeightTextures ? "R16 UNORM heights" : "source height formats",
                double(m_active->heightBytes) / 1048576, double(m_active->sourceHeightBytes) / 1048576,
                m_configuration.m_useValidationGather ? "Gather" : "four loads");
            m_status += m_configuration.m_useLevelTwoPredictor ? "; Level 2 predictor" : "; Level 1 predictor";
            m_status += m_configuration.m_localShadowMethod==LocalShadowMethod::Baseline ? "; local shadows: baseline"
                :m_configuration.m_localShadowMethod==LocalShadowMethod::Gather ? "; local shadows: Gather"
                :m_configuration.m_localShadowMethod==LocalShadowMethod::NeuralOnly ? "; local shadows: neural only (no march)"
                :"; local shadows: hybrid neural + march";
            if(m_active->learnedMaterialBatches)m_status+=AZStd::string::format("; Level %s material on %zu batches",m_configuration.m_useLevelFourShared?"4B shared":"4A",m_active->learnedMaterialBatches);
            if(m_configuration.m_useLevelFiveDepth)m_status+="; Level 5B bounded proxy depth (authored coverage)";
            if(!m_active->proxyBatches.empty())m_status+=AZStd::string::format("; Level 5A patch selection on %zu wall batches",m_active->proxyBatches.size());
            if (m_active->depthHeightBounds)
            {
                m_status += "; depth height bounds";
            }
            if (m_active->trainedModelBatches)
            {
                const float pixelBudget = m_configuration.m_reliefLodPixelBudget > 0.0f ? m_configuration.m_reliefLodPixelBudget : 4.0f;
                m_status += AZStd::string::format("; %zu trained profile batches, smooth %g px winning-hit resolve",
                    m_active->trainedModelBatches, pixelBudget);
                if (m_configuration.m_maxReliefDistance > 0.0f)
                    m_status += AZStd::string::format("; relief cap %g m", m_configuration.m_maxReliefDistance);
            }
            else if (m_active->neuralSeed)
            {
                m_status += m_active->neuralCache ? "; baked neural hit cache" : "; neural depth seed";
            }
            if (m_configuration.m_spiralLightMode == SpiralLightMode::Off)
                m_status += "; spiral light off";
            else if (m_configuration.m_spiralLightBrightness == 0.0f)
                m_status += "; spiral light muted (brightness 0)";
            else
                m_status += AZStd::string::format("; spiral light on %zu relief batches", m_active->spiralLightBatches);
        }
    }
}
}

// SPDX-License-Identifier: MIT
#include <SilPOM/Mesh.h>
#include <SilPOM/CameraCollisionMath.h>
#include <SilPOM/DecalPaint.h>
#include <AzCore/Serialization/Json/JsonUtils.h>
#include <AzCore/Asset/AssetSerializer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <SilPOM/HeightPrecision.h>
#include <SilPOM/ReceiverPaintState.h>
#include <SilPOM/SurfacePaintImage.h>
#include <AzCore/Math/Aabb.h>
#include <SilPOM/AppearanceCodec.h>
#include <SilPOM/ReliefLod.h>
#include <SilPOM/DirectionalEstimatorContract.h>
#include <SilPOM/StrictNeuralPolicy.h>
#include <Atom/RPI.Reflect/Shader/ShaderOptionGroup.h>
#include "ResolvePass.h"
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
#include <Atom/RPI.Reflect/Pass/ComputePassData.h>
#include <Atom/RPI.Public/Buffer/BufferSystemInterface.h>
#include <Atom/RPI.Reflect/Pass/PassTemplate.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/RPI.Public/Image/ImageSystemInterface.h>
#include <Atom/RPI.Public/Image/StreamingImage.h>
#include <Atom/RPI.Public/Image/StreamingImagePool.h>
#include <Atom/RPI.Public/Image/AttachmentImage.h>
#include <Atom/RPI.Public/Image/AttachmentImagePool.h>
#include <Atom/RPI.Public/Pass/FullscreenTrianglePass.h>
#include <Atom/RPI.Reflect/Pass/FullscreenTrianglePassData.h>
#include <Atom/RPI.Reflect/Material/ShaderCollection.h>
#include <AzCore/Math/Matrix4x4.h>
#include <chrono>
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
#include <AzCore/Settings/SettingsRegistry.h>
#include <SilPOM/LearnedMaterialAsset.h>

namespace SilPOM
{
namespace
{
// Learned materials are project data. The gem never names a project's assets:
// each project registers its learned bundles under /SilPOM in its own .setreg
// files. Trained displacement profiles are .silpomprofile assets named by the
// surface (Blender export) or a profile binding, not registry entries.
constexpr const char* LearnedWallMaterialRegistryRoot = "/SilPOM/LearnedWallMaterial";
constexpr const char* LearnedBundlesRegistryRoot = "/SilPOM/LearnedBundles";

bool ReadRegistryString(const AZStd::string& path, AZStd::string& value)
{
    auto* registry = AZ::SettingsRegistry::Get();
    return registry && registry->Get(value, path) && !value.empty();
}

AZ::Data::AssetId FindProduct(const AZStd::string& path, const AZ::TypeId& type)
{
    AZ::Data::AssetId id;
    AZ::Data::AssetCatalogRequestBus::BroadcastResult(id, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
        path.c_str(), type, false);
    return id;
}

// Product path of a learned bundle (.silpommaterial) registered under
// /SilPOM/LearnedBundles/<key>: LevelFourMaterial, LevelFourShared,
// LevelFiveAppearance or LevelFiveDepth.
bool FindLearnedBundle(const char* key, AZ::Data::AssetId& id)
{
    AZStd::string path;
    if (!ReadRegistryString(AZStd::string::format("%s/%s", LearnedBundlesRegistryRoot, key), path)) return false;
    AZ::Data::AssetCatalogRequestBus::BroadcastResult(id, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
        path.c_str(), azrtti_typeid<LearnedMaterialAsset>(), false);
    return id.IsValid();
}

// Product paths of the base color, normal and roughness images the learned
// wall material was trained from, in that order.
bool FindLearnedWallMaterialImages(AZ::Data::AssetId (&ids)[3])
{
    const char* keys[] = {"BaseColor", "Normal", "Roughness"};
    for (size_t channel = 0; channel < 3; ++channel)
    {
        AZStd::string path;
        if (!ReadRegistryString(AZStd::string::format("%s/%s", LearnedWallMaterialRegistryRoot, keys[channel]), path)) return false;
        AZ::Data::AssetCatalogRequestBus::BroadcastResult(ids[channel], &AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
            path.c_str(), AZ::Data::AssetType::CreateNull(), false);
        if (!ids[channel].IsValid()) return false;
    }
    return true;
}

AZStd::atomic_uint64_t ReceiverSurfaceRevision{0};
AZ_CVAR(bool, r_silpomReuseDepthHits, false, nullptr, AZ::ConsoleFunctorFlags::Null,
    "Use the DX12 SilPOM hit buffer in MainPipeline (1x, 2x or 4x).");
AZ_CVAR(bool,r_silpomMsaa4,false,nullptr,AZ::ConsoleFunctorFlags::Null,
    "Opt into sample-frequency 4x SilPOM. Disabling restores the sample count active before enabling it. Models/settings are unchanged.");
AZ_CVAR(bool,r_silpomMsaaRawCoverage,true,nullptr,AZ::ConsoleFunctorFlags::Null,
    "Inspect unrepaired 4x sample coverage. Does not affect the approved 1x repair path.");
AZ_CVAR(bool,r_silpomMsaaInspect,false,nullptr,AZ::ConsoleFunctorFlags::Null,
    "Enable diagnostic raw/final hit, depth and shaded-color sample atlases. Adds GPU/readback work; normally off.");
AZ_CVAR(bool,r_silpomAaTrace,false,nullptr,AZ::ConsoleFunctorFlags::Null,
    "Trace AA ownership, sample-count changes and pipeline transitions. Diagnostic only; normally off.");
AZ_CVAR(AZ::u32,r_silpomAntiAliasing,0,nullptr,AZ::ConsoleFunctorFlags::Null,
    "Renderer-wide AA: 0=project default, 1=SMAA, 2=TAA, 3=4x MSAA, 4=2x MSAA. Applies to every scene.");

bool SupportsHitMsaa(unsigned samples)
{
    if(samples==1)return true;
    if(samples!=2&&samples!=4)return false;
#if defined(AZ_PLATFORM_WINDOWS)
    if(AZ::RHI::Factory::Get().GetName()!=AZ::Name("dx12"))return false;
    auto* rhi=AZ::RHI::RHISystemInterface::Get();
    if(!rhi||rhi->GetDeviceCount()==0)return false;
    // Query the actual rendering devices, including each active device.
    for(int index=0;index<rhi->GetDeviceCount();++index)
    {
        auto* native=AZ::DX12::GetDeviceNativeHandle(*rhi->GetDevice(index));
        if(!native)return false;
        const DXGI_FORMAT colors[]={DXGI_FORMAT_R32G32B32A32_UINT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM};
        for(auto format:colors)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
            constexpr UINT needed=D3D12_FORMAT_SUPPORT1_RENDER_TARGET|D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET
                |D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format,samples,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
            if(FAILED(native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))
                ||(support.Support1&needed)!=needed
                ||FAILED(native->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&quality,sizeof(quality)))
                ||quality.NumQualityLevels==0)return false;
        }
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS depth{DXGI_FORMAT_D32_FLOAT_S8X24_UINT,samples,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
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

// Observe every scene during the operation, including pipelines created after it
// starts. Idle/execute-once preview pipelines need their queued rebuild processed
// now; waiting for their next draw leaves old draw packets against new targets.
struct HitSampleTransition
    : AZ::RPI::SceneNotificationBus::Router
    , AZ::TickBus::Handler
{
    struct Pipeline
    {
        AZ::RPI::RenderPipelinePtr pipeline;
        bool enabled;
        AZ::u64 lookupTick=~AZ::u64(0);
    };
    AZStd::vector<Pipeline> paused;
    bool active=false;
    unsigned targetSamples=1;

    void Pause(AZ::RPI::RenderPipeline* pipeline)
    {
        if(!pipeline||!pipeline->GetScene())return;
        auto found=AZStd::find_if(paused.begin(),paused.end(),[&](const auto& p){return p.pipeline.get()==pipeline;});
        if(found==paused.end())
        {
            auto owned=pipeline->GetScene()->GetRenderPipeline(pipeline->GetId());
            if(!owned)return;
            paused.push_back({owned,pipeline->GetRootPass()->IsEnabled()});
            // Added pipelines may have missed SetApplicationMultisampleState.
            if(pipeline->GetRenderSettings().m_multisampleState.m_samples!=targetSamples)
            {
                pipeline->GetRenderSettings().m_multisampleState=AZ::RPI::RPISystemInterface::Get()->GetApplicationMultisampleState();
                pipeline->MarkPipelinePassChanges(AZ::RPI::PipelinePassChanges::MultisampleStateChanged);
            }
        }
        pipeline->GetRootPass()->SetEnabled(false);
    }
    void Begin(AZ::RPI::Scene* scene,unsigned samples)
    {
        targetSamples=samples;
        if(!active)
        {
            active=true;
            AZ::RPI::SceneNotificationBus::Router::BusRouterConnect();
            AZ::TickBus::Handler::BusConnect();
        }
        for(auto& p:paused)p.lookupTick=~AZ::u64(0);
        if(scene)for(const auto& p:scene->GetRenderPipelines())Pause(p.get());
        for(const auto& root:AZ::RPI::PassSystemInterface::Get()->GetRootPass()->GetChildren())
            if(auto* owner=root->GetScene())for(const auto& p:owner->GetRenderPipelines())Pause(p.get());
        for(const auto& p:paused)AZ::RPI::SceneRequestBus::Event(p.pipeline->GetScene()->GetId(),
            &AZ::RPI::SceneRequest::PipelineStateLookupNeedsRebuild);
    }
    void Resume()
    {
        if(!active)return;
        // UpdatePasses emits notifications. Copy the pointers so added/removed
        // pipelines cannot invalidate this traversal.
        AZStd::vector<AZ::RPI::RenderPipelinePtr> pipelines;
        for(const auto& p:paused)pipelines.push_back(p.pipeline);
        for(const auto& p:pipelines)if(p->GetScene())p->UpdatePasses();
        const auto tick=AZ::RPI::RPISystemInterface::Get()->GetCurrentTick();
        for(const auto& p:paused)
        {
            const auto state=p.pipeline->GetRootPass()->GetPassState();
            if(p.pipeline->GetPipelinePassChanges()!=AZ::RPI::PipelinePassChanges::NoPassChanges
                ||p.pipeline->GetRenderSettings().m_multisampleState.m_samples!=targetSamples
                ||(state!=AZ::RPI::PassState::Initialized&&state!=AZ::RPI::PassState::Idle)
                ||p.lookupTick==~AZ::u64(0)||tick<=p.lookupTick)return;
        }
        // The rebuilt scene lookup has crossed a simulation boundary, allowing
        // mesh feature processors to refresh their draw packets before drawing.
        for(const auto& p:paused)if(p.pipeline->GetScene()&&p.pipeline->GetRootPass()->GetParent())
            p.pipeline->GetRootPass()->SetEnabled(p.enabled);
        paused.clear();active=false;
        AZ::RPI::SceneNotificationBus::Router::BusRouterDisconnect();
        AZ::TickBus::Handler::BusDisconnect();
    }
    void OnTick(float,AZ::ScriptTimePoint) override { Resume(); }
    void OnPipelineStateLookupRebuilt() override
    {
        const auto* scene=AZ::RPI::SceneNotificationBus::GetCurrentBusId();
        if(scene)for(auto& p:paused)if(p.pipeline->GetScene()&&p.pipeline->GetScene()->GetId()==*scene)
            p.lookupTick=AZ::RPI::RPISystemInterface::Get()->GetCurrentTick();
    }
    void OnRenderPipelineChanged(AZ::RPI::RenderPipeline* pipeline,AZ::RPI::SceneNotification::RenderPipelineChangeType type) override
    {
        if(r_silpomAaTrace&&pipeline)AZ_Printf("SilPOM AA","pipeline=%s ptr=%p root=%p event=%u paused=%zu tick=%llu\n",
            pipeline->GetId().GetCStr(),pipeline,pipeline->GetRootPass().get(),unsigned(type),paused.size(),
            static_cast<unsigned long long>(AZ::RPI::RPISystemInterface::Get()->GetCurrentTick()));
        if(type==AZ::RPI::SceneNotification::RenderPipelineChangeType::Removed)
        {
            AZStd::erase_if(paused,[&](const auto& p){return p.pipeline.get()==pipeline;});return;
        }
        Pause(pipeline);
        for(auto& p:paused)if(p.pipeline.get()==pipeline)p.lookupTick=~AZ::u64(0);
        AZ::RPI::SceneRequestBus::Event(pipeline->GetScene()->GetId(),&AZ::RPI::SceneRequest::PipelineStateLookupNeedsRebuild);
    }
};
HitSampleTransition& SampleTransition()
{
    static HitSampleTransition transition;
    return transition;
}

// One-shot sample-count requests from the selector. Default meshes remain
// passive after restoration, so later native application changes still work.
unsigned& PendingHitSamples() { static unsigned samples=0;return samples; }

unsigned UpdateHitSamplePolicy(AZ::RPI::Scene* scene)
{
    static bool legacyRequested=false,raw=true,inspect=false;
    static unsigned legacyOriginalSamples=1,version=0;
    auto& transition=SampleTransition();
    auto* rpi=AZ::RPI::RPISystemInterface::Get();
    if(transition.active)
    {
        // Pass attachments update before MeshFeatureProcessor has rebuilt all
        // scene draw packets. Submit no scene draws across that transition.
        transition.Resume();
    }
    unsigned desired=rpi->GetApplicationMultisampleState().m_samples;
    if(PendingHitSamples())
    {
        desired=PendingHitSamples();PendingHitSamples()=0;
        legacyRequested=r_silpomMsaa4;
    }
    else if(legacyRequested!=r_silpomMsaa4)
    {
        if(r_silpomMsaa4)
        {
            legacyOriginalSamples=desired;
            if(SupportsHitMsaa(4))desired=4;
            else
            {
                AZ_Error("SilPOM",false,"4x MSAA is unsupported; retaining the current sample count.");
                r_silpomMsaa4=false;
            }
        }
        else desired=legacyOriginalSamples;
        legacyRequested=r_silpomMsaa4;
    }
    if(desired!=rpi->GetApplicationMultisampleState().m_samples)
    {
        if(r_silpomAaTrace)AZ_Printf("SilPOM AA","change samples=%u->%u tick=%llu\n",
            unsigned(rpi->GetApplicationMultisampleState().m_samples),desired,
            static_cast<unsigned long long>(rpi->GetCurrentTick()));
        transition.Begin(scene,desired);++version;
        const auto command=AZStd::string::format("r_multiSampleCount %u",desired);
        AZ::Interface<AZ::IConsole>::Get()->PerformCommand(command.c_str());
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
    unsigned originalSamples=1;
    bool baselineCaptured=false,handoff=false;
    AntiAliasing handoffMode=AntiAliasing::CurrentDefault;
    bool checkedSamples[5]={},supportedSamples[5]={};
    AZ::u64 lastTick=~AZ::u64(0);

    void Update(AZ::EntityId entity,AntiAliasing mode)
    {
        auto* rpi=AZ::RPI::RPISystemInterface::Get();
        auto found=AZStd::find_if(requests.begin(),requests.end(),[&](const auto& r){return r.entity==entity;});
        const bool changed=found==requests.end()?mode!=AntiAliasing::CurrentDefault:found->mode!=mode;
        if(changed)
        {
            if(r_silpomAaTrace)AZ_Printf("SilPOM AA","owner=%s mode=%u->%u requests=%zu tick=%llu\n",entity.ToString().c_str(),
                found==requests.end()?0u:unsigned(found->mode),unsigned(mode),requests.size(),
                static_cast<unsigned long long>(AZ::RPI::RPISystemInterface::Get()->GetCurrentTick()));
            if(!baselineCaptured)
            {
                originalMsaa4=r_silpomMsaa4;
                originalSamples=rpi->GetApplicationMultisampleState().m_samples;
                baselineCaptured=true;
            }
            if(found!=requests.end())requests.erase(found);
            if(mode!=AntiAliasing::CurrentDefault)requests.push_back({entity,mode});
        }
        if(requests.empty())
        {
            if(handoff||!baselineCaptured)return;
            for(const auto& state:pipelines)
                if(auto* scene=rpi->GetScene(state.scene))
                    if(auto pipeline=scene->GetRenderPipeline(state.pipeline))
                        pipeline->SetActiveAAMethod(AZ::RPI::RenderPipeline::GetAAMethodNameByIndex(state.mode));
            for(const auto& state:scenes)
                if(auto* scene=rpi->GetScene(state.scene))
                    if(auto* fp=scene->GetFeatureProcessor<AZ::Render::SMAAFeatureProcessorInterface>())fp->SetEnable(state.smaa);
            pipelines.clear();scenes.clear();r_silpomMsaa4=originalMsaa4;
            PendingHitSamples()=originalSamples;baselineCaptured=false;
            lastTick=~AZ::u64(0);return;
        }
        if(!changed&&lastTick==rpi->GetCurrentTick())return;
        lastTick=rpi->GetCurrentTick();
        const auto active=handoff&&handoffMode!=AntiAliasing::CurrentDefault?handoffMode:requests.back().mode;
        const auto method=active==AntiAliasing::SMAA?AZ::RPI::AntiAliasingMode::SMAA:
            active==AntiAliasing::TAA?AZ::RPI::AntiAliasingMode::TAA:AZ::RPI::AntiAliasingMode::MSAA;
        // Reuse the capability check and guarded sample-count transition below.
        const unsigned samples=active==AntiAliasing::MSAA4?4u:active==AntiAliasing::MSAA2?2u:1u;
        if(!checkedSamples[samples])
        {
            supportedSamples[samples]=SupportsHitMsaa(samples);checkedSamples[samples]=true;
            AZ_Warning("SilPOM",supportedSamples[samples],"%ux MSAA is unavailable on this device; retaining single-sample rendering.",samples);
        }
        PendingHitSamples()=supportedSamples[samples]?samples:1u;
        r_silpomMsaa4=PendingHitSamples()==4;
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

AZ::RPI::Ptr<AZ::RPI::Pass> MakeFaceCachePass(const AZ::RPI::Pass& reference)
{
    auto* system=AZ::RPI::PassSystemInterface::Get();
    const auto passTemplate=system->GetPassTemplate(AZ::Name("SilPOMFaceCacheTemplate"));
    if(!passTemplate)return {};
    const auto* source=azrtti_cast<const AZ::RPI::ComputePassData*>(passTemplate->m_passData.get());
    if(!source)return {};
    auto data=AZStd::make_shared<AZ::RPI::ComputePassData>(*source);
    data->m_pipelineViewTag=reference.GetPipelineViewTag();
    AZ::RPI::PassRequest request;
    request.m_passName=AZ::Name("SilPOMFaceCache");
    request.m_templateName=AZ::Name("SilPOMFaceCacheTemplate");
    request.m_passData=AZStd::move(data);
    return system->CreatePassFromRequest(&request);
}

bool InstallHitPasses(AZ::RPI::Scene& scene, bool required)
{
    if ((!required && !r_silpomReuseDepthHits) || AZ::RHI::Factory::Get().GetName() != AZ::Name("dx12")) return false;
    RegisterResolvePassCreator();
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
        if ((samples!=1 && samples!=2 && samples!=4)
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
        const auto sampleCount=pipeline->GetRenderSettings().m_multisampleState.m_samples;
        const bool msaa=sampleCount>1;
        const bool repairedCoverage=sampleCount<=2;
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
        if(!pipeline->FindFirstPass(AZ::Name("SilPOMFaceCache")))
        {
            auto prepare=MakeFaceCachePass(*pipeline->FindFirstPass(depthName));
            // Preparation is optional. Missing templates or unsupported pass
            // creation retain the established depth/forward path.
            if(prepare)pipeline->AddPassBefore(prepare,hitDepthName);
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
        if(auto classify=pipeline->FindFirstPass(classifyName))classify->SetEnabled(repairedCoverage||!r_silpomMsaaRawCoverage);
        if(auto fill=pipeline->FindFirstPass(fillName))fill->SetEnabled(repairedCoverage||!r_silpomMsaaRawCoverage);
        // The normal 2x renderer needs the same inset wall backing as 1x:
        // neural misses must not become sky-visible cracks through solid walls.
        // Keep experimental 4x raw coverage independent of this continuity path.
        if(auto backup=pipeline->FindFirstPass(backupName))backup->SetEnabled(repairedCoverage);
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
        // Explicit integer cache-use evidence, cleared to no-attempt for every
        // sample and drawn only by a receiver's UV material. Timings disable it.
        const char* usageNames[]={"SilPOMUvUsageClear","SilPOMUvUsage","SilPOMInspectUvUsage"};
        const char* usageTemplates[]={"SilPOMUvUsageClearTemplate","SilPOMUvUsageTemplate","SilPOMInspectUvUsageTemplate"};
        for(unsigned i=0;i<3;++i)
        {
            auto pass=pipeline->FindFirstPass(AZ::Name(usageNames[i]));
            if(!pass&&r_silpomMsaaInspect)
            {
                if(!system->GetPassTemplate(AZ::Name(usageTemplates[i]))&&!system->LoadPassTemplateMappings("passes/silpomhittemplates.azasset"))return false;
                if(i==1)pass=MakeHitPass(AZ::Name(usageNames[i]),AZ::Name(usageTemplates[i]),*pipeline->FindFirstPass(forwardName));
                else {AZ::RPI::PassRequest request;request.m_passName=AZ::Name(usageNames[i]);request.m_templateName=AZ::Name(usageTemplates[i]);pass=system->CreatePassFromRequest(&request);}
                if(!pass||!pipeline->AddPassAfter(pass,i==0?hitForwardName:AZ::Name(usageNames[i-1])))return false;
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

AZ::Data::Instance<AZ::RPI::Buffer> FaceCoefficientBuffer(size_t faces)
{
    // Only availability/version metadata is initially meaningful. The exact
    // FP32 coefficients are prepared from the final packed faces on the GPU.
    AZStd::vector<AZ::u32> zero(faces*12,0u);
    AZ::RPI::BufferAssetCreator creator;creator.Begin(AZ::Uuid::CreateRandom());
    AZ::RHI::BufferDescriptor descriptor;
    descriptor.m_bindFlags=AZ::RHI::BufferBindFlags::ShaderReadWrite|AZ::RHI::BufferBindFlags::CopyRead;
    descriptor.m_byteCount=zero.size()*sizeof(AZ::u32);
    creator.SetBuffer(zero.data(),descriptor.m_byteCount,descriptor);
    creator.SetBufferViewDescriptor(AZ::RHI::BufferViewDescriptor::CreateRaw(0,aznumeric_cast<AZ::u32>(descriptor.m_byteCount)));
    creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::ReadWrite);
    AZ::Data::Asset<AZ::RPI::BufferAsset> asset;
    if(!creator.End(asset))return {};
    return AZ::RPI::Buffer::FindOrCreate(asset);
}
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
    struct BatchPolicy { bool displaced=false; AZ::u32 lightingEstimator=0; AZ::Name forwardTag; };
    AZStd::vector<BatchPolicy> batchPolicies;
    // Publication intent survives temporary shader-product unavailability.
    // Certification is separate and gates every actual enabled draw.
    AZStd::atomic_bool strictRequired{false}, strictCertified{false};
    bool hiddenForInstancing=false;
    struct PaintFace {AZStd::string id;AZ::u32 source=0,batch=0,local=0,hit=0;};
    AZStd::vector<PaintFace> paintFaces;
    // Receiver-wide projection ownership survives canonical triangle edges.
    AZ::u32 paintReceiver=0;
    // Keep CPU picking and anchors on the same immutable source/scale as the
    // published model while a replacement configuration is loading or compiling.
    MeshConfig paintConfig;
    float paintScale=1;
    AZ::Transform paintWorld=AZ::Transform::CreateIdentity();
    struct ProxyPatch {AZStd::vector<AZ::u32> localFaces,sourceFaces;bool protectedBoundary=false,far=false;};
    struct ProxyBatch
    {
        ResolveBatchPtr resolveBatch;
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
    // One registry entry per complete object/material/owner batch. The pass
    // replaces its original resolve item independently for each render view.
    AZStd::vector<ResolveBatchPtr> resolveBatches;
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
namespace
{
AZ::Name StrictForwardTag(const Material& material)
{
    const auto flag=[&](const char* name)
    {
        const auto index=material->FindPropertyIndex(AZ::Name(name));
        return index.IsValid()&&material->GetPropertyValue(index).Is<bool>()&&material->GetPropertyValue(index).GetValue<bool>();
    };
    const auto estimator=material->FindPropertyIndex(AZ::Name("surface.lightingEstimator"));
    const auto light=estimator.IsValid()?material->GetPropertyValue(estimator).GetValue<AZ::u32>():0u;
    const bool proxy=flag("surface.proxyLodEnabled"),shared=flag("surface.sharedVisibility"),ambient=flag("surface.diffuseAccessibility");
    if(light==5)return AZ::Name(proxy?(ambient?"silpom_hit_forward_angular_proxy_ambient":"silpom_hit_forward_angular_proxy"):
        (ambient?"silpom_hit_forward_angular_ambient":"silpom_hit_forward_angular"));
    if(proxy&&!shared)return AZ::Name("silpom_hit_forward_level_five");
    if(shared)
        return AZ::Name(proxy?(light?(ambient?"silpom_hit_forward_shared_proxy_ambient":"silpom_hit_forward_shared_proxy_compare"):"silpom_hit_forward_shared_proxy"):
            (light?(ambient?"silpom_hit_forward_shared_ambient":"silpom_hit_forward_shared_compare"):"silpom_hit_forward_shared"));
    return AZ::Name(flag("surface.learnedMaterial")?"silpom_hit_forward_learned_material":"silpom_hit_forward_neural_only");
}
bool CertifyStrictPacket(const AZ::RPI::MeshDrawPacket& packet,bool displaced,AZ::u32 lightingEstimator,
    const AZ::Name& forwardTag,AZ::RHI::DrawListTag motionTag)
{
    if(!displaced)return true; // Ordinary authored batches perform no SILPOM query.
    const auto* draw=packet.GetRHIDrawPacket();
    const auto& shaders=packet.GetActiveShaderList();
    if(!draw||draw->GetDrawItemCount()!=shaders.size())return false;
    StrictNeuralPolicy::PacketProof proof;
    for(size_t i=0;i<shaders.size();++i)
    {
        if(draw->GetDrawListTag(i)==motionTag)continue; // Always disabled below.
        const auto& active=shaders[i];
        if(!active.m_shader||active.m_materialPipelineName!=AZ::Name("MainPipeline")
            ||active.m_shader->GetDrawListTag()!=draw->GetDrawListTag(i))return false;
        const auto& asset=active.m_shader->GetAsset();
        if(!asset||!asset.IsReady())return false;
        AZStd::string path;
        AZ::Data::AssetCatalogRequestBus::BroadcastResult(path,&AZ::Data::AssetCatalogRequests::GetAssetPathById,asset.GetId());
        for(auto& character:path)
        {
            if(character=='\\')character='/';
            if(character>='A'&&character<='Z')character=char(character-'A'+'a');
        }
        const auto& supervariant=asset->GetSupervariantName(active.m_shader->GetSupervariantIndex());
        if(asset->GetSupervariantIndex(supervariant)!=active.m_shader->GetSupervariantIndex())return false;
        const auto capability=StrictNeuralPolicy::Classify(active.m_shaderTag.GetCStr(),path.c_str(),supervariant.GetCStr());
        proof.Add(capability);
        if(capability!=StrictNeuralPolicy::Capability::Visibility)continue;
        if(active.m_shaderTag!=forwardTag)return false;
        AZ::RPI::ShaderOptionGroup requested(asset->GetShaderOptionGroupLayout(),active.m_requestedShaderVariantId);
        AZ::RPI::ShaderOptionGroup enabled(asset->GetShaderOptionGroupLayout(),active.m_activeShaderVariantId);
        requested.SetUnspecifiedToDefaultValues();
        const auto matches=[&](const char* name,AZ::u32 expected,bool required)
        {
            const auto index=requested.FindShaderOptionIndex(AZ::Name(name));
            if(!index.IsValid())return !required;
            const auto desired=requested.GetValue(index),baked=enabled.GetValue(index);
            return desired.IsValid()&&desired.GetIndex()==expected&&(!baked.IsValid()||baked.GetIndex()==expected);
        };
        // Root variants legitimately use the draw-SRG fallback key. A baked
        // option must agree too; product certification establishes the strict
        // compile boundary independently of optional runtime options.
        if(!matches("o_silpomLocalShadowMethod",3,false))return false;
        const bool angular=lightingEstimator==5;
        const bool fixedPhysical=active.m_shaderTag==AZ::Name("silpom_hit_forward_neural_only");
        if(!angular&&!matches("o_silpomLightingEstimator",lightingEstimator,lightingEstimator!=0&&!fixedPhysical))return false;
    }
    return proof.Complete();
}
}
#include "CameraCollision.inl"
#include "ReceiverUvPaint.inl"
#include "SurfacePaintRaster.inl"
#include "SurfacePaintRender.inl"
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
        sc->Class<MeshProfileBinding>()->Version(5)->Field("ProfileId", &MeshProfileBinding::m_id)
            ->Field("Profile", &MeshProfileBinding::m_profile)->Field("Height", &MeshProfileBinding::m_height)
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
            ->Value("SMAA",AntiAliasing::SMAA)->Value("TAA",AntiAliasing::TAA)
            ->Value("MSAA2",AntiAliasing::MSAA2)->Value("MSAA4",AntiAliasing::MSAA4);
        sc->Class<MeshConfig, AZ::ComponentConfig>()->Version(23)->Field("Surface", &MeshConfig::m_surface)
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
            ->Field("ReliefMode", &MeshConfig::m_reliefMode)
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
bool MeshConfig::UpgradeLegacyReliefMode()
{
    return ReliefModes::UpgradeLegacy(m_maxCells, m_reliefMode);
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
            ->Event("GetAntiAliasingState",&MeshRequests::GetAntiAliasingState)
            ->Event("GetPaintFacesJson",&MeshRequests::GetPaintFacesJson)
            ->Event("GetProfileReportJson",&MeshRequests::GetProfileReportJson)
            ->Event("GetSurfacePaintStatus",&MeshRequests::GetSurfacePaintStatus)
            ->Event("BeginReceiverUvPaint",&MeshRequests::BeginReceiverUvPaint)
            ->Event("SetReceiverUvPaintMode",&MeshRequests::SetReceiverUvPaintMode)
            ->Event("GetReceiverUvPaintStatus",&MeshRequests::GetReceiverUvPaintStatus)
            ->Event("ReleaseReceiverUvPaint",&MeshRequests::ReleaseReceiverUvPaint);
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
    return m_active && !m_dirty && !m_pending && m_status.starts_with("Ready:")
        &&StrictNeuralPolicy::CanEnable(m_active->published.load(),m_active->strictRequired.load(),m_active->strictCertified.load());
}
// {"status", "surface", "profiles": [{"id", "binding": {...} | null, "source":
// "override" | "surface" | "model 0 default" | "none", "product", "state":
// "ready" | "loading" | "not processed" | "invalid", "error", "profile": {...}}]}
AZStd::string MeshController::GetProfileReportJson() const
{
    rapidjson::StringBuffer text;
    rapidjson::Writer<rapidjson::StringBuffer> json(text);
    const auto string = [&json](const char* key, const AZStd::string& value) { json.Key(key); json.String(value.c_str()); };
    const auto hint = [](const auto& asset) -> AZStd::string
    {
        if (!asset.GetId().IsValid()) return {};
        AZStd::string path = asset.GetHint();
        if (path.empty()) AZ::Data::AssetCatalogRequestBus::BroadcastResult(path, &AZ::Data::AssetCatalogRequests::GetAssetPathById, asset.GetId());
        return path.empty() ? asset.GetId().ToString<AZStd::string>() : path;
    };
    const auto& c = m_configuration;
    json.StartObject();
    string("status", GetStatus());
    string("surface", hint(c.m_surface));
    // The solver budget this mesh renders with, so training can select for it.
    json.Key("validation");
    json.StartArray();
    json.Uint(c.m_validationIterations); json.Uint(c.m_validationRetryCap); json.Double(c.m_validationToleranceMultiplier);
    json.EndArray();
    AZStd::map<AZ::u32, AZStd::string> surfaceProfiles;
    if (c.m_surface.IsReady())
        for (const auto& profile : c.m_surface->m_profiles) surfaceProfiles[profile.m_id] = profile.m_profileProduct;
    AZStd::set<AZ::u32> ids;
    for (const auto& binding : c.m_profiles) ids.insert(binding.m_id);
    for (const auto& [id, product] : surfaceProfiles) ids.insert(id);
    json.Key("profiles");
    json.StartArray();
    for (const AZ::u32 id : ids)
    {
        json.StartObject();
        json.Key("id"); json.Uint(id);
        const MeshProfileBinding* binding = nullptr;
        for (const auto& candidate : c.m_profiles) if (candidate.m_id == id) binding = &candidate;
        json.Key("binding");
        if (!binding) json.Null();
        else
        {
            json.StartObject();
            json.Key("useBaseMesh"); json.Bool(binding->m_useBaseMesh);
            json.Key("model"); json.Uint(binding->m_neuralModel);
            json.Key("scale"); json.Double(binding->m_scale);
            json.Key("reference"); json.Double(binding->m_reference);
            json.Key("addressMode"); json.Uint(binding->m_addressMode);
            string("height", hint(binding->m_height));
            string("cache", hint(binding->m_neuralCache));
            string("override", hint(binding->m_profile));
            json.EndObject();
        }
        AZStd::string source = "none", product, state, error;
        const SurfaceProfileAsset* asset = nullptr;
        if (binding && binding->m_useBaseMesh) source = "base mesh";
        else if (const auto loaded = m_profileAssets.find(id); loaded != m_profileAssets.end())
        {
            source = binding && binding->m_profile.GetId().IsValid() ? "override" : "surface";
            product = hint(loaded->second);
            if (loaded->second.IsReady()) { asset = loaded->second.Get(); state = "ready"; error = asset->Validate(); }
            else state = loaded->second.IsError() ? "invalid" : "loading";
        }
        else if (const auto missing = m_missingProfiles.find(id); missing != m_missingProfiles.end())
        { source = "surface"; product = missing->second; state = "not processed"; }
        else if (binding && binding->m_neuralModel == 0 && c.m_useNeuralSeed && m_modelZero.IsReady()
            && binding->m_addressMode == m_modelZero->m_addressMode && std::abs(binding->m_scale - m_modelZero->m_scale) < 1e-6f
            && std::abs(binding->m_reference - m_modelZero->m_reference) < 1e-6f)
        { source = "model 0 default"; product = hint(m_modelZero); asset = m_modelZero.Get(); state = "ready"; }
        if (!error.empty()) state = "invalid";
        string("source", source); string("product", product); string("state", state); string("error", error);
        json.Key("profile");
        if (!asset) json.Null();
        else
        {
            json.StartObject();
            json.Key("shaderModel"); json.Uint(asset->m_shaderModel);
            json.Key("role"); json.Uint(static_cast<AZ::u32>(asset->m_role));
            json.Key("width"); json.Uint(asset->m_width);
            json.Key("heightPixels"); json.Uint(asset->m_heightPixels);
            json.Key("scale"); json.Double(asset->m_scale);
            json.Key("reference"); json.Double(asset->m_reference);
            json.Key("addressMode"); json.Uint(asset->m_addressMode);
            string("height", asset->m_heightProduct);
            string("cache", asset->m_cacheProduct);
            string("networks", asset->m_networks);
            string("generation", asset->m_generation);
            json.Key("flags"); json.Uint(asset->NetworkFlags());
            json.EndObject();
        }
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
    return text.GetString();
}
AZStd::string MeshController::GetStatus() const
{
    if(m_active&&m_active->strictRequired.load()&&!m_active->strictCertified.load())
        return m_status+"; strict neural policy suppressed uncertified previous draw shaders";
    return m_status;
}
bool MeshController::HasTrainedRenderingDuty() const
{
    return m_configuration.UsesTrainedProfiles()||m_resolvedTrained||(m_active&&m_active->trainedModelBatches)
        ||(m_pending&&m_pending->trainedModelBatches);
}
void MeshController::UpdateSceneSubscription()
{
    StrictNeuralPolicy::SceneDuties duties;
    duties.requestedStrict=m_configuration.m_localShadowMethod==LocalShadowMethod::NeuralOnly;
    duties.requestedTrained=m_configuration.UsesTrainedProfiles()||m_resolvedTrained;
    if(m_active)
    {
        duties.activeStrict=m_active->strictRequired.load();
        duties.activeRestoration=m_active->hiddenForInstancing;
        duties.activeTrained=m_active->trainedModelBatches!=0;
    }
    if(m_pending)
    {
        duties.pendingStrict=m_pending->strictRequired.load();
        duties.pendingRestoration=m_pending->hiddenForInstancing;
        duties.pendingTrained=m_pending->trainedModelBatches!=0;
    }
    auto* scene=m_entity.IsValid()?AZ::RPI::Scene::GetSceneForEntityId(m_entity):nullptr;
    if((!duties.NeedsNotifications()&&!m_surfacePaintRuntime)||!scene)
    {
        AZ::RPI::SceneNotificationBus::Handler::BusDisconnect();
        if(!duties.TrainedRendering())m_checkHitPasses=false;
        return;
    }
    if(!AZ::RPI::SceneNotificationBus::Handler::BusIsConnectedId(scene->GetId()))
    {
        AZ::RPI::SceneNotificationBus::Handler::BusDisconnect();
        AZ::RPI::SceneNotificationBus::Handler::BusConnect(scene->GetId());
    }
}
void MeshController::EnforceStrictGenerationPolicy(bool finalBarrier)
{
    UpdateSceneSubscription();
    if(!m_processor)return;
    const bool strict=m_configuration.m_localShadowMethod==LocalShadowMethod::NeuralOnly;
    const auto motionTag=AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("motion"));
    for(auto* generation:{m_active.get(),m_pending.get()})
    {
        if(!generation)continue;
        const bool wasStrict=generation->strictRequired.exchange(strict);
        if(strict&&AZ::Render::r_meshInstancingEnabled)
        {
            // Atom hides its per-mesh packet inventory while instancing is
            // enabled. Reject that unsupported policy before culling rather
            // than treating an inaccessible inventory as certified.
            generation->strictCertified.store(false);
            generation->hiddenForInstancing=true;
            for(size_t batch=0;batch<generation->handles.size();++batch)
            {
                m_processor->SetVisible(generation->handles[batch],false);
                SetResolveBatchPublished(generation->resolveBatches[batch],false);
            }
            continue;
        }
        if(generation->hiddenForInstancing)
        {
            generation->hiddenForInstancing=false;
            for(const auto& handle:generation->handles)m_processor->SetVisible(handle,true);
        }
        // Leave baseline/hybrid draw controls untouched. Only leaving a
        // previously applied strict policy needs restoration here.
        if(!strict&&!wasStrict)continue;
        bool certified=!generation->handles.empty()&&generation->batchPolicies.size()==generation->handles.size();
        if(strict&&certified)
            for(size_t batch=0;batch<generation->handles.size();++batch)
            {
                const auto& lods=m_processor->GetDrawPackets(generation->handles[batch]);
                if(lods.empty()){certified=false;break;}
                const auto& policy=generation->batchPolicies[batch];
                for(const auto& lod:lods)
                {
                    if(lod.empty())certified=false;
                    for(const auto& packet:lod)
                        certified&=CertifyStrictPacket(packet,policy.displaced,policy.lightingEstimator,policy.forwardTag,motionTag);
                }
            }
        generation->strictCertified.store(strict&&certified);
        const bool enable=StrictNeuralPolicy::CanEnable(generation->published.load(),strict,certified);
        for(size_t batch=0;batch<generation->handles.size();++batch)
        {
            // Keep initialization/packet updates running while suppressed.
            // SetVisible(false) would prevent Atom from preparing recovery.
            const auto& handle=generation->handles[batch];
            for(const auto& lod:m_processor->GetDrawPackets(handle))for(const auto& packet:lod)
                if(const auto* draw=packet.GetRHIDrawPacket())
                {
                    for(size_t i=0;i<draw->GetDrawItemCount();++i)
                        m_processor->SetDrawItemEnabled(handle,draw->GetDrawListTag(i),enable&&draw->GetDrawListTag(i)!=motionTag);
                    if(finalBarrier||!strict)
                    {
                        // Per-mesh shader flags can rebuild packets without
                        // sending the update event, after our Begin handler.
                        // End runs after collection, whose lists retain these
                        // DrawItems. Native submission ignores m_enabled, so
                        // also suppress already-collected draws at execution.
                        // These private generated models require instancing
                        // disabled and each original draw has one instance.
                        const_cast<AZ::RHI::DrawPacket*>(draw)->SetInstanceCount(enable?1u:0u);
                        for(size_t i=0;i<draw->GetDrawItemCount();++i)
                            if(draw->GetDrawListTag(i)==motionTag)
                            {
                                AZ::RHI::DrawInstanceArguments disabled;
                                disabled.m_instanceCount=0;
                                const_cast<AZ::RHI::DrawItem*>(draw->GetDrawItem(i))->SetDrawInstanceArgs(disabled);
                            }
                    }
                }
            SetResolveBatchPublished(generation->resolveBatches[batch],enable);
        }
    }
    // Leaving strict mode can finish the last restoration duty above. Keep
    // the subscription until that work completes, then release it centrally.
    UpdateSceneSubscription();
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
bool MeshController::ResolveProfileAssets()
{
    AZStd::map<AZ::u32, AZ::Data::AssetId> wanted;
    AZStd::map<AZ::u32, AZStd::string> missing;
    const auto& surface = m_configuration.m_surface;
    if (surface.IsReady())
        for (const auto& profile : surface->m_profiles)
        {
            const auto id = FindProduct(profile.m_profileProduct, azrtti_typeid<SurfaceProfileAsset>());
            if (id.IsValid()) wanted[profile.m_id] = id;
            else missing[profile.m_id] = profile.m_profileProduct;
        }
    for (const auto& binding : m_configuration.m_profiles)
    {
        if (binding.m_useBaseMesh) { wanted.erase(binding.m_id); missing.erase(binding.m_id); continue; }
        if (binding.m_profile.GetId().IsValid()) { wanted[binding.m_id] = binding.m_profile.GetId(); missing.erase(binding.m_id); }
    }
    bool changed = missing != m_missingProfiles;
    m_missingProfiles = AZStd::move(missing);
    for (auto it = m_profileAssets.begin(); it != m_profileAssets.end();)
    {
        const auto found = wanted.find(it->first);
        if (found == wanted.end() || found->second != it->second.GetId()) { it = m_profileAssets.erase(it); changed = true; }
        else ++it;
    }
    for (const auto& [profileId, assetId] : wanted)
        if (!m_profileAssets.contains(profileId))
        {
            m_profileAssets[profileId] = AZ::Data::AssetManager::Instance().GetAsset<SurfaceProfileAsset>(assetId, AZ::Data::AssetLoadBehavior::NoLoad);
            changed = true;
        }
    // Height images and seed caches follow their loaded profiles.
    AZStd::map<AZ::u32, AZ::Data::AssetId> heights, caches;
    AZStd::set<AZ::u32> bindingHeights;
    for (const auto& binding : m_configuration.m_profiles)
        if (!m_profileAssets.contains(binding.m_id)) bindingHeights.insert(binding.m_id);
    for (const auto& [profileId, profile] : m_profileAssets)
        if (profile.IsReady())
        {
            if (!profile->m_heightProduct.empty())
                heights[profileId] = FindProduct(profile->m_heightProduct, azrtti_typeid<AZ::RPI::StreamingImageAsset>());
            else bindingHeights.insert(profileId);
            if (!profile->m_cacheProduct.empty())
                caches[profileId] = FindProduct(profile->m_cacheProduct, azrtti_typeid<AZ::RPI::StreamingImageAsset>());
        }
    const auto follow = [&changed](auto& images, const AZStd::map<AZ::u32, AZ::Data::AssetId>& wantedImages)
    {
        for (auto it = images.begin(); it != images.end();)
        {
            const auto found = wantedImages.find(it->first);
            if (found == wantedImages.end() || found->second != it->second.GetId()) { it = images.erase(it); changed = true; }
            else ++it;
        }
        for (const auto& [profileId, assetId] : wantedImages)
            if (!images.contains(profileId))
            {
                images[profileId] = AZ::Data::Asset<AZ::RPI::StreamingImageAsset>(assetId, azrtti_typeid<AZ::RPI::StreamingImageAsset>());
                changed = true;
            }
    };
    follow(m_profileHeights, heights);
    follow(m_profileCaches, caches);
    if (bindingHeights != m_bindingHeights) { m_bindingHeights = AZStd::move(bindingHeights); changed = true; }
    const auto modelZero = m_configuration.m_useNeuralSeed
        ? FindProduct("silpom/profiles/modelzero.silpomprofileasset", azrtti_typeid<SurfaceProfileAsset>()) : AZ::Data::AssetId();
    if (modelZero != m_modelZero.GetId())
    {
        m_modelZero = modelZero.IsValid() ? AZ::Data::AssetManager::Instance().GetAsset<SurfaceProfileAsset>(
            modelZero, AZ::Data::AssetLoadBehavior::NoLoad) : AZ::Data::Asset<SurfaceProfileAsset>();
        changed = true;
    }
    m_resolvedTrained = !m_profileAssets.empty() || !m_missingProfiles.empty();
    return changed;
}
void MeshController::ConnectAssets()
{
    AZ::Data::AssetBus::MultiHandler::BusDisconnect();
    ResolveProfileAssets();
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
        if(FindLearnedBundle(m_configuration.m_useLevelFourShared?"LevelFourShared":"LevelFourMaterial",id))
            m_learnedMaterial=AZ::Data::AssetManager::Instance().GetAsset<LearnedMaterialAsset>(id,AZ::Data::AssetLoadBehavior::NoLoad);
        queue(m_learnedMaterial);
    }
    if(m_configuration.m_useLevelFiveProxy)
    {
        AZ::Data::AssetId id;
        // The two encodings are separate registered bundles.
        if(FindLearnedBundle(m_configuration.m_useLevelFiveDepth?"LevelFiveDepth":"LevelFiveAppearance",id))
            m_appearance=AZ::Data::AssetManager::Instance().GetAsset<LearnedMaterialAsset>(id,AZ::Data::AssetLoadBehavior::NoLoad);
        queue(m_appearance);
    }
    for (auto& binding : m_configuration.m_materials) queue(binding.m_material);
    for (auto& profile : m_configuration.m_profiles)
    {
        if (profile.m_useBaseMesh) continue;
        // A trained profile's own height image replaces the binding's.
        if (m_bindingHeights.contains(profile.m_id)) queue(profile.m_height);
        queue(profile.m_neuralCache);
    }
    for (auto& [id, profile] : m_profileAssets) queue(profile);
    for (auto& [id, height] : m_profileHeights) queue(height);
    for (auto& [id, cache] : m_profileCaches) queue(cache);
    queue(m_modelZero);
}
void BeginAntiAliasingHandoff()
{
    auto& state=AntiAliasingState();
    state.handoff=true;
    state.handoffMode=state.requests.empty()?AntiAliasing::CurrentDefault:state.requests.back().mode;
    if(r_silpomAaTrace)AZ_Printf("SilPOM AA","begin handoff mode=%u\n",unsigned(state.handoffMode));
}
void EndAntiAliasingHandoff()
{
    auto& state=AntiAliasingState();state.handoff=false;state.lastTick=~AZ::u64(0);
    state.Update(AZ::EntityId{},static_cast<AntiAliasing>(AZ::u32(r_silpomAntiAliasing)));
    UpdateHitSamplePolicy(nullptr);
    if(r_silpomAaTrace)AZ_Printf("SilPOM AA","end handoff requests=%zu\n",state.requests.size());
}
void TickRenderingSettings()
{
    auto* rhi=AZ::RHI::RHISystemInterface::Get();
    auto* passes=AZ::RPI::PassSystemInterface::Get();
    if(!rhi||rhi->GetDeviceCount()==0||!passes||!passes->GetRootPass()||!AZ::RPI::RPISystemInterface::Get())return;
    if(r_silpomAntiAliasing>4u)r_silpomAntiAliasing=0u;
    AntiAliasingState().Update(AZ::EntityId{},static_cast<AntiAliasing>(AZ::u32(r_silpomAntiAliasing)));
    UpdateHitSamplePolicy(nullptr);
}
namespace
{
struct RenderingSettingsTicker : AZ::TickBus::Handler
{
    void OnTick(float,AZ::ScriptTimePoint) override { TickRenderingSettings(); }
};
RenderingSettingsTicker& RenderingTicker() { static RenderingSettingsTicker ticker;return ticker; }
}
void StartRenderingSettings() { RenderingTicker().BusConnect(); }
void StopRenderingSettings() { RenderingTicker().BusDisconnect(); }
void MeshController::Activate(AZ::EntityId entity)
{
    m_entity = entity; m_dirty = true;
    // Prefabs load through JSON, so serializer version converters do not run.
    m_configuration.UpgradeLegacyReliefMode();
    UpdateSceneSubscription();
    m_paintSurfaceRevision=++ReceiverSurfaceRevision;
    // Migrate an old component selection once; renderer controls own all later
    // changes. The serialized field remains readable but is no longer editable.
    static bool migratedLegacySelection=false;
    if(!migratedLegacySelection&&m_configuration.m_antiAliasing!=AntiAliasing::CurrentDefault)
    {
        if(r_silpomAntiAliasing==0u)r_silpomAntiAliasing=static_cast<AZ::u32>(m_configuration.m_antiAliasing);
        migratedLegacySelection=true;
    }
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
    if (generation) for (const auto& batch : generation->resolveBatches) SetResolveBatchPublished(batch, false);
    if (generation && m_processor) for (auto& handle : generation->handles) if (handle.IsValid()) m_processor->ReleaseMesh(handle);
    generation.reset();
}
void MeshController::Deactivate()
{
    AZ::RPI::SceneNotificationBus::Handler::BusDisconnect();
    m_checkHitPasses = false;
    AZ::TickBus::Handler::BusDisconnect(); MeshRequestBus::Handler::BusDisconnect();
    AZ::TransformNotificationBus::Handler::BusDisconnect(); AZ::Data::AssetBus::MultiHandler::BusDisconnect();
    ReleaseReceiverUvPaint();
    ReleaseSurfacePaintTextures();
    Release(m_pending); Release(m_active); m_processor = nullptr; m_entity.SetInvalid(); m_status = "Inactive";
}
void MeshController::SetConfiguration(const MeshConfig& config)
{
    m_configuration = config; m_configuration.UpgradeLegacyReliefMode(); m_dirty = true;
    // Strictness takes effect before loading or validation can fail. Retention
    // of an old generation is conditional on its actual shader capabilities.
    EnforceStrictGenerationPolicy();
    m_paintSurfaceRevision=++ReceiverSurfaceRevision;
    if (m_entity.IsValid())
    {
        Release(m_pending);
        UpdateSceneSubscription();
        ConnectAssets();
    }
}
void MeshController::OnTransformChanged(const AZ::Transform&, const AZ::Transform& world)
{
    m_paintSurfaceRevision=++ReceiverSurfaceRevision;
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
                generation->paintWorld=rigid;
                for (const auto& batch : generation->resolveBatches) SetResolveBatchWorld(batch, rigid);
                for (const auto& handle : generation->handles) m_processor->SetTransform(handle, rigid);
            }
}
void MeshController::OnAssetReady(AZ::Data::Asset<AZ::Data::AssetData> asset) { OnAssetReloaded(asset); }
void MeshController::OnAssetReloaded(AZ::Data::Asset<AZ::Data::AssetData> asset)
{
    m_paintSurfaceRevision=++ReceiverSurfaceRevision;
    if (asset.GetId() == m_configuration.m_surface.GetId()) { m_configuration.m_surface = asset; m_profilesStale = true; }
    for (auto& [id, profile] : m_profileAssets) if (asset.GetId() == profile.GetId()) { profile = asset; m_profilesStale = true; }
    for (auto& [id, height] : m_profileHeights) if (asset.GetId() == height.GetId()) height = asset;
    for (auto& [id, cache] : m_profileCaches) if (asset.GetId() == cache.GetId()) cache = asset;
    if (asset.GetId() == m_modelZero.GetId()) m_modelZero = asset;
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
{
    EnforceStrictGenerationPolicy();
    Release(m_pending); m_dirty = false;
    m_status = "Asset load failed; only a policy-compatible previous generation may remain visible. Check Asset Processor.";
}

void MeshController::OnRenderPipelineChanged(AZ::RPI::RenderPipeline* pipeline, AZ::RPI::SceneNotification::RenderPipelineChangeType)
{
    if (HasTrainedRenderingDuty() && pipeline && pipeline->GetDescriptor().m_materialPipelineTag == "MainPipeline")
    {
        m_checkHitPasses = true;
    }
}

bool MeshController::Prepare()
{
    // Strict requests need the final render barrier even if any validation or
    // asset-loading branch below returns before constructing a replacement.
    UpdateSceneSubscription();
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
        if(!m_appearance.IsReady()){m_status=AZStd::string::format("Register and process the %s bundle (%s/%s) before enabling Level %s",
            c.m_useLevelFiveDepth?"Level 5B depth":"Level 5A appearance",LearnedBundlesRegistryRoot,c.m_useLevelFiveDepth?"LevelFiveDepth":"LevelFiveAppearance",
            c.m_useLevelFiveDepth?"5B":"5A");return m_appearance.GetId().IsValid();}
        if(!m_appearance->Validate()||memcmp(m_appearance->Header().magic,c.m_useLevelFiveDepth?"SILPDEP5":"SILPAPP5",8))
        {m_status="Invalid Level 5A appearance bundle";return false;}
    }
    if(c.m_useLevelFourWallMaterial||c.m_useLevelFourShared)
    {
        if(c.m_localShadowMethod!=LocalShadowMethod::NeuralOnly){m_status="Level 4A requires neural-only local visibility; select it before enabling material decoding";return false;}
        if(!m_learnedMaterial.GetId().IsValid()){m_status=AZStd::string::format("Register and process the %s bundle (%s/%s) before enabling Level %s",
            c.m_useLevelFourShared?"Level 4B shared":"Level 4A material",LearnedBundlesRegistryRoot,c.m_useLevelFourShared?"LevelFourShared":"LevelFourMaterial",
            c.m_useLevelFourShared?"4B":"4A");return false;}
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
        {
            MeshProfileBinding profile; profile.m_id = face.m_profile; c.m_profiles.push_back(profile);
            const bool trainedDefault = AZStd::any_of(asset.m_profiles.begin(), asset.m_profiles.end(),
                [&](const auto& p) { return p.m_id == face.m_profile; });
            initialized = initialized || !trainedDefault;
        }
    if (initialized) { m_status = "Material slots and profiles discovered; assign their materials and height images"; return false; }
    AZStd::map<AZStd::string, const MeshMaterialBinding*> materialBindings;
    AZStd::map<AZ::u32, MeshProfileBinding> profiles;
    for (const auto& b : c.m_materials)
    {
        if (!materialBindings.emplace(b.m_id, &b).second) { m_status = "Duplicate material binding"; return false; }
        if (!b.m_material.GetId().IsValid()) { m_status = "Assign material: " + b.m_id; return false; }
        if (!b.m_material.IsReady()) { m_status = "Loading materials"; return true; }
    }
    for (const auto& binding : c.m_profiles)
    {
        // A trained profile asset supplies the height image and relief parameters.
        MeshProfileBinding p = binding;
        if (const auto missing = m_missingProfiles.find(p.m_id); !p.m_useBaseMesh && missing != m_missingProfiles.end())
        { m_status = AZStd::string::format("Profile %u: surface profile %s is not processed; check Asset Processor", p.m_id, missing->second.c_str()); return false; }
        if (const auto trained = m_profileAssets.find(p.m_id); !p.m_useBaseMesh && trained != m_profileAssets.end())
        {
            if (!trained->second.IsReady()) { m_status = "Loading surface profiles"; return true; }
            const auto& profile = *trained->second;
            if (const auto invalid = profile.Validate(); !invalid.empty())
            { m_status = AZStd::string::format("Profile %u: invalid surface profile (%s)", p.m_id, invalid.c_str()); return false; }
            // A model 0 profile without a height image applies to the binding's.
            if (!profile.m_heightProduct.empty())
            {
                const auto height = m_profileHeights.find(p.m_id);
                if (height == m_profileHeights.end() || !height->second.GetId().IsValid())
                { m_status = AZStd::string::format("Profile %u: height image %s is not processed", p.m_id, profile.m_heightProduct.c_str()); return false; }
                p.m_height = height->second;
            }
            if (!profile.m_cacheProduct.empty())
            {
                const auto cache = m_profileCaches.find(p.m_id);
                if (cache == m_profileCaches.end() || !cache->second.GetId().IsValid())
                { m_status = AZStd::string::format("Profile %u: seed cache %s is not processed", p.m_id, profile.m_cacheProduct.c_str()); return false; }
                p.m_neuralCache = cache->second;
            }
            p.m_scale = profile.m_scale; p.m_reference = profile.m_reference;
            p.m_addressMode = profile.m_addressMode; p.m_neuralModel = profile.m_shaderModel;
        }
        else if (!p.m_useBaseMesh && p.m_neuralModel != 0)
        { m_status = AZStd::string::format("Profile %u needs a trained surface profile (.silpomprofile) for model %u", p.m_id, p.m_neuralModel); return false; }
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
    if (!std::isfinite(scale) || scale <= 0 || c.m_maxCells < 1 || c.m_maxCells > 65536 || c.m_reliefShadowSteps > 64
        || !ReliefModes::IsValid(c.m_reliefMode))
    { m_status = "Invalid positive uniform scale, relief mode, cell budget or relief shadow steps (0-64)"; return false; }
    auto* scene = AZ::RPI::Scene::GetSceneForEntityId(m_entity);
    if (!scene) { m_status = "Waiting for render scene"; return true; }
    m_processor = scene->GetFeatureProcessor<AZ::Render::MeshFeatureProcessorInterface>();
    if (!m_processor) { m_status = "Waiting for MeshFeatureProcessor"; return true; }
    // Profiles resolved to a trained model 1-7 (model 0 profiles are the seed path).
    const bool requiresTrainedHits = AZStd::any_of(profiles.begin(), profiles.end(),
        [](const auto& entry) { return !entry.second.m_useBaseMesh && entry.second.m_neuralModel != 0; });
    if (c.m_useNeuralSeed && m_modelZero.GetId().IsValid() && !m_modelZero.IsReady())
    { m_status = "Loading model 0 networks"; return true; }
    // SilPOM::SurfaceRole of a profile ID; untrained profiles are plain surfaces.
    const auto ProfileRole = [this](AZ::u32 id)
    {
        const auto found = m_profileAssets.find(id);
        return found != m_profileAssets.end() && found->second.IsReady() ? static_cast<AZ::u32>(found->second->m_role) : 0u;
    };
    const float depthMultiplier = requiresTrainedHits ? c.m_reliefDepthMultiplier : 1.0f;
    if (!std::isfinite(depthMultiplier) || depthMultiplier < 0.0f || depthMultiplier > MeshConfig::MaxReliefDepthMultiplier)
    { m_status = "Relief depth multiplier must be between 0 and 2"; return false; }
    // A model 0 surface profile's seed and no-march hit networks run in the
    // same hit passes as the trained models, so they install them too rather
    // than depending on r_silpomReuseDepthHits.
    const bool usesModelZeroProfiles = c.m_useNeuralSeed && AZStd::any_of(profiles.begin(), profiles.end(),
        [this](const auto& entry) { return !entry.second.m_useBaseMesh && entry.second.m_neuralModel == 0
            && m_profileAssets.contains(entry.first); });
    const bool reuseDepthHits = InstallHitPasses(*scene, requiresTrainedHits || usesModelZeroProfiles);
    const bool neuralVisibilityOnly = c.m_localShadowMethod == LocalShadowMethod::NeuralOnly;
    if((c.m_neuralDirectionalVisibility||c.m_proxyDirectionalResponse)&&!neuralVisibilityOnly)
    {m_status="Learned directional controls require neural-only visibility; they never enable a marcher";return false;}
    if (neuralVisibilityOnly && (!requiresTrainedHits || !reuseDepthHits || std::abs(depthMultiplier-.5f)>1e-6f))
    { m_status="Neural-only visibility requires trained profiles, calibrated relief depth 0.5 and DX12 MainPipeline hit reuse";return false; }
    if (neuralVisibilityOnly && ((c.m_spiralLightShadowSteps>0 && c.m_spiralLightShadowSteps<5)
        || (c.m_reliefShadowSteps>0 && c.m_reliefShadowSteps<5)))
    { m_status="Neural-only visibility requires shadow schedules of 0 (off) or 5-64; it never switches to marching";return false; }
    if (requiresTrainedHits && (!reuseDepthHits || !c.m_useNeuralSeed || c.m_reliefMode != ReliefMode::Lod4pxResolve || c.m_reliefShadowSteps != 0))
    {
        m_status = "Trained profiles require DX12 winning-hit resolve (Relief mode: LOD 4 px + resolve) and relief shadow steps 0; use the learned directional controls";
        return true;
    }
    if (AZ::Render::r_meshInstancingEnabled)
    { m_status = "Planar face renderer currently requires r_meshInstancingEnabled=false"; return false; }
    auto candidate = AZStd::make_unique<Generation>(); candidate->id = asset.m_generation;
    candidate->strictRequired.store(neuralVisibilityOnly);
    candidate->paintReceiver=AllocateHitFaceBase(1);
    candidate->paintConfig=c;candidate->paintScale=scale;
    candidate->paintWorld=m_world;candidate->paintWorld.SetUniformScale(1);
    if ((c.m_useLevelFourWallMaterial || c.m_useLevelFourShared) && m_learnedMaterial.IsReady())
        candidate->id += ":" + m_learnedMaterial->m_generation;
    if (c.m_useLevelFiveProxy && m_appearance.IsReady()) candidate->id += ":" + m_appearance->m_generation;
    auto fail = [&](const AZStd::string& message) { Release(candidate); m_status = message; return false; };
    if(!candidate->paintReceiver)return fail("SilPOM receiver ownership IDs exhausted");
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
            AZ::u32 resolveOwnerBase = 0;
            FaceCacheBatchInputs faceCacheInputs;
            AZStd::vector<Vertex> vertices; AZStd::vector<AZ::u32> indices;
            AZ::Aabb bounds = AZ::Aabb::CreateNull();
            AZ::Aabb resolveBounds = AZ::Aabb::CreateNull();
            AZStd::vector<AZ::Aabb> resolveClusters;
            if (key.second != 0) resolveClusters.reserve(faces.size());
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
                    const auto layout=binding.m_material->GetMaterialPropertiesLayout();const auto& values=binding.m_material->GetPropertyValues();
                    AZ::Data::AssetId expected[3];
                    decoded=FindLearnedWallMaterialImages(expected);
                    for(size_t channel=0;decoded&&channel<3;++channel)
                    {
                        const auto index=layout->FindPropertyIndex(AZ::Name(names[channel]));
                        decoded=index.IsValid() && values[index.GetIndex()].Is<AZ::Data::Asset<AZ::RPI::ImageAsset>>()
                            && values[index.GetIndex()].GetValue<AZ::Data::Asset<AZ::RPI::ImageAsset>>().GetId()==expected[channel];
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
                    const auto visibilityProfile = m_profileAssets.find(key.second);
                    if (visibilityProfile == m_profileAssets.end() || !visibilityProfile->second.IsReady()
                        || !(visibilityProfile->second->NetworkFlags() & SurfaceProfileAsset::VisibilityFlag) || reliefScale<0)
                        return fail("Neural-only visibility needs a trained profile with visibility networks (Train profile: hit network, Level 2 and visibility) and nonnegative relief");
                    const auto profileEstimator=StrictNeuralPolicy::LightingEstimatorForProfile(p.m_neuralModel,static_cast<AZ::u32>(c.m_lightingEstimator));
                    if(profileEstimator>=2u&&(!decoded||!c.m_useLevelFourShared))
                        return fail("Selected directional/angular estimator requires its canonical wall material and shared features; unsupported materials cannot use a different estimator");
                    // The trained profile supplied p.m_height, so it is the training image.
                    if (!m_profileAssets.contains(key.second))
                        return fail("Neural-only visibility requires a trained surface profile for this profile ID");
                    // Estimators are networks of the profile; only profiles trained with one can select it.
                    if (profileEstimator>=2u && !m_profileAssets.find(key.second)->second->HasEstimator(profileEstimator-2u))
                        return fail("The surface profile has no trained network for the selected directional/angular estimator");
                }
                if (requiresTrainedHits)
                {
                    const auto trained = m_profileAssets.find(key.second);
                    // Model 0 surface profiles may sit beside trained models
                    // (seed path per batch); the legacy model 0 cache may not.
                    if (p.m_neuralCache.GetId().IsValid() || trained == m_profileAssets.end())
                        return fail("Assign each displaced profile a surface profile (model 0 included) and no model 0 cache");
                    if (desc.m_size.m_width != trained->second->m_width || desc.m_size.m_height != trained->second->m_heightPixels
                        || p.m_tiling != AZ::Vector2(1) || p.m_offset != AZ::Vector2(0))
                        return fail("Trained surface profile does not match its height image, or the profile is tiled/offset");
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
                    AZ::Aabb resolveFaceBounds = AZ::Aabb::CreateNull();
                    for (const auto& corner : corners)
                    {
                        const auto point=corner.m_position*scale;
                        bounds.AddPoint(point+corner.m_direction*(-reliefScale*p.m_reference));
                        bounds.AddPoint(point+corner.m_direction*(reliefScale*(1-p.m_reference)));
                        // Match the packed face reconstruction used by every
                        // hit writer, including shell/proxy vertices. Include
                        // multiply-add order differences and the ray slab's
                        // 1e-5 local expansion before projecting on the CPU.
                        const auto delta = corner.m_uv - uv0;
                        const auto reconstructed = a + du * delta.GetX() + dv * delta.GetY();
                        const float lowHeight = -reliefScale * p.m_reference;
                        const float highHeight = reliefScale * (1 - p.m_reference);
                        const auto magnitude = a.GetAbs() + du.GetAbs() * std::abs(delta.GetX())
                            + dv.GetAbs() * std::abs(delta.GetY())
                            + corner.m_direction.GetAbs() * AZStd::max(std::abs(lowHeight), std::abs(highHeight));
                        const auto padding = magnitude * (1.0f / 262144.0f) + AZ::Vector3(2e-5f);
                        for (const float height : {lowHeight, highHeight})
                        {
                            const auto displaced = reconstructed + corner.m_direction * height;
                            resolveFaceBounds.AddPoint(displaced - padding);
                            resolveFaceBounds.AddPoint(displaced + padding);
                        }
                    }
                    // Each face is a spatial cluster of the same published
                    // batch, not another material/object identity. Keeping its
                    // complete producer bound avoids a batch-wide box spanning
                    // the camera when walls surround an interior viewpoint.
                    resolveBounds.AddAabb(resolveFaceBounds);
                    resolveClusters.push_back(resolveFaceBounds);
                }
                candidate->bytes+=packed.size()*sizeof(float);
                auto data=Buffer(packed.data(),packed.size()/4,16,true);
                auto buffer=AZ::RPI::Buffer::FindOrCreate(data);
                if (!buffer) return fail("Cannot allocate planar face descriptors");
                buffer->WaitForUpload();
                const auto bindless=buffer->GetBufferView()->GetBindlessReadIndex();
                if(bindless.size()!=1 || bindless.begin()->second==AZ::u32(-1)) return fail("One bindless-capable GPU is required");
                // Producer accounting covers every hit-reuse mode, including
                // those without the optional coefficient/view caches.
                if(reuseDepthHits)
                {
                    faceCacheInputs.faces=buffer;
                    faceCacheInputs.meshBufferIndex=bindless.begin()->second;
                    faceCacheInputs.faceCount=aznumeric_cast<AZ::u32>(faces.size());
                    faceCacheInputs.scale=reliefScale;faceCacheInputs.reference=p.m_reference;
                    faceCacheInputs.reliefMode=static_cast<AZ::u32>(c.m_reliefMode);faceCacheInputs.profileRole=ProfileRole(key.second);
                    faceCacheInputs.reliefLodPixelBudget=c.m_reliefLodPixelBudget;
                    faceCacheInputs.maxReliefDistance=c.m_maxReliefDistance;
                }
                if(reuseDepthHits && c.m_reliefMode==ReliefMode::Lod4pxResolve)
                {
                    auto coefficients=FaceCoefficientBuffer(faces.size());
                    if(coefficients)
                    {
                        coefficients->WaitForUpload();
                        const auto coefficientIndices=coefficients->GetBufferView()->GetBindlessReadIndex();
                        if(coefficientIndices.size()==1&&coefficientIndices.begin()->second!=AZ::u32(-1))
                        {
                            faceCacheInputs.coefficients=coefficients;
                            candidate->bytes+=faces.size()*48u;
                            candidate->buffers.push_back(coefficients);
                        }
                    }
                }
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
                if(faceCacheInputs.coefficients)
                {
                    const auto coefficientIndices=faceCacheInputs.coefficients->GetBufferView()->GetBindlessReadIndex();
                    if(!Set(material,"mesh.faceCoefficientBuffer",coefficientIndices.begin()->second))
                        return fail("Face coefficient material contract mismatch");
                }
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
                    faceCacheInputs.proxyLodEnabled=true;
                    faceCacheInputs.proxySelection=batch.buffer;
                    faceCacheInputs.proxyBufferIndex=batch.bufferIndex;
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
                // Model 0's networks: its profile's, or the gem's default for a
                // binding with model 0's calibration and no profile.
                const auto profileAsset = m_profileAssets.find(key.second);
                const SurfaceProfileAsset* modelZero = nullptr;
                if (reuseDepthHits && c.m_useNeuralSeed && p.m_neuralModel == 0)
                {
                    if (profileAsset != m_profileAssets.end()) modelZero = profileAsset->second.Get();
                    else if (m_modelZero.IsReady() && p.m_addressMode == m_modelZero->m_addressMode
                        && std::abs(p.m_scale - m_modelZero->m_scale) < 1e-6f && std::abs(p.m_reference - m_modelZero->m_reference) < 1e-6f)
                        modelZero = m_modelZero.Get();
                }
                const bool neuralSeed = requiresTrainedHits || modelZero;
                Image neuralCache;
                if (modelZero && p.m_neuralCache.GetId().IsValid())
                {
                    const auto& cacheDesc = p.m_neuralCache->GetImageDescriptor();
                    if (!modelZero->HasCacheLayout() || cacheDesc.m_size.m_width != 3072 || cacheDesc.m_size.m_height != 1536
                        || cacheDesc.m_size.m_depth != 1 || cacheDesc.m_arraySize != 1 || cacheDesc.m_mipLevels != 1
                        || cacheDesc.m_format != AZ::RHI::Format::R16G16B16A16_FLOAT
                        || desc.m_size.m_width != modelZero->m_width || desc.m_size.m_height != modelZero->m_heightPixels
                        || p.m_tiling != AZ::Vector2(1) || p.m_offset != AZ::Vector2(0))
                    {
                        return fail("Model 0 seed cache requires a 3072 x 1536 single-mip RGBA16F cache, its profile's cache layout and a height image of the profile's size");
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
                    && Set(material,"surface.maxCells",c.m_maxCells)
                    && Set(material,"surface.reliefMode",static_cast<AZ::u32>(c.m_reliefMode)) && Set(material,"surface.useHierarchy",bool(minMax))
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
                    // Wall comparisons do not change the other profiles'
                    // documented physical neural-only visibility route.
                    && Set(material,"surface.lightingEstimator",StrictNeuralPolicy::LightingEstimatorForProfile(
                        p.m_neuralModel,static_cast<AZ::u32>(c.m_lightingEstimator)))
                    && Set(material,"surface.diffuseAccessibility",p.m_neuralModel==4u && c.m_diffuseAccessibility)
                    && Set(material,"surface.neuralDirectionalVisibility",c.m_neuralDirectionalVisibility)
                    && Set(material,"surface.proxyDirectionalResponse",c.m_proxyDirectionalResponse)
                    && Set(material,"surface.neuralShadowConfidence",c.m_neuralShadowConfidence)
                    && Set(material,"surface.useHeightDerivedNormals",c.m_useHeightDerivedNormals)
                    && Set(material,"surface.useNeuralCache",bool(neuralCache))
                    && (!neuralCache || Set(material,"surface.neuralCache",neuralCache))
                    && (!minMax || Set(material,"surface.heightBounds",minMax))
                    && Set(material,"surface.reliefShadowSteps",c.m_reliefShadowSteps)
                    && Set(material,"mesh.paintReceiver",candidate->paintReceiver);
                if(!valid) return fail("Planar face material contract mismatch");
                // The profile's networks (model 0 included): one weights buffer per batch.
                const SurfaceProfileAsset* networkProfile=p.m_neuralModel!=0 && profileAsset!=m_profileAssets.end()
                    ?profileAsset->second.Get():modelZero;
                if(neuralSeed && !networkProfile) return fail("Neural seeding has no surface profile networks for this profile ID");
                if(neuralSeed)
                {
                    const auto& profile=*networkProfile;
                    const auto networks=profile.PackNetworks();
                    auto weights=AZ::RPI::Buffer::FindOrCreate(Buffer(networks.data(),networks.size(),4,true));
                    if(!weights) return fail("Cannot allocate trained profile networks");
                    weights->WaitForUpload();
                    const auto weightIndices=weights->GetBufferView()->GetBindlessReadIndex();
                    if(weightIndices.size()!=1||weightIndices.begin()->second==AZ::u32(-1)) return fail("Trained profiles require one bindless-capable GPU");
                    if(!Set(material,"surface.profileWeights",weightIndices.begin()->second)
                        ||!Set(material,"surface.profileRole",static_cast<AZ::u32>(profile.m_role))
                        ||!Set(material,"surface.profileFlags",profile.NetworkFlags()))
                        return fail("Trained profile material contract mismatch");
                    candidate->buffers.push_back(weights);
                }
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
                    resolveOwnerBase = base;
                    if(proxy)candidate->proxyBatches.back().hitFaceBase=base;
                    for(AZ::u32 i=0;i<faces.size();++i)candidate->paintFaces.push_back({asset.m_faces[faces[i]].m_id,
                        faces[i],aznumeric_cast<AZ::u32>(candidate->handles.size()),i,base+i});
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
            const auto estimatorIndex=material->FindPropertyIndex(AZ::Name("surface.lightingEstimator"));
            candidate->batchPolicies.push_back({key.second!=0,estimatorIndex.IsValid()?material->GetPropertyValue(estimatorIndex).GetValue<AZ::u32>():0u,
                key.second!=0?StrictForwardTag(material):AZ::Name{}});
            candidate->resolveBatches.push_back(resolveOwnerBase
                ? CreateResolveBatch(scene, candidate->paintReceiver, resolveOwnerBase,
                    aznumeric_cast<AZ::u32>(faces.size()), resolveBounds, rigid, AZStd::move(resolveClusters),
                    faceCacheInputs.profileRole, c.m_reliefMode == ReliefMode::Lod4pxResolve)
                : ResolveBatchPtr{});
            if(faceCacheInputs.faces)
            {
                SetResolveBatchFaceCacheInputs(candidate->resolveBatches.back(),faceCacheInputs);
                if(faceCacheInputs.proxyLodEnabled)
                    candidate->proxyBatches.back().resolveBatch=candidate->resolveBatches.back();
            }
            const auto motionTag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("motion"));
            auto handler = AZStd::make_unique<Generation::PacketHandler>(
                [generation = candidate.get(), batchIndex, processor = m_processor, motionTag](
                    const AZ::Render::ModelDataInstanceInterface&, AZ::u32, AZ::u32, const AZ::RPI::MeshDrawPacket& packet)
                {
                    UpdateResolveBatchPacket(generation->resolveBatches[batchIndex], packet);
                    const auto& policy=generation->batchPolicies[batchIndex];
                    const bool strict=generation->strictRequired.load();
                    const bool certified=!strict||CertifyStrictPacket(packet,policy.displaced,policy.lightingEstimator,policy.forwardTag,motionTag);
                    if(!certified)generation->strictCertified.store(false);
                    const bool enable=StrictNeuralPolicy::CanEnable(generation->published.load(),strict,
                        certified&&generation->strictCertified.load());
                    if (const auto* draw = packet.GetRHIDrawPacket())
                        for (size_t i = 0; i < draw->GetDrawItemCount(); ++i)
                        {
                            const auto tag = draw->GetDrawListTag(i);
                            processor->SetDrawItemEnabled(generation->handles[batchIndex], tag,
                                enable && tag != motionTag);
                        }
                    SetResolveBatchPublished(generation->resolveBatches[batchIndex],enable);
                });
            candidate->handles.back()->ConnectMeshDrawPacketUpdatedHandler(*handler);
            candidate->packetHandlers.push_back(AZStd::move(handler));
        }
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
void MeshController::OnBeginPrepareRender(){EnforceStrictGenerationPolicy();UpdateProxyLod();SyncSurfacePaintMaterials();}
void MeshController::OnEndPrepareRender(){EnforceStrictGenerationPolicy(true);}
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
            if(m_uvPaint&&m_uvPaint->original==batch.material&&m_uvPaint->material)
            {
                Set(m_uvPaint->material,"surface.proxyLodBuffer",indices.begin()->second);
                m_uvPaint->material->Compile();
                if(auto srg=m_uvPaint->material->GetShaderResourceGroup())srg->Compile();
            }
            // The backend permits at most three in-flight frames. Keep eight
            // render preparations, in addition to RHI's deferred destruction.
            batch.retired.push_back({m_active->proxyFrame+8,batch.buffer});
            batch.buffer=next;batch.bufferIndex=indices.begin()->second;
            SetResolveBatchProxySelection(batch.resolveBatch,batch.buffer,batch.bufferIndex);
        }
    }
    m_active->proxyState=AZ::Vector3(float(selected),float(eligible),float(maxSelected));
}
void MeshController::OnTick(float, AZ::ScriptTimePoint)
{
    EnforceStrictGenerationPolicy();
    if(!m_surfacePaintRuntime)TickReceiverUvPaint();
    const unsigned policy=UpdateHitSamplePolicy(AZ::RPI::Scene::GetSceneForEntityId(m_entity));
    if(m_hitSamplePolicy!=policy){m_hitSamplePolicy=policy;m_checkHitPasses=HasTrainedRenderingDuty();}
    // Newly loaded surfaces/profiles can name further profile and height assets.
    if (m_profilesStale) { m_profilesStale = false; if (ResolveProfileAssets()) { ConnectAssets(); m_dirty = true; } }
    if (m_dirty) { Release(m_pending); m_dirty = Prepare(); }
    if (m_checkHitPasses && HasTrainedRenderingDuty())
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
        EnforceStrictGenerationPolicy();
        if(m_pending->strictRequired.load()&&!m_pending->strictCertified.load())
        {
            ready=false;
            m_status="Waiting for certified neural-only lighting and trained no-march depth shaders; previous incompatible generation suppressed";
        }
        if (ready)
        {
            // All handles reference private immutable model/buffer assets. Stock
            // source-model reloads cannot replace just the ordinary half.
            if (m_active)
            {
                for (const auto& batch : m_active->resolveBatches) SetResolveBatchPublished(batch, false);
                for (const auto& handle : m_active->handles) m_processor->SetVisible(handle, false);
            }
            m_pending->published.store(true);
            const bool enable=StrictNeuralPolicy::CanEnable(true,m_pending->strictRequired.load(),m_pending->strictCertified.load());
            const auto motionTag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("motion"));
            for (size_t batchIndex = 0; batchIndex < m_pending->handles.size(); ++batchIndex)
            {
                const auto& handle = m_pending->handles[batchIndex];
                for (const auto& lod : m_processor->GetDrawPackets(handle)) for (const auto& packet : lod)
                {
                    UpdateResolveBatchPacket(m_pending->resolveBatches[batchIndex], packet);
                    for (size_t i = 0; i < packet.GetRHIDrawPacket()->GetDrawItemCount(); ++i)
                    {
                        const auto tag = packet.GetRHIDrawPacket()->GetDrawListTag(i);
                        m_processor->SetDrawItemEnabled(handle, tag, enable && tag != motionTag);
                    }
                }
                SetResolveBatchPublished(m_pending->resolveBatches[batchIndex], enable);
            }
            ReleaseSurfacePaintTextures();
            Release(m_active); m_active = AZStd::move(m_pending);
            m_paintSurfaceRevision=++ReceiverSurfaceRevision;
            UpdateSceneSubscription();
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
    TickSurfacePaintTextures();
    UpdateSceneSubscription();
}
}

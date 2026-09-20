// SPDX-License-Identifier: MIT
#include "MeshRender.h"
#include <SilPOM/PreviewSampling.h>
#include <AzCore/Console/IConsole.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/RPIUtils.h>
#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/Pass/ParentPass.h>
#include <Atom/RPI.Public/Pass/RasterPass.h>
#include <Atom/RPI.Public/Pass/PassUtils.h>
#include <Atom/RPI.Public/Shader/Shader.h>
#include <Atom/RPI.Public/Shader/ShaderResourceGroup.h>
#include <Atom/RPI.Reflect/Pass/RasterPassData.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RHI/CommandList.h>
#include <Atom/RHI/DispatchItem.h>
#include <Atom/RHI/FrameGraphInterface.h>
#include <Atom/RHI/FrameGraphAttachmentInterface.h>
#include <Atom/RHI/FrameGraphExecuteContext.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/std/containers/set.h>
#include <cmath>

namespace SilPOM
{
AZ_CVAR(AZ::u32, r_silpomPreviewRayBudget, 0, nullptr, AZ::ConsoleFunctorFlags::Null,
    "Opt-in APPROXIMATE block-sampled displacement preview. 0 = strict full-quality rays; otherwise 32..65536 rays per batch across views.");
namespace
{
using namespace AZ;
using FP = MeshRenderFeatureProcessor;
const Name HitSlot("SilPomComputedHits");
bool Consumer(RPI::RasterPass* pass)
{
    auto tag = pass->GetDrawListTag();
    if (!tag.IsValid()) return false;
    const auto name = RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->GetName(tag).GetStringView();
    return name.find("forward") != AZStd::string_view::npos || name.find("depth") != AZStd::string_view::npos
        || name.find("shadow") != AZStd::string_view::npos;
}
void Visit(RPI::ParentPass* parent, const AZStd::function<void(RPI::RasterPass*)>& callback)
{
    for (const auto& child : parent->GetChildren())
    {
        if (auto raster = azrtti_cast<RPI::RasterPass*>(child.get()); raster && Consumer(raster)) callback(raster);
        if (auto nested = child->AsParent()) Visit(nested, callback);
    }
}
void AttachRead(RPI::RasterPass* pass, const Data::Instance<RPI::Buffer>& buffer)
{
    if (auto binding = pass->FindAttachmentBinding(HitSlot); binding && binding->GetAttachment()) return;
    RPI::PassSlot slot; slot.m_name = HitSlot; slot.m_slotType = RPI::PassSlotType::Input;
    slot.m_scopeAttachmentUsage = RHI::ScopeAttachmentUsage::Shader;
    slot.m_scopeAttachmentStage = RHI::ScopeAttachmentStage::FragmentShader;
    slot.m_shaderInputName = Name("NoBind");
    slot.m_bufferViewDesc = AZStd::make_shared<RHI::BufferViewDescriptor>(buffer->GetBufferView()->GetDescriptor());
    if (!pass->FindAttachmentBinding(HitSlot))
    {
        RPI::PassAttachmentBinding binding(slot);
        // This hook also runs after RasterPass::InitializeInternal, so the
        // human-readable NoBind name alone has not been resolved to an index.
        binding.m_shaderInputIndex = RPI::PassAttachmentBinding::ShaderInputNoBind;
        pass->AddAttachmentBinding(binding);
    }
    pass->AttachBufferToSlot(HitSlot, buffer);
}
struct ViewWork
{
    Matrix4x4 vp, inverse;
    u32 width = 0, height = 0;
    bool shadow = false, supported = true;
    AZStd::string diagnostic;
};
struct Job
{
    AZStd::shared_ptr<MeshRenderBatch> batch;
    ViewWork view;
    u32 rect[4]{}, header = 0, records = 0, views = 0, error = 0;
    u32 sampling[4]{1, 0, 0, 0}; // stride, columns, rows, approximate-preview budget (0 = strict)
};
struct PipelineWork { AZStd::vector<Job> jobs; };
bool Enabled(const RPI::Pass* pass)
{
    for (auto p = pass; p; p = p->GetParent()) if (!p->IsEnabled()) return false;
    return true;
}
// This pass is a root sibling, not a child of the engine's cascade container:
// that container assumes every one of its children is a ShadowmapPass.
class MeshComputePass final : public RPI::RenderPass
{
public:
    AZ_RTTI(MeshComputePass, "{25BEE5E6-9B2E-4089-A806-F0EEC2B39FEA}", RPI::RenderPass);
    AZ_CLASS_ALLOCATOR(MeshComputePass, SystemAllocator);
    MeshComputePass(const RPI::PassDescriptor& desc, FP* owner, AZStd::shared_ptr<PipelineWork> work, bool resolve)
        : RenderPass(desc), m_owner(owner), m_work(AZStd::move(work)), m_resolve(resolve)
    {
        m_shader = RPI::LoadShader(resolve ? "shaders/silpom/curved/resolve.azshader" : "shaders/silpom/curved/intersect.azshader");
        if (m_shader)
        {
            RHI::PipelineStateDescriptorForDispatch pipeline;
            m_shader->GetDefaultVariant().ConfigurePipelineState(pipeline, m_shader->GetDefaultShaderOptions());
            m_pipelineState = m_shader->AcquirePipelineState(pipeline);
        }
    }
    bool IsEnabled() const override { return RenderPass::IsEnabled() && m_owner && m_owner->GetHits() && m_pipelineState; }
    void Detach() { m_owner = nullptr; }
private:
    void BuildInternal() override
    {
        RenderPass::BuildInternal();
        RPI::PassSlot slot; slot.m_name = HitSlot; slot.m_slotType = RPI::PassSlotType::InputOutput;
        slot.m_scopeAttachmentUsage = RHI::ScopeAttachmentUsage::Shader;
        slot.m_scopeAttachmentStage = RHI::ScopeAttachmentStage::ComputeShader;
        slot.m_shaderInputName = Name("NoBind");
        slot.m_bufferViewDesc = AZStd::make_shared<RHI::BufferViewDescriptor>(m_owner->GetHits()->GetBufferView()->GetDescriptor());
        AddAttachmentBinding(RPI::PassAttachmentBinding(slot));
        AttachBufferToSlot(HitSlot,m_owner->GetHits());
    }
    void PrepareWork()
    {
        m_work->jobs.clear();
        AZStd::vector<ViewWork> views;
        Visit(GetRenderPipeline()->GetRootPass().get(), [&](RPI::RasterPass* pass)
        {
            if (!Enabled(pass)) return;
            auto view = pass->GetView(); if (!view) return;
            ViewWork item; item.vp = view->GetWorldToClipMatrixWithOffset(); item.inverse = view->GetClipToWorldMatrixWithOffset();
            item.width = m_fallbackWidth;
            item.height = m_fallbackHeight;
            // A pipeline root may precede the window pass that establishes the
            // inherited viewport. Main camera raster targets expose the real
            // render resolution even in that case (including resize).
            for (const auto& binding : pass->GetAttachmentBindings())
                if (binding.GetAttachment() && (binding.m_scopeAttachmentUsage == RHI::ScopeAttachmentUsage::RenderTarget
                    || binding.m_scopeAttachmentUsage == RHI::ScopeAttachmentUsage::DepthStencil))
                {
                    const auto& size = binding.GetAttachment()->m_descriptor.m_image.m_size;
                    item.width = size.m_width; item.height = size.m_height; break;
                }
            const auto tag = RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->GetName(pass->GetDrawListTag()).GetStringView();
            item.shadow = tag.find("shadow") != AZStd::string_view::npos;
            auto descriptor = pass->GetPassDescriptor();
            if (const auto data = RPI::PassUtils::GetPassData<RPI::RasterPassData>(descriptor))
            {
                if (!data->m_overrideViewport.IsNull())
                {
                    item.width = u32(data->m_overrideViewport.m_maxX-data->m_overrideViewport.m_minX);
                    item.height = u32(data->m_overrideViewport.m_maxY-data->m_overrideViewport.m_minY);
                }
                const int index = data->m_viewportAndScissorTargetOutputIndex;
                if (index >= 0)
                {
                    auto binding = pass->GetOutputCount() > u32(index) ? &pass->GetOutputBinding(index)
                        : pass->GetInputOutputCount() > u32(index) ? &pass->GetInputOutputBinding(index) : nullptr;
                    if (binding && binding->GetAttachment())
                    {
                        const auto& size = binding->GetAttachment()->m_descriptor.m_image.m_size;
                        item.width = size.m_width; item.height = size.m_height;
                    }
                }
            }
            if (item.shadow)
            {
                // Cascades use full array slices. Projected atlas viewports have
                // no public getter in this Atom version; never guess their size.
                item.supported = pass->GetParent() && AZStd::string_view(pass->GetParent()->RTTI_GetTypeName()).find("CascadedShadowmapsPass") != AZStd::string_view::npos;
                if (item.supported && pass->GetOutputCount() && pass->GetOutputBinding(0).GetAttachment())
                {
                    const auto& size = pass->GetOutputBinding(0).GetAttachment()->m_descriptor.m_image.m_size;
                    item.width = size.m_width; item.height = size.m_height;
                }
            }
            item.supported = item.supported && item.width && item.height && pass->GetMultisampleState().m_samples == 1;
            if (!item.supported)
                item.diagnostic = AZStd::string::format("Unsupported view %s (parent %s, %ux%u, samples %u, shadow %u)",
                    pass->GetPathName().GetCStr(),pass->GetParent()?pass->GetParent()->RTTI_GetTypeName():"none",
                    item.width,item.height,pass->GetMultisampleState().m_samples,item.shadow?1:0);
            for (auto& existing : views)
                if (existing.width == item.width && existing.height == item.height && existing.shadow == item.shadow
                    && existing.vp.IsClose(item.vp, 1e-7f))
                {
                    existing.supported = existing.supported && item.supported;
                    if (!item.supported) existing.diagnostic = item.diagnostic;
                    return;
                }
            views.push_back(item);
        });
        for (const auto& weak : m_owner->GetBatches()) if (auto batch = weak.lock(); batch && batch->registered)
        {
            // A later pipeline must not erase an earlier pipeline's failure in
            // this frame and allow an incompletely prepared generation to publish.
            const auto tick = RPI::RPISystemInterface::Get()->GetCurrentTick();
            if (batch->preparedTick != tick)
            {
                batch->preparedTick = tick;
                batch->prepared = false; batch->error.clear();
                batch->previewBudget = 0; batch->previewStride = 1; batch->previewRays = 0;
            }
            if (views.empty()) { batch->error = "No supported raster views"; continue; }
            if (views.size() > FP::MaxViews) batch->error = "View count exceeds staged renderer budget";
            const u32 count = u32(AZStd::min(views.size(), size_t(FP::MaxViews)));
            const size_t firstJob = m_work->jobs.size();
            AZStd::array<SampleExtent, FP::MaxViews> extents{};
            for (u32 i = 0; i < count; ++i)
            {
                Job job; job.batch = batch; job.view = views[i]; job.views = count;
                job.header = batch->slot * FP::BatchBytes + 16 + i * 128;
                Vector2 low(1), high(-1); bool ambiguous = false;
                const auto model = Matrix4x4::CreateFromTransform(batch->rigid);
                for (u32 corner = 0; corner < 8; ++corner)
                {
                    const auto& a = batch->bounds.GetMin(); const auto& b = batch->bounds.GetMax();
                    Vector4 p((corner&1)?b.GetX():a.GetX(),(corner&2)?b.GetY():a.GetY(),(corner&4)?b.GetZ():a.GetZ(),1);
                    p = job.view.vp * (model * p);
                    if (p.GetW() <= 1e-6f || p.GetZ() < 0 || p.GetZ() > p.GetW()) ambiguous = true;
                    Vector2 xy(p.GetX()/AZStd::max(p.GetW(),1e-6f),p.GetY()/AZStd::max(p.GetW(),1e-6f));
                    low = low.GetMin(xy); high = high.GetMax(xy);
                }
                if (ambiguous) { low = Vector2(-1); high = Vector2(1); }
                auto clipPixel = [](float v, u32 size) { return u32(AZStd::clamp(v,0.f,float(size))); };
                const u32 left = clipPixel(std::floor((low.GetX()*.5f+.5f)*job.view.width)-1,job.view.width);
                const u32 right = clipPixel(std::ceil((high.GetX()*.5f+.5f)*job.view.width)+1,job.view.width);
                const u32 top = clipPixel(std::floor((.5f-high.GetY()*.5f)*job.view.height)-1,job.view.height);
                const u32 bottom = clipPixel(std::ceil((.5f-low.GetY()*.5f)*job.view.height)+1,job.view.height);
                job.rect[0]=left; job.rect[1]=top; job.rect[2]=right>=left?right-left:0; job.rect[3]=bottom>=top?bottom-top:0;
                if (!job.view.supported) { job.error = 3; batch->error = job.view.diagnostic; }
                else extents[i] = {job.rect[2], job.rect[3]};
                m_work->jobs.push_back(AZStd::move(job));
            }
            const u32 requested = r_silpomPreviewRayBudget;
            const u32 budget = requested ? AZStd::clamp(requested, FP::MaxViews, FP::MaxRays) : FP::MaxRays;
            const u32 stride = requested ? ChoosePreviewStride(extents.data(), count, budget) : 1;
            u32 used = 0;
            for (u32 i = 0; i < count; ++i)
            {
                auto& job = m_work->jobs[firstJob + i];
                job.records = batch->slot * FP::BatchBytes + FP::HeaderBytes + used * FP::RecordBytes;
                job.sampling[0] = AZStd::max(stride, 1u);
                job.sampling[1] = SampleColumns(job.rect[2], job.sampling[0]);
                job.sampling[2] = SampleColumns(job.rect[3], job.sampling[0]);
                job.sampling[3] = requested ? budget : 0;
                const u64 samples = u64(job.sampling[1]) * job.sampling[2];
                if (!job.error)
                {
                    if (!stride || samples > budget - used)
                    { job.error = 2; batch->error = "Projected ray budget exhausted (65536 samples per batch across views; opt-in r_silpomPreviewRayBudget enables approximate preview)"; }
                    else used += u32(samples);
                }
            }
            batch->previewBudget = requested ? budget : 0;
            batch->previewStride = AZStd::max(batch->previewStride, stride);
            batch->previewRays += used;
            batch->prepared = batch->error.empty();
        }
    }
    void FrameBeginInternal(FramePrepareParams params) override
    {
        m_fallbackWidth = u32(params.m_viewportState.m_maxX - params.m_viewportState.m_minX);
        m_fallbackHeight = u32(params.m_viewportState.m_maxY - params.m_viewportState.m_minY);
        RenderPass::FrameBeginInternal(params);
    }
    void SetupFrameGraphDependencies(RHI::FrameGraphInterface frameGraph) override
    {
        // Atom refreshes the jittered View matrices in Scene::UpdateSrgs AFTER
        // pass FrameBegin, but BEFORE FrameScheduler::PrepareProducers calls
        // this hook. Snapshot here so compute and the raster ViewSrg describe
        // the same frame even while the camera/light moves. Intersect precedes
        // Resolve in scope registration order; both consume this one snapshot.
        if (!m_resolve) PrepareWork();
        while (m_srgs.size() < m_work->jobs.size())
            m_srgs.push_back(RPI::ShaderResourceGroup::Create(m_shader->GetAsset(), Name("PassSrg")));
        RenderPass::SetupFrameGraphDependencies(frameGraph);
        auto database = frameGraph.GetAttachmentDatabase();
        AZStd::set<const RPI::Buffer*> seen;
        for (const auto& job : m_work->jobs) if (seen.insert(job.batch->geometry.get()).second)
        {
            auto buffer = job.batch->geometry;
            // RPI only assigns automatic attachment IDs to writable buffers.
            // Give this immutable input its own identity for explicit tracking.
            const RHI::AttachmentId id(AZStd::string::format("SilPomGeometry_%p",buffer.get()));
            if (!database.IsAttachmentValid(id)) database.ImportBuffer(id, buffer->GetRHIBuffer());
            frameGraph.UseShaderAttachment(RHI::BufferScopeAttachmentDescriptor(id, buffer->GetBufferView()->GetDescriptor()),
                RHI::ScopeAttachmentAccess::Read, RHI::ScopeAttachmentStage::ComputeShader);
            // Height is an immutable streaming asset, like the material textures;
            // it is referenced by the SRG and not written by any render scope.
        }
        frameGraph.SetEstimatedItemCount(u32(m_work->jobs.size()));
    }
    void CompileResources(const RHI::FrameGraphCompileContext&) override
    {
        m_dispatches.clear();
        for (size_t i=0;i<m_work->jobs.size();++i)
        {
            const auto& job = m_work->jobs[i]; const auto& b = *job.batch; auto srg = m_srgs[i];
            auto set = [&](const char* name, const auto& value) { return srg->SetConstant(srg->FindShaderInputConstantIndex(Name(name)), value); };
            bool ok = srg->SetBuffer(srg->FindShaderInputBufferIndex(Name("m_hits")),m_owner->GetHits());
            ok = srg->SetBuffer(srg->FindShaderInputBufferIndex(Name("m_geometry")),b.geometry) && ok;
            // The packed bilinear cell coefficients mean some entrypoints can
            // optimize out the image. Bind only if retained in the SRG layout.
            auto imageIndex = srg->FindShaderInputImageIndex(Name("m_height"));
            if (imageIndex.IsValid()) ok = srg->SetImage(imageIndex,b.height) && ok;
            const auto model = Matrix4x4::CreateFromTransform(b.rigid);
            ok = set("m_viewProjection",job.view.vp) && set("m_clipToWorld",job.view.inverse) && set("m_worldToModel",model.GetInverseFast())
                && set("m_modelToWorld",model) && set("m_surface",b.surface) && set("m_uvTransform",b.uvTransform) && ok;
            auto raw = [&](const char* name,const void* value,u32 bytes) { return srg->SetConstantRaw(srg->FindShaderInputConstantIndex(Name(name)),value,bytes); };
            const u32 limits[] = {b.maxNodes,b.addressMode,job.header,job.records};
            ok = raw("m_counts",b.counts,16) && raw("m_limits",limits,16) && raw("m_rect",job.rect,16)
                && raw("m_sampling",job.sampling,16)
                && set("m_viewport",Vector4(float(job.view.width),float(job.view.height),job.view.shadow?0.f:1.f,float(job.error)))
                && set("m_boundsMin",Vector4(b.bounds.GetMin(),0)) && set("m_boundsMax",Vector4(b.bounds.GetMax(),0))
                && set("m_batchOffset",b.slot*FP::BatchBytes) && set("m_viewCount",job.views) && ok;
            if (!ok) { job.batch->error = "Compute SRG contract mismatch"; job.batch->prepared = false; }
            srg->Compile();
            RHI::DispatchItem dispatch(RHI::MultiDevice::AllDevices); dispatch.SetPipelineState(m_pipelineState);
            RHI::DispatchDirect args; args.m_threadsPerGroupX = 64; args.m_threadsPerGroupY = args.m_threadsPerGroupZ = 1;
            args.m_totalNumberOfThreadsX = job.error?1:AZStd::max(1u,job.sampling[1]*job.sampling[2]);
            args.m_totalNumberOfThreadsY = args.m_totalNumberOfThreadsZ = 1; dispatch.SetArguments(args);
            const RHI::ShaderResourceGroup* groups[] = {srg->GetRHIShaderResourceGroup()}; dispatch.SetShaderResourceGroups(groups);
            m_dispatches.push_back(AZStd::move(dispatch));
        }
    }
    void BuildCommandListInternal(const RHI::FrameGraphExecuteContext& context) override
    {
        for (u32 i=context.GetSubmitRange().m_startIndex;i<context.GetSubmitRange().m_endIndex;++i)
            context.GetCommandList()->Submit(m_dispatches[i].GetDeviceDispatchItem(context.GetDeviceIndex()),i);
    }
    FP* m_owner;
    AZStd::shared_ptr<PipelineWork> m_work;
    bool m_resolve;
    u32 m_fallbackWidth = 0, m_fallbackHeight = 0;
    Data::Instance<RPI::Shader> m_shader;
    const RHI::PipelineState* m_pipelineState = nullptr;
    AZStd::vector<Data::Instance<RPI::ShaderResourceGroup>> m_srgs;
    AZStd::vector<RHI::DispatchItem> m_dispatches;
};
}
void MeshRenderFeatureProcessor::Reflect(AZ::ReflectContext* context)
{
    if (auto sc = azrtti_cast<AZ::SerializeContext*>(context)) sc->Class<MeshRenderFeatureProcessor, AZ::RPI::FeatureProcessor>()->Version(1);
}
void MeshRenderFeatureProcessor::Activate() { EnableSceneNotification(); }
void MeshRenderFeatureProcessor::Deactivate()
{
    DisableSceneNotification();
    for (const auto& pipeline : GetParentScene()->GetRenderPipelines())
        for (const char* name : {"SilPomIntersect", "SilPomResolve"})
            if (auto pass = pipeline->GetRootPass()->FindChildPass(AZ::Name(name)))
            { if (auto compute = azrtti_cast<MeshComputePass*>(pass.get())) compute->Detach(); pass->QueueForRemoval(); }
    m_hits.reset();
}
bool MeshRenderFeatureProcessor::Acquire(const AZStd::shared_ptr<MeshRenderBatch>& batch)
{
    AZ::u32 slot = 0; while (slot < MaxBatches && !m_batches[slot].expired()) ++slot;
    if (slot == MaxBatches) return false;
    if (!m_hits)
    {
        AZ::RPI::BufferAssetCreator creator; creator.Begin(AZ::Uuid::CreateRandom());
        AZ::RHI::BufferDescriptor descriptor;
        descriptor.m_bindFlags = AZ::RHI::BufferBindFlags::ShaderReadWrite | AZ::RHI::BufferBindFlags::CopyRead;
        descriptor.m_byteCount = AZ::u64(MaxBatches)*BatchBytes;
        AZStd::vector<AZ::u8> zero(descriptor.m_byteCount,0);
        creator.SetBuffer(zero.data(),descriptor.m_byteCount,descriptor);
        creator.SetBufferViewDescriptor(AZ::RHI::BufferViewDescriptor::CreateRaw(0,AZ::u32(descriptor.m_byteCount)));
        creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::ReadWrite);
        AZ::Data::Asset<AZ::RPI::BufferAsset> asset; if (!creator.End(asset)) return false;
        m_hits = AZ::RPI::Buffer::FindOrCreate(asset); if (!m_hits) return false;
        m_hits->WaitForUpload();
        if (GetBindlessIndex() == AZ::u32(-1)) { m_hits.reset(); return false; }
    }
    batch->slot = slot; m_batches[slot] = batch;
    for (const auto& pipeline : GetParentScene()->GetRenderPipelines()) AddRenderPasses(pipeline.get());
    return true;
}
AZ::u32 MeshRenderFeatureProcessor::GetBindlessIndex() const
{
    if (!m_hits) return AZ::u32(-1);
    const auto indices = m_hits->GetBufferView()->GetBindlessReadIndex();
    return indices.size()==1?indices.begin()->second:AZ::u32(-1);
}
void MeshRenderFeatureProcessor::AddRenderPasses(AZ::RPI::RenderPipeline* pipeline)
{
    if (!m_hits) return;
    auto root = pipeline->GetRootPass();
    if (!root->FindChildPass(AZ::Name("SilPomIntersect")))
    {
        auto work = AZStd::make_shared<PipelineWork>();
        AZ::RPI::Ptr<MeshComputePass> initial = aznew MeshComputePass(AZ::RPI::PassDescriptor(AZ::Name("SilPomIntersect")),this,work,false);
        AZ::RPI::Ptr<MeshComputePass> resolve = aznew MeshComputePass(AZ::RPI::PassDescriptor(AZ::Name("SilPomResolve")),this,work,true);
        root->InsertChild(initial,0u); root->InsertChild(resolve,1u);
    }
    Visit(root.get(),[&](AZ::RPI::RasterPass* pass) { AttachRead(pass,m_hits); });
}
void MeshRenderFeatureProcessor::OnRenderPipelineChanged(AZ::RPI::RenderPipeline* pipeline, AZ::RPI::SceneNotification::RenderPipelineChangeType change)
{
    if (change != AZ::RPI::SceneNotification::RenderPipelineChangeType::Removed) AddRenderPasses(pipeline);
}
}

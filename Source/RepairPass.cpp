// SPDX-License-Identifier: MIT
#include "RepairPass.h"
#include "ResolvePass.h"
#include <SilPOM/RepairRegion.h>
#include <SilPOM/RepairExecution.h>
#include <Atom/RPI.Public/Pass/FullscreenTrianglePass.h>
#include <Atom/RPI.Public/Pass/PassAttachment.h>
#include <Atom/RPI.Public/Pass/PassSystemInterface.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RHI/FrameGraphInterface.h>
#include <Atom/RHI/FrameGraphExecuteContext.h>
#include <Atom/RHI/FrameGraphCompileContext.h>
#include <Atom/RHI/ImageView.h>
#include <AzCore/Casting/numeric_cast.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/std/containers/array.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <cmath>

namespace SilPOM
{
    AZ_CVAR(bool, r_silpomRepairRegions, true, nullptr, AZ::ConsoleFunctorFlags::Null,
        "Restrict classification and fill to shared conservative disjoint wall-donor rectangles.");
    AZ_CVAR(bool, r_silpomRepairRegionTrace, false, nullptr, AZ::ConsoleFunctorFlags::Null,
        "Log shared repair-plan mode and support without changing repair shaders.");

    namespace
    {
        AZ::u64 RepairTick()
        { return AZ::RPI::RPISystemInterface::Get()->GetCurrentTick(); }
        bool RepairSameViewport(const AZ::RHI::Viewport& a,const AZ::RHI::Viewport& b)
        {
            return a.m_minX==b.m_minX&&a.m_maxX==b.m_maxX&&a.m_minY==b.m_minY&&a.m_maxY==b.m_maxY
                &&a.m_minZ==b.m_minZ&&a.m_maxZ==b.m_maxZ;
        }
        bool RepairSameScissor(const AZ::RHI::Scissor& a,const AZ::RHI::Scissor& b)
        { return a.m_minX==b.m_minX&&a.m_minY==b.m_minY&&a.m_maxX==b.m_maxX&&a.m_maxY==b.m_maxY; }
        bool RepairSameImage(const AZ::RHI::ImageDescriptor& a,const AZ::RHI::ImageDescriptor& b)
        {
            return a.m_size==b.m_size&&a.m_format==b.m_format&&a.m_dimension==b.m_dimension
                &&a.m_arraySize==b.m_arraySize&&a.m_mipLevels==b.m_mipLevels&&a.m_bindFlags==b.m_bindFlags
                &&a.m_sharedQueueMask==b.m_sharedQueueMask&&a.m_isCubemap==b.m_isCubemap
                &&a.m_multisampleState==b.m_multisampleState;
        }
        struct RepairBindingStamp
        {
            AZ::RPI::Ptr<AZ::RPI::PassAttachment> attachment;
            AZ::RHI::ImageDescriptor descriptor;
            AZ::RHI::ImageScopeAttachmentDescriptor scope;
            AZ::RHI::AttachmentId identity;
            AZ::RHI::ScopeAttachmentUsage usage;
            AZ::RHI::ScopeAttachmentAccess access;
            AZ::RHI::ScopeAttachmentStage stage;
            AZ::RHI::AttachmentLifetimeType lifetime;
            AZ::Data::Instance<AZ::Data::InstanceData> imported;
            AZ::Name shaderInput;
            int16_t shaderIndex;
            uint16_t arrayIndex;
            uint8_t usageIndex;
        };
        struct RepairCoverageSnapshot
        {
            AZ::u64 tick=0;
            const AZ::RPI::RenderPipeline* pipeline=nullptr;
            AZ::RPI::ViewPtr view;
            AZ::Matrix4x4 worldToClip=AZ::Matrix4x4::CreateIdentity();
            AZStd::shared_ptr<const RepairProducerInventory> inventory;
            RepairRegion::Plan bounds;
            AZ::RHI::Viewport classifyViewport,fillViewport;
            AZ::RHI::Scissor classifyScissor,fillScissor;
            bool baselineCandidate=false;
        };
        struct PreparedRepairDraw
        {
            AZ::Data::Instance<AZ::RPI::Shader> shader;
            AZ::Data::Instance<AZ::RPI::ShaderResourceGroup> passSrg,drawSrg;
            AZ::RHI::ConstPtr<AZ::RHI::PipelineState> state;
            AZ::RHI::PipelineStateDescriptorForDraw descriptor;
            AZStd::vector<AZ::RHI::ConstPtr<AZ::RHI::ShaderResourceGroup>> srgs;
            AZStd::vector<RepairBindingStamp> bindings;
            AZStd::vector<AZ::RHI::ConstPtr<AZ::RHI::ImageView>> images;
            AZ::RHI::Viewport viewport;
            AZ::RHI::Scissor scissor;
            AZ::RHI::GeometryView geometry{AZ::RHI::MultiDevice::AllDevices};
            AZ::RHI::DrawItem item{AZ::RHI::MultiDevice::AllDevices};
        };
        struct SharedRepairDecision
        {
            AZStd::shared_ptr<const RepairCoverageSnapshot> coverage;
            RepairExecution::Mode mode=RepairExecution::Mode::Unavailable;
            // Copy of the sealed scheduling value; recording never sees the
            // mutable preparation shared by the compile callbacks.
            RepairExecution::Preparation scheduling{RepairRegion::Mode::FullScreenFallback,0};
            AZStd::array<AZStd::shared_ptr<const PreparedRepairDraw>,2> draws;
        };
        struct SharedRepairPreparation
        {
            explicit SharedRepairPreparation(AZStd::shared_ptr<const RepairCoverageSnapshot> snapshot)
                : coverage(AZStd::move(snapshot)),scheduling(coverage->bounds.mode,coverage->bounds.rectangles.size()) {}
            AZStd::shared_ptr<const RepairCoverageSnapshot> coverage;
            RepairExecution::Preparation scheduling;
            AZStd::array<AZStd::vector<RepairBindingStamp>,2> declarations;
            AZStd::array<AZ::RHI::RenderAttachmentConfiguration,2> configurations;
            AZStd::array<AZStd::shared_ptr<PreparedRepairDraw>,2> draws;
        };

        class RepairPass final : public AZ::RPI::FullscreenTrianglePass
        {
            AZ_RPI_PASS(RepairPass);
        public:
            AZ_RTTI(RepairPass,"{B960C7A8-5CB3-46D9-B345-D4105CA3330B}",AZ::RPI::FullscreenTrianglePass);
            AZ_CLASS_ALLOCATOR(RepairPass,AZ::SystemAllocator);
            static AZ::RPI::Ptr<RepairPass> CreateClassify(const AZ::RPI::PassDescriptor& descriptor)
            { return aznew RepairPass(descriptor,true); }
            static AZ::RPI::Ptr<RepairPass> CreateFill(const AZ::RPI::PassDescriptor& descriptor)
            { return aznew RepairPass(descriptor,false); }
        private:
            RepairPass(const AZ::RPI::PassDescriptor& descriptor,bool classify)
                : FullscreenTrianglePass(descriptor),m_classify(classify) {}
            RepairPass* Partner() const
            {
                if(!m_pipeline)return nullptr;
                const auto found=m_pipeline->FindFirstPass(AZ::Name(m_classify?"SilPOMFillWallHoles":"SilPOMClassifyWallHoles"));
                return found?azrtti_cast<RepairPass*>(found.get()):nullptr;
            }
            bool DrawReady() const
            {
                return IsEnabled()&&m_shader&&m_shaderResourceGroup&&m_pipelineStateForDraw.GetRHIPipelineState()
                    &&m_pipelineStateForDraw.ConstDescriptor().m_vertexFunction
                    &&m_pipelineStateForDraw.ConstDescriptor().m_fragmentFunction
                    &&m_shader->GetSupervariantIndex()==m_shader->GetAsset()->GetSupervariantIndex(GetSuperVariantName());
            }
            static const AZ::RPI::PassAttachmentBinding* Binding(const RepairPass& pass,const char* name)
            { return pass.FindAttachmentBinding(AZ::Name(name)); }
            bool CompatibleBindings(const RepairPass& fill) const
            {
                const char* classifyNames[]={"Fill","FillUvGradients","FillDepthGradients","Hit","HitUvGradients","HitDepthGradients","SceneDepth"};
                const char* fillNames[]={"Fill","FillUvGradients","FillDepthGradients","Hit","HitUvGradients","HitDepthGradients","Depth"};
                const auto* reference=Binding(*this,"Fill");
                if(!reference||!reference->GetAttachment())return false;
                const auto& desc=reference->GetAttachment()->m_descriptor.m_image;
                if(!desc.m_size.m_width||!desc.m_size.m_height||!desc.m_multisampleState.m_samples)return false;
                if(!(m_pipelineStateForDraw.ConstDescriptor().m_renderStates.m_multisampleState==desc.m_multisampleState)
                    ||!(fill.m_pipelineStateForDraw.ConstDescriptor().m_renderStates.m_multisampleState==desc.m_multisampleState))return false;
                for(size_t i=0;i<7;++i)
                {
                    const auto* a=Binding(*this,classifyNames[i]);const auto* b=Binding(fill,fillNames[i]);
                    if(!a||!b||!a->GetAttachment()||a->GetAttachment()!=b->GetAttachment()
                        ||a->GetAttachment()->GetAttachmentType()!=AZ::RHI::AttachmentType::Image)return false;
                    const auto& d=a->GetAttachment()->m_descriptor.m_image;
                    if(d.m_size!=desc.m_size||!(d.m_multisampleState==desc.m_multisampleState)
                        ||d.m_dimension!=AZ::RHI::ImageDimension::Image2D||d.m_size.m_depth!=1
                        ||d.m_mipLevels!=1||d.m_arraySize!=1||d.m_isCubemap)return false;
                    const auto& av=a->m_unifiedScopeDesc.GetAsImage().m_imageViewDescriptor;
                    const auto& bv=b->m_unifiedScopeDesc.GetAsImage().m_imageViewDescriptor;
                    if(av.m_mipSliceMin||bv.m_mipSliceMin||av.m_arraySliceMin||bv.m_arraySliceMin
                        ||av.m_depthSliceMin||bv.m_depthSliceMin||av.m_isArray||bv.m_isArray||av.m_isCubemap||bv.m_isCubemap)return false;
                    // Depth shader reads use a depth-only aspect; DSV writes
                    // can include stencil. Color/scratch views must match.
                    if(i!=6&&!(av==bv))return false;
                    const auto shader=AZ::RHI::ScopeAttachmentUsage::Shader;
                    const auto target=AZ::RHI::ScopeAttachmentUsage::RenderTarget;
                    const auto depth=AZ::RHI::ScopeAttachmentUsage::DepthStencil;
                    if(a->m_scopeAttachmentUsage!=(i<3?target:shader)
                        ||b->m_scopeAttachmentUsage!=(i<3?shader:(i==6?depth:target)))return false;
                    if(a->GetAttachmentAccess()!=(i<3?AZ::RHI::ScopeAttachmentAccess::Write:AZ::RHI::ScopeAttachmentAccess::Read)
                        ||b->GetAttachmentAccess()!=(i<3?AZ::RHI::ScopeAttachmentAccess::Read:AZ::RHI::ScopeAttachmentAccess::Write)
                        ||b->m_slotType!=(i<3?AZ::RPI::PassSlotType::Input:AZ::RPI::PassSlotType::InputOutput))return false;
                    const auto& action=a->m_unifiedScopeDesc.GetAsImage().m_loadStoreAction;
                    const auto& fillAction=b->m_unifiedScopeDesc.GetAsImage().m_loadStoreAction;
                    if(i<3&&(action.m_storeAction!=AZ::RHI::AttachmentStoreAction::Store
                        ||(desc.m_multisampleState.m_samples==1&&(action.m_loadAction!=AZ::RHI::AttachmentLoadAction::Clear
                            ||(!(action.m_clearValue==AZ::RHI::ClearValue::CreateVector4Float(0,0,0,0))
                                &&!(action.m_clearValue==AZ::RHI::ClearValue::CreateVector4Uint(0,0,0,0)))))))return false;
                    if(i>=3&&(fillAction.m_loadAction!=AZ::RHI::AttachmentLoadAction::Load
                        ||fillAction.m_storeAction!=AZ::RHI::AttachmentStoreAction::Store))return false;
                }
                return true;
            }
            bool BaselineCompatible(const RepairPass& fill) const
            {
                if(!DrawReady()||!fill.DrawReady()||fill.m_pipeline!=m_pipeline
                    ||static_cast<const AZ::RHI::ScopeProducer&>(*this).GetDeviceIndex()!=static_cast<const AZ::RHI::ScopeProducer&>(fill).GetDeviceIndex()
                    ||GetSuperVariantName()!=fill.GetSuperVariantName()||!CompatibleBindings(fill)
                    ||!PipelineMatchesAttachments()||!fill.PipelineMatchesAttachments())return false;
                const auto& a=m_pipelineStateForDraw.ConstDescriptor().m_renderStates;
                const auto& b=fill.m_pipelineStateForDraw.ConstDescriptor().m_renderStates;
                if(!(a.m_multisampleState==b.m_multisampleState)
                    ||a.m_rasterState.m_forcedSampleCount||b.m_rasterState.m_forcedSampleCount
                    ||a.m_blendState.m_alphaToCoverageEnable||b.m_blendState.m_alphaToCoverageEnable
                    ||a.m_rasterState.m_fillMode!=AZ::RHI::FillMode::Solid
                    ||b.m_rasterState.m_fillMode!=AZ::RHI::FillMode::Solid
                    ||!(a.m_rasterState==b.m_rasterState)
                    ||a.m_depthStencilState.m_depth.m_enable||a.m_depthStencilState.m_stencil.m_enable
                    ||b.m_depthStencilState.m_stencil.m_enable||!b.m_depthStencilState.m_depth.m_enable
                    ||b.m_depthStencilState.m_depth.m_func!=AZ::RHI::ComparisonFunc::GreaterEqual
                    ||b.m_depthStencilState.m_depth.m_writeMask!=AZ::RHI::DepthWriteMask::All)return false;
                for(size_t i=0;i<3;++i)
                    if(a.m_blendState.m_targets[i].m_enable||b.m_blendState.m_targets[i].m_enable
                        ||a.m_blendState.m_targets[i].m_writeMask!=0xf||b.m_blendState.m_targets[i].m_writeMask!=0xf)return false;
                const auto validViewport=[](const AZ::RHI::Viewport& v)
                {
                    return RepairRegion::Internal::ValidViewport({v.m_minX,v.m_minY,v.m_maxX,v.m_maxY})
                        &&std::isfinite(v.m_minZ)&&std::isfinite(v.m_maxZ)&&v.m_minZ>=0&&v.m_maxZ<=1&&v.m_minZ<=v.m_maxZ;
                };
                // Both shaders use SV_Position, so classification may adopt
                // fill's complete domain without changing any donor reads.
                return validViewport(fill.m_viewportState)&&fill.m_scissorState.m_minX<=fill.m_scissorState.m_maxX
                    &&fill.m_scissorState.m_minY<=fill.m_scissorState.m_maxY;
            }
            bool PipelineMatchesAttachments() const
            {
                const auto& config=m_pipelineStateForDraw.ConstDescriptor().m_renderAttachmentConfiguration;
                uint32_t target=0;
                auto depthFormat=AZ::RHI::Format::Unknown;
                for(const auto& binding:m_attachmentBindings)
                {
                    if(!binding.GetAttachment())return false;
                    const auto& view=binding.m_unifiedScopeDesc.GetAsImage().m_imageViewDescriptor;
                    const auto format=view.m_overrideFormat==AZ::RHI::Format::Unknown?
                        binding.GetAttachment()->m_descriptor.m_image.m_format:view.m_overrideFormat;
                    if(binding.m_scopeAttachmentUsage==AZ::RHI::ScopeAttachmentUsage::Shader
                        &&!AZ::RHI::CheckBitsAll(binding.m_scopeAttachmentStage,AZ::RHI::ScopeAttachmentStage::FragmentShader))return false;
                    if(binding.m_scopeAttachmentUsage==AZ::RHI::ScopeAttachmentUsage::RenderTarget)
                    {
                        if(target>=config.GetRenderTargetCount()||config.GetRenderTargetFormat(target++)!=format)return false;
                    }
                    if(binding.m_scopeAttachmentUsage==AZ::RHI::ScopeAttachmentUsage::DepthStencil)depthFormat=format;
                }
                return target==3&&config.GetRenderTargetCount()==target&&config.GetDepthStencilFormat()==depthFormat;
            }
            void QueuePairRebuild()
            {
                QueueForBuildAndInitialization();
                if(auto* other=Partner())other->QueueForBuildAndInitialization();
            }
            // Shader replacement must not mutate the selected frame while
            // either scope may still record. Apply reloads at initialization.
            bool DeferShaderReload()
            {
                if(GetPassState()!=AZ::RPI::PassState::Rendering)return false;
                m_reloadPending=true;
                QueuePairRebuild();
                return true;
            }
            void OnShaderReinitialized(const AZ::RPI::Shader& shader) override
            { if(!DeferShaderReload())FullscreenTrianglePass::OnShaderReinitialized(shader); }
            void OnShaderAssetReinitialized(const AZ::Data::Asset<AZ::RPI::ShaderAsset>& shader) override
            { if(!DeferShaderReload())FullscreenTrianglePass::OnShaderAssetReinitialized(shader); }
            void OnShaderVariantReinitialized(const AZ::RPI::ShaderVariant& shader) override
            { if(!DeferShaderReload())FullscreenTrianglePass::OnShaderVariantReinitialized(shader); }
            void InitializeInternal() override
            {
                if(m_reloadPending||!m_shader)
                {
                    m_reloadPending=false;
                    // The base notification handlers all reload from the
                    // pass descriptor; no callback argument is consulted.
                    // A failed initial load has no notification subscription,
                    // so paired reconstruction must explicitly retry it.
                    FullscreenTrianglePass::OnShaderAssetReinitialized({});
                }
                if(m_shader&&m_disabledForMissingShader)
                {
                    SetEnabled(true);
                    m_disabledForMissingShader=false;
                }
                const bool wasEnabled=IsEnabled();
                FullscreenTrianglePass::InitializeInternal();
                if(!m_shader&&wasEnabled)m_disabledForMissingShader=true;
            }
            void FrameBeginInternal(FramePrepareParams params) override
            {
                if(m_classify)m_retired[m_retireIndex++%m_retired.size()]=AZStd::move(m_decision);
                else m_decision.reset();
                m_preparation.reset();
                FullscreenTrianglePass::FrameBeginInternal(params);
            }
            void BuildPlan()
            {
                auto plan=AZStd::make_shared<RepairCoverageSnapshot>();
                plan->tick=RepairTick();plan->pipeline=m_pipeline;
                plan->classifyViewport=m_viewportState;plan->classifyScissor=m_scissorState;
                plan->bounds.mode=RepairRegion::Mode::FullScreenFallback;
                auto* fill=Partner();
                if(fill)
                {
                    plan->fillViewport=fill->m_viewportState;plan->fillScissor=fill->m_scissorState;
                    plan->baselineCandidate=BaselineCompatible(*fill)
                        &&RepairSameViewport(m_viewportState,fill->m_viewportState)
                        &&RepairSameScissor(m_scissorState,fill->m_scissorState);
                }
                const auto depth=m_pipeline->FindFirstPass(AZ::Name("SilPOMHitDepth"));
                if(plan->baselineCandidate&&depth&&depth->IsEnabled()&&r_silpomRepairRegions)
                {
                    const auto& views=m_pipeline->GetViews(depth->GetPipelineViewTag());
                    if(views.size()==1)
                    {
                        plan->view=views.front();
                        plan->inventory=FindRepairProducerInventory(m_pipeline,plan->view.get());
                        const auto* output=Binding(*this,"Fill");
                        const auto& size=output->GetAttachment()->m_descriptor.m_image.m_size;
                        const auto& inventory=plan->inventory;
                        if(inventory&&inventory->complete&&inventory->tick==plan->tick
                            &&inventory->pipeline==m_pipeline&&inventory->view==plan->view.get()
                            &&inventory->width==size.m_width&&inventory->height==size.m_height
                            &&inventory->sampleCount==output->GetAttachment()->m_descriptor.m_image.m_multisampleState.m_samples
                            &&inventory->hitAttachment==Binding(*this,"Hit")->GetAttachment().get()
                            &&inventory->uvGradientsAttachment==Binding(*this,"HitUvGradients")->GetAttachment().get()
                            &&inventory->depthGradientsAttachment==Binding(*this,"HitDepthGradients")->GetAttachment().get()
                            &&inventory->depthAttachment==Binding(*this,"SceneDepth")->GetAttachment().get())
                        {
                            RepairRegion::PlanBuilder builder(
                                {m_viewportState.m_minX,m_viewportState.m_minY,m_viewportState.m_maxX,m_viewportState.m_maxY},
                                {m_scissorState.m_minX,m_scissorState.m_minY,m_scissorState.m_maxX,m_scissorState.m_maxY},size.m_width,size.m_height);
                            plan->worldToClip=plan->view->GetWorldToClipMatrixWithOffset();
                            const auto& vp=plan->worldToClip;
                            for(const auto& producer:inventory->producers)
                            {
                                if(!producer.localBounds.IsValid()){builder.RequireFullScreen();break;}
                                const auto model=AZ::Matrix4x4::CreateFromTransform(producer.world);
                                double magnitudeMatrix[4][4]={};
                                for(int r=0;r<4;++r)for(int c=0;c<4;++c)for(int k=0;k<4;++k)
                                    magnitudeMatrix[r][c]+=std::abs(double(vp.GetElement(r,k)))*std::abs(double(model.GetElement(k,c)));
                                const auto addBounds=[&](const AZ::Aabb& bounds)
                                {
                                    if(!bounds.IsValid()){builder.RequireFullScreen();return;}
                                    std::array<ResolveRegion::Clip,8> corners,errors;
                                    const auto lo=bounds.GetMin(),hi=bounds.GetMax();
                                    for(size_t i=0;i<8;++i)
                                    {
                                        const AZ::Vector4 local((i&1)?hi.GetX():lo.GetX(),(i&2)?hi.GetY():lo.GetY(),(i&4)?hi.GetZ():lo.GetZ(),1);
                                        const auto clip=vp*(model*local);
                                        corners[i]={clip.GetX(),clip.GetY(),clip.GetZ(),clip.GetW()};
                                        double e[4]={};
                                        for(int r=0;r<4;++r)
                                        {
                                            for(int c=0;c<4;++c)e[r]+=magnitudeMatrix[r][c]*std::abs(double(local.GetElement(c)));
                                            e[r]=e[r]*(1.0/262144.0)+1e-7;
                                        }
                                        errors[i]={e[0],e[1],e[2],e[3]};
                                    }
                                    const auto& v=inventory->viewport;const auto& s=producer.scissor;
                                    builder.AddProducer(corners,{v.m_minX,v.m_minY,v.m_maxX,v.m_maxY},
                                        {s.m_minX,s.m_minY,s.m_maxX,s.m_maxY},errors);
                                };
                                // Published clusters partition the complete batch's
                                // producer bounds. Missing clusters retain its AABB.
                                if(producer.coverageClusters&&!producer.coverageClusters->empty())
                                    for(const auto& cluster:*producer.coverageClusters)addBounds(cluster);
                                else addBounds(producer.localBounds);
                            }
                            plan->bounds=builder.Finish();
                        }
                    }
                }
                m_preparation=AZStd::make_shared<SharedRepairPreparation>(AZStd::move(plan));
                if(fill)fill->m_preparation=m_preparation;
            }
            void SetupFrameGraphDependencies(AZ::RHI::FrameGraphInterface frameGraph) override
            {
                // Keep original attachment declarations, clears and DontCare
                // scopes even when there will be no draw.
                FullscreenTrianglePass::SetupFrameGraphDependencies(frameGraph);
                if(m_classify)BuildPlan();
                if(const auto* other=Partner();!other||!other->IsEnabled())QueuePairRebuild();
                if(m_preparation)
                {
                    const size_t side=m_classify?0:1;
                    CaptureDeclarations(m_preparation->declarations[side]);
                    m_preparation->configurations[side]=m_pipelineStateForDraw.ConstDescriptor().m_renderAttachmentConfiguration;
                    // Execution groups are built before CompileResources.
                    // Even candidate no-work must leave room for fallback.
                    frameGraph.SetEstimatedItemCount(aznumeric_cast<AZ::u32>(m_preparation->scheduling.ReservedItemCount()));
                }
            }
            bool CaptureDeclarations(AZStd::vector<RepairBindingStamp>& result) const
            {
                result.clear();
                if(m_attachmentBindings.size()!=7)return false;
                for(const auto& binding:m_attachmentBindings)
                {
                    const auto& attachment=binding.GetAttachment();
                    if(!attachment||attachment->GetAttachmentType()!=AZ::RHI::AttachmentType::Image
                        ||binding.m_unifiedScopeDesc.GetType()!=AZ::RHI::AttachmentType::Image)
                    { result.clear();return false; }
                    result.push_back({attachment,attachment->m_descriptor.m_image,binding.m_unifiedScopeDesc.GetAsImage(),
                        attachment->GetAttachmentId(),binding.m_scopeAttachmentUsage,binding.GetAttachmentAccess(),
                        binding.m_scopeAttachmentStage,attachment->m_lifetime,attachment->m_importedResource,
                        binding.m_shaderInputName,binding.m_shaderInputIndex,binding.m_shaderInputArrayIndex,binding.m_attachmentUsageIndex});
                }
                return true;
            }
            bool DeclarationsMatch(const AZStd::vector<RepairBindingStamp>& declared) const
            {
                AZStd::vector<RepairBindingStamp> current;
                if(!CaptureDeclarations(current)||current.size()!=declared.size())return false;
                for(size_t i=0;i<current.size();++i)
                {
                    const auto& a=current[i];const auto& b=declared[i];
                    if(a.attachment!=b.attachment||a.identity!=b.identity||!RepairSameImage(a.descriptor,b.descriptor)
                        ||!(a.scope.m_imageViewDescriptor==b.scope.m_imageViewDescriptor)
                        ||a.scope.m_attachmentId!=b.scope.m_attachmentId||!(a.scope.m_loadStoreAction==b.scope.m_loadStoreAction)
                        ||a.usage!=b.usage||a.access!=b.access||a.stage!=b.stage||a.lifetime!=b.lifetime||a.imported!=b.imported
                        ||a.shaderInput!=b.shaderInput||a.shaderIndex!=b.shaderIndex||a.arrayIndex!=b.arrayIndex||a.usageIndex!=b.usageIndex)return false;
                }
                return true;
            }
            AZStd::shared_ptr<PreparedRepairDraw> PrepareDraw(const AZ::RHI::FrameGraphCompileContext& context)
            {
                const size_t side=m_classify?0:1;
                const auto& declared=m_preparation->declarations[side];
                if(!DrawReady()||!DeclarationsMatch(declared)
                    ||!(m_pipelineStateForDraw.ConstDescriptor().m_renderAttachmentConfiguration==m_preparation->configurations[side]))return {};
                auto draw=AZStd::make_shared<PreparedRepairDraw>();
                draw->bindings=declared;
                for(const auto& binding:declared)
                {
                    if(!context.IsAttachmentValid(binding.identity)
                        ||!RepairSameImage(context.GetImageDescriptor(binding.identity),binding.descriptor))return {};
                    const auto* image=context.GetImageView(binding.identity,binding.scope.m_imageViewDescriptor,binding.usage);
                    if(!image||image->GetImage()!=context.GetImage(binding.identity))return {};
                    draw->images.push_back(image);
                }
                // Use the original compilation/binding path, only after the
                // compiled graph is known to describe these exact resources.
                FullscreenTrianglePass::CompileResources(context);
                for(size_t i=0;i<declared.size();++i)
                {
                    const auto& binding=declared[i];
                    if(binding.usage!=AZ::RHI::ScopeAttachmentUsage::Shader)continue;
                    const auto index=m_shaderResourceGroup->FindShaderInputImageIndex(binding.shaderInput);
                    // BindPassSrg can skip an invalid input or ignore a failed
                    // SetImageView. That does not establish a usable baseline.
                    if(!index.IsValid()||m_shaderResourceGroup->GetImageView(index,binding.arrayIndex)!=draw->images[i].get())return {};
                }
                draw->shader=m_shader;draw->passSrg=m_shaderResourceGroup;draw->drawSrg=m_drawShaderResourceGroup;
                draw->state=m_pipelineStateForDraw.GetRHIPipelineState();
                draw->descriptor=m_pipelineStateForDraw.ConstDescriptor();
                draw->viewport=m_viewportState;draw->scissor=m_scissorState;
                CaptureSrgs(draw->srgs);
                draw->geometry.SetDrawArguments(AZ::RHI::DrawLinear(3,0));
                draw->item.SetGeometryView(&draw->geometry);
                draw->item.SetPipelineState(draw->state.get());
                draw->item.SetStencilRef(aznumeric_cast<uint8_t>(m_stencilRef));
                return draw;
            }
            void CaptureSrgs(AZStd::vector<AZ::RHI::ConstPtr<AZ::RHI::ShaderResourceGroup>>& srgs) const
            {
                srgs.clear();
                const auto add=[&](const AZ::RHI::ShaderResourceGroup* srg)
                {
                    if(!srg)return;
                    // Match RenderPass::CollectSrgs and the fullscreen draw
                    // SRG, including the last binding at each slot.
                    const auto found=AZStd::find_if(srgs.begin(),srgs.end(),
                        [&](const auto& bound){return bound->GetBindingSlot()==srg->GetBindingSlot();});
                    if(found==srgs.end())srgs.push_back(srg);else *found=srg;
                };
                if(const auto* scene=m_pipeline->GetScene())add(scene->GetRHIShaderResourceGroup());
                if(BindViewSrg())if(const auto view=GetView())add(view->GetRHIShaderResourceGroup());
                if(m_shaderResourceGroup)add(m_shaderResourceGroup->GetRHIShaderResourceGroup());
                if(m_drawShaderResourceGroup)add(m_drawShaderResourceGroup->GetRHIShaderResourceGroup());
            }
            bool PreparedDrawStillCurrent(const PreparedRepairDraw& draw) const
            {
                if(!DrawReady()||draw.shader!=m_shader||draw.passSrg!=m_shaderResourceGroup||draw.drawSrg!=m_drawShaderResourceGroup
                    ||draw.state.get()!=m_pipelineStateForDraw.GetRHIPipelineState()
                    ||!(draw.descriptor==m_pipelineStateForDraw.ConstDescriptor())||!DeclarationsMatch(draw.bindings))return false;
                AZStd::vector<AZ::RHI::ConstPtr<AZ::RHI::ShaderResourceGroup>> current;
                CaptureSrgs(current);
                return draw.srgs==current;
            }
            bool CoverageStillCurrent(const RepairPass& fill) const
            {
                const auto& plan=*m_preparation->coverage;
                if(plan.bounds.mode==RepairRegion::Mode::FullScreenFallback)return false;
                if(!r_silpomRepairRegions||!plan.inventory||!plan.inventory->complete||plan.tick!=RepairTick()
                    ||plan.pipeline!=m_pipeline||!plan.view
                    ||!RepairSameViewport(m_viewportState,plan.classifyViewport)||!RepairSameScissor(m_scissorState,plan.classifyScissor)
                    ||!RepairSameViewport(fill.m_viewportState,plan.fillViewport)||!RepairSameScissor(fill.m_scissorState,plan.fillScissor))return false;
                const auto depth=m_pipeline->FindFirstPass(AZ::Name("SilPOMHitDepth"));
                if(!depth||!depth->IsEnabled())return false;
                const auto& views=m_pipeline->GetViews(depth->GetPipelineViewTag());
                if(views.size()!=1||views.front()!=plan.view
                    ||FindRepairProducerInventory(m_pipeline,plan.view.get())!=plan.inventory)return false;
                const auto& current=plan.view->GetWorldToClipMatrixWithOffset();
                for(int r=0;r<4;++r)for(int c=0;c<4;++c)
                    if(current.GetElement(r,c)!=plan.worldToClip.GetElement(r,c))return false;
                return true;
            }
            void FinalizePair()
            {
                auto& preparation=*m_preparation;
                if(!preparation.scheduling.BothPrepared()||preparation.scheduling.IsSealed())return;
                auto* other=Partner();
                auto* classify=m_classify?this:other;auto* fill=m_classify?other:this;
                bool baseline=classify&&fill&&other->m_preparation==m_preparation
                    &&preparation.coverage->tick==RepairTick()&&preparation.coverage->pipeline==m_pipeline
                    &&preparation.draws[0]&&preparation.draws[1]
                    &&classify->BaselineCompatible(*fill)
                    &&classify->PreparedDrawStillCurrent(*preparation.draws[0])&&fill->PreparedDrawStillCurrent(*preparation.draws[1]);
                // A late attachment replacement cannot be fixed with a new
                // pointer snapshot: its lifetimes/barriers belong to this graph.
                if(baseline)
                {
                    const auto& a=preparation.draws[0]->images;const auto& b=preparation.draws[1]->images;
                    for(const auto& image:a)
                        if(AZStd::none_of(b.begin(),b.end(),[&](const auto& otherImage){return image->GetImage()==otherImage->GetImage();}))baseline=false;
                }
                const bool coverage=baseline&&classify->CoverageStillCurrent(*fill);
                preparation.scheduling.Seal(baseline,coverage);
                auto decision=AZStd::make_shared<SharedRepairDecision>();
                decision->coverage=preparation.coverage;
                decision->scheduling=preparation.scheduling;
                decision->mode=preparation.scheduling.GetMode();
                if(baseline)
                {
                    // Keep fill's intended domain. SV_Position/global loads
                    // allow classification over precisely the same samples,
                    // including fractional viewport edges and custom MSAA.
                    preparation.draws[0]->viewport=fill->m_viewportState;
                    preparation.draws[0]->scissor=fill->m_scissorState;
                    preparation.draws[1]->viewport=fill->m_viewportState;
                    preparation.draws[1]->scissor=fill->m_scissorState;
                }
                decision->draws={preparation.draws[0],preparation.draws[1]};
                m_decision=decision;
                if(other&&other->m_preparation==m_preparation)other->m_decision=decision;
                if(!baseline)
                {
                    QueuePairRebuild();
                }
                if(r_silpomRepairRegionTrace)
                {
                    const auto& plan=*decision->coverage;
                    AZ::u64 pixels=0;
                    if(decision->mode==RepairExecution::Mode::Regional)
                        for(const auto& r:plan.bounds.rectangles)pixels+=AZ::u64(r.maxX-r.minX)*AZ::u64(r.maxY-r.minY);
                    const auto mode=decision->mode==RepairExecution::Mode::Regional?RepairRegion::Mode::Regional:
                        decision->mode==RepairExecution::Mode::NoWork?RepairRegion::Mode::NoWork:RepairRegion::Mode::FullScreenFallback;
                    AZ_Printf("SilPOM","RepairRegion tick=%llu mode=%u paired=%u producers=%zu rectangles=%zu scheduledPixels=%llu\n",
                        static_cast<unsigned long long>(plan.tick),unsigned(mode),unsigned(baseline),
                        plan.inventory?plan.inventory->producers.size():0,decision->scheduling.DrawCount(),static_cast<unsigned long long>(pixels));
                }
            }
            void CompileResources(const AZ::RHI::FrameGraphCompileContext& context) override
            {
                const auto* other=Partner();
                if(!m_preparation||!other||!other->IsEnabled()||other->m_preparation!=m_preparation)
                { QueuePairRebuild();return; }
                if(m_preparation->scheduling.IsSealed())return;
                m_preparation->draws[m_classify?0:1]=PrepareDraw(context);
                m_preparation->scheduling.Prepared(m_classify);
                // Compile callbacks are serial. Only the second completion
                // seals/publishes the immutable decision used by both scopes.
                FinalizePair();
            }
            void BuildCommandListInternal(const AZ::RHI::FrameGraphExecuteContext& context) override
            {
                const auto& decision=m_decision;
                if(!decision)return;
                const auto range=context.GetSubmitRange();
                const auto end=AZStd::min(range.m_endIndex,aznumeric_cast<AZ::u32>(decision->scheduling.DrawCount()));
                if(!decision->scheduling.Submits(range.m_startIndex,range.m_startIndex,end))return;
                const auto& draw=*decision->draws[m_classify?0:1];
                auto* commands=context.GetCommandList();
                for(const auto& srg:draw.srgs)
                    commands->SetShaderResourceGroupForDraw(*srg->GetDeviceShaderResourceGroup(context.GetDeviceIndex()));
                commands->SetViewport(draw.viewport);
                // Each pass keeps one attachment scope. All classifier draws
                // complete before the dependent fill scope starts.
                for(AZ::u32 i=range.m_startIndex;i<end;++i)
                {
                    if(decision->mode==RepairExecution::Mode::Regional)
                    {
                        const auto& r=decision->coverage->bounds.rectangles[i];
                        commands->SetScissor(AZ::RHI::Scissor(r.minX,r.minY,r.maxX,r.maxY));
                    }
                    else commands->SetScissor(draw.scissor);
                    commands->Submit(draw.item.GetDeviceDrawItem(context.GetDeviceIndex()),i);
                }
            }
            bool m_classify=false,m_reloadPending=false,m_disabledForMissingShader=false;
            AZStd::shared_ptr<SharedRepairPreparation> m_preparation;
            AZStd::shared_ptr<const SharedRepairDecision> m_decision;
            AZStd::array<AZStd::shared_ptr<const SharedRepairDecision>,8> m_retired;
            size_t m_retireIndex=0;
        };
    }
    void RegisterRepairPassCreators()
    {
        auto* system=AZ::RPI::PassSystemInterface::Get();
        static AZ::RPI::PassSystemInterface* registered=nullptr;
        if(system&&system!=registered)
        {
            system->AddPassCreator(AZ::Name("SilPOMRepairClassifyPass"),&RepairPass::CreateClassify);
            system->AddPassCreator(AZ::Name("SilPOMRepairFillPass"),&RepairPass::CreateFill);
            registered=system;
        }
    }
}

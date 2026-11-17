// SPDX-License-Identifier: MIT
#include "ResolvePass.h"
#include "RepairPass.h"
#include <SilPOM/ResolveRegion.h>
#include <SilPOM/ResolveRegions.h>

#include <Atom/RHI/Buffer.h>
#include <Atom/RHI/DevicePipelineState.h>
#include <Atom/RHI/DeviceShaderResourceGroup.h>
#include <Atom/RHI/DrawPacketBuilder.h>
#include <Atom/RHI/CommandList.h>
#include <Atom/RHI/FrameGraphCompileContext.h>
#include <Atom/RHI/FrameGraphExecuteContext.h>
#include <Atom/RHI/FrameGraphInterface.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <Atom/RPI.Public/Buffer/BufferSystemInterface.h>
#include <Atom/RPI.Public/MeshDrawPacket.h>
#include <Atom/RPI.Public/Pass/PassSystemInterface.h>
#include <Atom/RPI.Public/Pass/ComputePass.h>
#include <Atom/RPI.Public/Pass/RasterPass.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Public/Shader/ShaderResourceGroup.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/Debug/Trace.h>
#include <AzCore/Math/Matrix4x4.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/containers/array.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/parallel/lock.h>
#include <AzCore/std/parallel/mutex.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/smart_ptr/weak_ptr.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace SilPOM
{
    AZ_CVAR(bool, r_silpomResolveRegions, true, nullptr, AZ::ConsoleFunctorFlags::Null,
        "Restrict tower 4px batch resolves to conservative screen regions; false retains the reference draws.");
    AZ_CVAR(bool, r_silpomResolveRegionTrace, false, nullptr, AZ::ConsoleFunctorFlags::Null,
        "Log per-pass resolve replacement counts and conservative scheduled pixel area for validation.");
    AZ_CVAR(bool, r_silpomResolveMultiRect, true, nullptr, AZ::ConsoleFunctorFlags::Null,
        "Use disjoint regions from face clusters; false retains one conservative rectangle per batch.");
    AZ_CVAR(bool, r_silpomFaceCache, true, nullptr, AZ::ConsoleFunctorFlags::Null,
        "Prepare immutable face coefficients and current-view implicit depth proxies on the GPU; false retains the reference expressions.");
    AZ_CVAR(bool, r_silpomFaceCacheTrace, false, nullptr, AZ::ConsoleFunctorFlags::Null,
        "Periodically log current-view face-cache dispatch and generation/input availability.");

    namespace
    {
        struct FrameFaceCache;
        struct PacketSnapshot
        {
            explicit PacketSnapshot(const AZ::RPI::MeshDrawPacket& source)
                : packet(source.GetRHIDrawPacket()), geometry(source.GetMesh()), meshIdentity(&source.GetMesh()),
                  material(source.GetMaterial()), shaders(source.GetActiveShaderList())
            {
                // Do not copy MeshDrawPacket: its shader-variant event handler
                // captures the source packet's this pointer and reconnects when
                // copied. Retain only rendering resources, never its callbacks.
                const auto* indexBuffer = geometry.GetIndexBufferView().GetBuffer();
                if (indexBuffer) buffers.emplace_back(indexBuffer);
                for (const auto& stream : geometry.GetStreamBufferViews())
                    if (stream.GetBuffer()) buffers.emplace_back(stream.GetBuffer());
                shaderProductPaths.reserve(shaders.size());
                for (const auto& active : shaders)
                {
                    AZStd::string path;
                    if (active.m_shader)
                        AZ::Data::AssetCatalogRequestBus::BroadcastResult(path,
                            &AZ::Data::AssetCatalogRequests::GetAssetPathById, active.m_shader->GetAsset().GetId());
                    for (auto& c : path)
                    {
                        if (c == '\\') c = '/';
                        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
                    }
                    shaderProductPaths.push_back(AZStd::move(path));
                }
                if (!indexBuffer) return;
                auto& sourceMesh = const_cast<AZ::RPI::ModelLod::Mesh&>(source.GetMesh());
                AZ::RHI::MultiDeviceObject::IterateDevices(indexBuffer->GetDeviceMask(), [&](int deviceIndex)
                {
                    sourceGeometryViews.emplace(deviceIndex, sourceMesh.GetDeviceGeometryView(deviceIndex));
                    for (size_t i = 0; i < packet->GetDrawItemCount(); ++i)
                    {
                        const auto& item = packet->GetDrawItem(i)->GetDeviceDrawItem(deviceIndex);
                        if (item.m_pipelineState) pipelineStates.emplace_back(item.m_pipelineState);
                        for (size_t j = 0; j < item.m_shaderResourceGroupCount; ++j)
                            if (item.m_shaderResourceGroups[j]) shaderResourceGroups.emplace_back(item.m_shaderResourceGroups[j]);
                        if (item.m_uniqueShaderResourceGroup) shaderResourceGroups.emplace_back(item.m_uniqueShaderResourceGroup);
                    }
                    return true;
                });
                for (size_t i = 0; i < packet->GetDrawItemCount(); ++i)
                    if (const auto& srg = source.GetDrawSrg(aznumeric_cast<AZ::u32>(i)); srg) drawSrgs.push_back(srg);
            }

            AZ::RHI::ConstPtr<AZ::RHI::DrawPacket> packet;
            AZ::RHI::GeometryView geometry;
            const AZ::RPI::ModelLod::Mesh* meshIdentity;
            AZ::Data::Instance<AZ::RPI::Material> material;
            AZ::RPI::MeshDrawPacket::ShaderList shaders;
            AZStd::vector<AZStd::string> shaderProductPaths;
            AZStd::vector<AZ::RHI::ConstPtr<AZ::RHI::Buffer>> buffers;
            AZStd::vector<AZ::RHI::ConstPtr<AZ::RHI::DeviceShaderResourceGroup>> shaderResourceGroups;
            AZStd::vector<AZ::RHI::ConstPtr<AZ::RHI::DevicePipelineState>> pipelineStates;
            AZStd::vector<AZ::Data::Instance<AZ::RPI::ShaderResourceGroup>> drawSrgs;
            AZStd::unordered_map<int, const AZ::RHI::DeviceGeometryView*> sourceGeometryViews;
        };

        struct RegistryEntry
        {
            AZStd::weak_ptr<ResolveBatch> batch;
            AZStd::shared_ptr<const PacketSnapshot> snapshot;
            size_t itemIndex = 0;
        };

        struct Registry
        {
            AZStd::mutex mutex;
            AZStd::unordered_map<const AZ::RHI::DrawItem*, RegistryEntry> items;
            AZStd::unordered_map<const AZ::RPI::RenderPipeline*, AZStd::weak_ptr<FrameFaceCache>> faceCaches;
            AZStd::unordered_map<const AZ::RPI::RenderPipeline*, AZStd::weak_ptr<const RepairProducerInventory>> repairInventories;
        };

        Registry& GetRegistry()
        {
            static Registry registry;
            return registry;
        }
    }

    class ResolveBatch
    {
    public:
        ~ResolveBatch()
        {
            auto& registry = GetRegistry();
            AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
            for (const auto& [mesh, keys] : packetKeys)
            {
                (void)mesh;
                for (const auto* key : keys) registry.items.erase(key);
            }
        }

        AZ::RPI::Scene* scene = nullptr;
        AZ::u64 generation = 0;
        AZ::u32 ownerBase = 0, ownerCount = 0;
        AZ::u32 neuralModel = 0;
        bool resolveEligible = false;
        AZ::Aabb localBounds = AZ::Aabb::CreateNull();
        AZStd::shared_ptr<const AZStd::vector<AZ::Aabb>> coverageClusters;
        AZ::Transform world = AZ::Transform::CreateIdentity();
        bool published = false;
        FaceCacheBatchInputs faceCacheInputs;
        AZStd::unordered_map<const AZ::RPI::ModelLod::Mesh*, AZStd::vector<const AZ::RHI::DrawItem*>> packetKeys;
    };

    ResolveBatchPtr CreateResolveBatch(
        AZ::RPI::Scene* scene, AZ::u64 generation, AZ::u32 ownerBase, AZ::u32 ownerCount,
        const AZ::Aabb& localBounds, const AZ::Transform& world, AZStd::vector<AZ::Aabb> coverageClusters,
        AZ::u32 neuralModel, bool resolveEligible)
    {
        auto batch = AZStd::make_shared<ResolveBatch>();
        batch->scene = scene;
        batch->generation = generation;
        batch->ownerBase = ownerBase;
        batch->ownerCount = ownerCount;
        batch->neuralModel = neuralModel;
        batch->resolveEligible = resolveEligible;
        batch->localBounds = localBounds;
        batch->coverageClusters = AZStd::make_shared<const AZStd::vector<AZ::Aabb>>(AZStd::move(coverageClusters));
        batch->world = world;
        return batch;
    }

    void SetResolveBatchPublished(const ResolveBatchPtr& batch, bool published)
    {
        if (!batch) return;
        auto& registry = GetRegistry();
        AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
        batch->published = published;
    }

    void SetResolveBatchWorld(const ResolveBatchPtr& batch, const AZ::Transform& world)
    {
        if (!batch) return;
        auto& registry = GetRegistry();
        AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
        batch->world = world;
    }

    void SetResolveBatchFaceCacheInputs(const ResolveBatchPtr& batch, const FaceCacheBatchInputs& inputs)
    {
        if (!batch) return;
        auto& registry = GetRegistry();
        AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
        batch->faceCacheInputs = inputs;
    }

    void SetResolveBatchProxySelection(const ResolveBatchPtr& batch,
        const AZ::Data::Instance<AZ::RPI::Buffer>& buffer, AZ::u32 index)
    {
        if (!batch) return;
        auto& registry = GetRegistry();
        AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
        batch->faceCacheInputs.proxySelection = buffer;
        batch->faceCacheInputs.proxyBufferIndex = index;
    }

    void UpdateResolveBatchPacket(const ResolveBatchPtr& batch, const AZ::RPI::MeshDrawPacket& packet)
    {
        if (!batch || !packet.GetRHIDrawPacket()) return;
        const auto tag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("silpomHitForward"));
        const auto depthTag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("silpomHitDepth"));
        if (!tag.IsValid() && !depthTag.IsValid()) return;
        auto snapshot = AZStd::make_shared<PacketSnapshot>(packet);
        const auto* draw = snapshot->packet.get();
        auto& registry = GetRegistry();
        AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
        auto& keys = batch->packetKeys[snapshot->meshIdentity];
        for (const auto* key : keys) registry.items.erase(key);
        keys.clear();
        for (size_t i = 0; i < draw->GetDrawItemCount(); ++i)
        {
            if (draw->GetDrawListTag(i) != tag && draw->GetDrawListTag(i) != depthTag) continue;
            const auto* key = draw->GetDrawItem(i);
            registry.items[key] = {batch, snapshot, i};
            keys.push_back(key);
        }
    }

    namespace
    {
        struct Selection
        {
            AZStd::shared_ptr<const PacketSnapshot> snapshot;
            size_t itemIndex = 0;
            AZ::Aabb localBounds = AZ::Aabb::CreateNull();
            AZ::Transform world = AZ::Transform::CreateIdentity();
            AZStd::shared_ptr<const AZStd::vector<AZ::Aabb>> coverageClusters;
            AZ::u64 generation = 0;
            AZ::u32 ownerBase = 0, ownerCount = 0;
            FaceCacheBatchInputs faceCacheInputs;
            AZ::u32 neuralModel = 0;
            bool resolveEligible = false;
        };

        bool FindBatch(const AZ::RHI::DrawItem* item, AZ::RPI::Scene* scene, Selection& selection)
        {
            auto& registry = GetRegistry();
            AZStd::unique_lock<AZStd::mutex> lock(registry.mutex);
            const auto found = registry.items.find(item);
            if (found == registry.items.end()) return false;
            const auto batch = found->second.batch.lock();
            if (!batch || !batch->published || batch->scene != scene)
            {
                // The temporary shared pointer can be the final owner when a
                // generation is concurrently retired; do not destroy it locked.
                lock.unlock();
                return false;
            }
            selection = {found->second.snapshot, found->second.itemIndex, batch->localBounds, batch->world,
                batch->coverageClusters, batch->generation, batch->ownerBase, batch->ownerCount, batch->faceCacheInputs,
                batch->neuralModel, batch->resolveEligible};
            lock.unlock();
            return true;
        }

        enum class WallCapability { Unknown, NonWall, Wall };

        struct RepairAttachmentResources
        {
            AZStd::vector<AZ::RPI::Ptr<AZ::RPI::PassAttachment>> attachments;
        };

        WallCapability GetWallCapability(const PacketSnapshot& snapshot, size_t itemIndex, AZ::u32 neuralModel)
        {
            // MeshDrawPacket appends ShaderData and DrawItem in the same order.
            // Check that correspondence rather than inferring a shader from the
            // material's configuration or from the optional face-cache inputs.
            if (snapshot.shaders.size() != snapshot.packet->GetDrawItemCount() || itemIndex >= snapshot.shaders.size())
                return WallCapability::Unknown;
            const auto& active = snapshot.shaders[itemIndex];
            if (!active.m_shader || active.m_materialPipelineName != AZ::Name("MainPipeline"))
                return WallCapability::Unknown;
            const auto& asset = active.m_shader->GetAsset();
            if (!asset || active.m_shader->GetDrawListTag() != snapshot.packet->GetDrawListTag(itemIndex))
                return WallCapability::Unknown;
            const auto& supervariant = asset->GetSupervariantName(active.m_shader->GetSupervariantIndex());
            if (!supervariant.IsEmpty() && supervariant != AZ::Name("NoMSAA")) return WallCapability::Unknown;
            const auto& tag = active.m_shaderTag;
            // Logical shader tags can survive a custom material shader
            // override. Trust the WALL contract only for the actual built-in
            // generated shader product paired with that tag, including for
            // proxy/non-wall decisions. Missing catalog identity is unknown.
            struct KnownShader { const char* tag; const char* product; };
            static constexpr KnownShader knownShaders[] = {
                {"silpom_hit_depth", "silpomhitdepth"},
                {"silpom_hit_depth_bounds", "silpomhitdepthbounds"},
                {"silpom_hit_depth_neural", "silpomhitdepthneural"},
                {"silpom_hit_depth_neural_cache", "silpomhitdepthneuralcache"},
                {"silpom_hit_depth_lod_near", "silpomhitdepthlodnear"},
                {"silpom_hit_depth_lod_near_wall", "silpomhitdepthlodnearwall"},
                {"silpom_hit_depth_lod_shell", "silpomhitdepthlodshell"},
                {"silpom_hit_depth_proxy", "silpomhitdepthproxy"},
                {"silpom_hit_depth_learned_proxy", "silpomhitdepthlearnedproxy"}
            };
            bool knownProduct = false;
            if (itemIndex < snapshot.shaderProductPaths.size())
                for (const auto& known : knownShaders)
                    if (tag == AZ::Name(known.tag))
                    {
                        const auto& path = snapshot.shaderProductPaths[itemIndex];
                        knownProduct = path == AZStd::string::format("materials/types/silpommesh/silpommesh_mainpipeline_%s.azshader", known.product)
                            || path == AZStd::string::format("materials/types/silpommesh/silpommeshuv_mainpipeline_%s.azshader", known.product);
                        break;
                    }
            if (!knownProduct) return WallCapability::Unknown;
            if (tag == AZ::Name("silpom_hit_depth_proxy") || tag == AZ::Name("silpom_hit_depth_learned_proxy"))
                return WallCapability::NonWall; // Both publish SHELL without WALL.
            if (tag == AZ::Name("silpom_hit_depth_lod_near_wall"))
            {
                // The default MSAA shader forces model 4 in both stages. The
                // NoMSAA supervariant retains the published material model.
                return supervariant.IsEmpty() || neuralModel == 1 || neuralModel == 4
                    ? WallCapability::Wall : WallCapability::NonWall;
            }
            if (tag == AZ::Name("silpom_hit_depth") || tag == AZ::Name("silpom_hit_depth_bounds")
                || tag == AZ::Name("silpom_hit_depth_neural") || tag == AZ::Name("silpom_hit_depth_neural_cache")
                || tag == AZ::Name("silpom_hit_depth_lod_near") || tag == AZ::Name("silpom_hit_depth_lod_shell"))
                return neuralModel == 1 || neuralModel == 4 ? WallCapability::Wall : WallCapability::NonWall;
            return WallCapability::Unknown;
        }

        struct ResolveDraw
        {
            explicit ResolveDraw(const Selection& selected)
                : source(selected.snapshot), geometry(selected.snapshot->geometry) {}

            AZStd::shared_ptr<const PacketSnapshot> source;
            AZ::RHI::GeometryView geometry;
            AZ::RHI::Scissor scissor;
            AZ::RHI::Ptr<AZ::RHI::DrawPacket> packet;
        };

        constexpr AZ::u32 FaceCacheRecordBytes = 48;
        constexpr AZ::u32 FaceCoefficientBytes = 48;
        struct FaceCacheTableEntry
        {
            AZ::u32 ownerBase, faceCount, cacheOffsetBytes, meshBufferIndex;
            AZ::u32 coefficientBufferIndex, generationLow, generationHigh, flags;
        };
        static_assert(sizeof(FaceCacheTableEntry) == 32);

        struct FaceCacheDispatch
        {
            explicit FaceCacheDispatch(const AZ::RHI::DispatchItem& prototype) : item(prototype) {}
            Selection selected;
            AZ::u32 cacheOffsetBytes = 0;
            AZ::Data::Instance<AZ::RPI::ShaderResourceGroup> srg;
            AZ::RHI::ConstPtr<AZ::RHI::BufferView> facesView, coefficientView;
            AZ::RHI::DispatchItem item;
        };

        struct FrameFaceCache
        {
            AZ::RPI::ViewPtr view;
            AZ::u64 tick = 0;
            AZ::Data::Instance<AZ::RPI::Shader> shader;
            AZ::Data::Instance<AZ::RPI::ShaderResourceGroup> drawSrg;
            AZStd::vector<AZ::RHI::ConstPtr<AZ::RHI::DevicePipelineState>> pipelineStates;
            AZ::Data::Instance<AZ::RPI::Buffer> records, table;
            AZ::RHI::ConstPtr<AZ::RHI::BufferView> recordsReadView, recordsWriteView, tableView;
            AZStd::vector<FaceCacheDispatch> dispatches;
            AZ::u64 inputIdentity = 0;
            AZ::u32 faceCount = 0;
            // Published only after every dispatch SRG is ready for this frame.
            bool ready = false;
        };

        AZ::u64 CurrentRenderTick()
        {
            const auto* system = AZ::RPI::RPISystemInterface::Get();
            return system ? system->GetCurrentTick() : 0;
        }

        AZStd::shared_ptr<FrameFaceCache> FindFaceCache(const AZ::RPI::RenderPipeline* pipeline, const AZ::RPI::ViewPtr& view)
        {
            if (!r_silpomFaceCache || !view) return {};
            auto& registry = GetRegistry();
            AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
            const auto found = registry.faceCaches.find(pipeline);
            auto cache = found == registry.faceCaches.end() ? AZStd::shared_ptr<FrameFaceCache>{} : found->second.lock();
            if (!cache || cache->view != view || cache->tick != CurrentRenderTick()) return {};
            return cache;
        }

        AZ::Data::Instance<AZ::RPI::Buffer> CreateFaceCacheBuffer(const AZStd::string& name,
            AZ::u64 byteCount, AZ::RPI::CommonBufferPoolType pool, const void* initialData = nullptr)
        {
            auto* system = AZ::RPI::BufferSystemInterface::Get();
            if (!system || !byteCount || byteCount > std::numeric_limits<AZ::u32>::max()) return {};
            AZ::RPI::CommonBufferDescriptor descriptor;
            descriptor.m_bufferName = name;
            descriptor.m_poolType = pool;
            descriptor.m_elementSize = 4;
            descriptor.m_elementFormat = AZ::RHI::Format::R32_UINT;
            descriptor.m_byteCount = byteCount;
            descriptor.m_bufferData = initialData;
            return system->CreateBufferFromCommonPool(descriptor);
        }

        AZ::RHI::ConstPtr<AZ::RHI::BufferView> FaceCacheRawView(
            const AZ::Data::Instance<AZ::RPI::Buffer>& buffer, AZ::RHI::BufferBindFlags flags)
        {
            if (!buffer || !buffer->GetRHIBuffer()) return {};
            auto descriptor = AZ::RHI::BufferViewDescriptor::CreateRaw(0, aznumeric_cast<AZ::u32>(buffer->GetBufferSize()));
            descriptor.m_overrideBindFlags = flags;
            return buffer->GetRHIBuffer()->GetBufferView(descriptor);
        }

        AZ::RHI::AttachmentId FaceCacheAttachmentId(const AZ::Data::Instance<AZ::RPI::Buffer>& buffer)
        {
            if (!buffer || !buffer->GetRHIBuffer()) return {};
            const auto* rhiBuffer = buffer->GetRHIBuffer();
            // RPI only creates an attachment id for writable/dynamic buffers;
            // asking a static ShaderRead buffer for that id asserts. Such face
            // buffers still need a unique frame-graph identity when imported.
            // Retained frame snapshots keep the RHI object alive through use.
            constexpr auto ownsAttachmentId = AZ::RHI::BufferBindFlags::ShaderWrite
                | AZ::RHI::BufferBindFlags::CopyWrite | AZ::RHI::BufferBindFlags::DynamicInputAssembly;
            if (AZ::RHI::CheckBitsAny(rhiBuffer->GetDescriptor().m_bindFlags, ownsAttachmentId))
                return buffer->GetAttachmentId();
            return AZ::Name(AZStd::string::format("SilPOM.FaceCache.External.%p", static_cast<const void*>(rhiBuffer)));
        }

        bool ImportFaceCacheBuffer(AZ::RHI::FrameGraphInterface frameGraph,
            const AZ::Data::Instance<AZ::RPI::Buffer>& buffer)
        {
            const auto attachmentId = FaceCacheAttachmentId(buffer);
            if (attachmentId.IsEmpty()) return false;
            auto attachments = frameGraph.GetAttachmentDatabase();
            return attachments.IsAttachmentValid(attachmentId)
                || attachments.ImportBuffer(attachmentId, buffer->GetRHIBuffer()) == AZ::RHI::ResultCode::Success;
        }

        bool UseFaceCacheBuffer(AZ::RHI::FrameGraphInterface frameGraph,
            const AZ::Data::Instance<AZ::RPI::Buffer>& buffer, AZ::RHI::ScopeAttachmentAccess access,
            AZ::RHI::ScopeAttachmentStage stage)
        {
            if (!ImportFaceCacheBuffer(frameGraph, buffer)) return false;
            AZ::RHI::BufferScopeAttachmentDescriptor descriptor(FaceCacheAttachmentId(buffer),
                AZ::RHI::BufferViewDescriptor::CreateRaw(0, aznumeric_cast<AZ::u32>(buffer->GetBufferSize())));
            return frameGraph.UseShaderAttachment(descriptor, access, stage) == AZ::RHI::ResultCode::Success;
        }

        class FaceCachePass final : public AZ::RPI::ComputePass
        {
            AZ_RPI_PASS(FaceCachePass);
        public:
            AZ_RTTI(FaceCachePass, "{6190F13D-E19B-4B23-97B2-E1B98761AB14}", AZ::RPI::ComputePass);
            AZ_CLASS_ALLOCATOR(FaceCachePass, AZ::SystemAllocator);
            static AZ::RPI::Ptr<FaceCachePass> Create(const AZ::RPI::PassDescriptor& descriptor)
            { return aznew FaceCachePass(descriptor); }
        private:
            explicit FaceCachePass(const AZ::RPI::PassDescriptor& descriptor) : ComputePass(descriptor) {}

            void TraceUnavailable(const char* reason)
            {
                if (r_silpomFaceCacheTrace && (m_unavailableFrames++ < 3 || CurrentRenderTick() % 60 == 0
                    || m_unavailableReason != reason))
                {
                    m_unavailableReason = reason;
                    AZ_Printf("SilPOM", "FaceCache pass=%s tick=%llu output=unavailable table=unavailable reason=%s\n",
                        GetPathName().GetCStr(), static_cast<unsigned long long>(CurrentRenderTick()), reason);
                }
            }

            void FrameBeginInternal(FramePrepareParams params) override
            {
                m_retired[m_retireFrame++ % m_retired.size()] = AZStd::move(m_frame);
                {
                    auto& registry = GetRegistry();
                    AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
                    registry.faceCaches.erase(m_pipeline);
                }
                ComputePass::FrameBeginInternal(params);
            }

            void SetupFrameGraphDependencies(AZ::RHI::FrameGraphInterface frameGraph) override
            {
                ComputePass::SetupFrameGraphDependencies(frameGraph);
                frameGraph.SetEstimatedItemCount(0);
                const auto view = GetView();
                if (!r_silpomFaceCache) return;
                if (!view || m_pipeline->GetViews(GetPipelineViewTag()).size() != 1
                    || !m_shader || !m_shaderResourceGroup)
                { TraceUnavailable("view-or-shader-not-ready"); return; }
                const auto* shaderSrg = m_shaderResourceGroup->GetRHIShaderResourceGroup();
                bool hasDevice = false, hasPipelineStates = true;
                if (shaderSrg)
                    AZ::RHI::MultiDeviceObject::IterateDevices(shaderSrg->GetDeviceMask(), [&](int deviceIndex)
                    {
                        hasDevice = true;
                        if (!m_dispatchItem.GetDeviceDispatchItem(deviceIndex).m_pipelineState)
                        {
                            hasPipelineStates = false;
                            return false;
                        }
                        return true;
                    });
                // A shader asset and SRG can exist even when PSO acquisition
                // failed. Do not publish readable records without an executable
                // producer; the raster consumers must retain count0/fallback.
                if (!hasDevice || !hasPipelineStates)
                { TraceUnavailable("compute-pipeline-state-unavailable"); return; }
                const auto tag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("silpomHitDepth"));
                if (!tag.IsValid()) return;
                AZStd::vector<Selection> selections;
                for (const auto& properties : view->GetDrawList(tag))
                {
                    if (!(properties.m_drawFilterMask & m_pipeline->GetDrawFilterMask()) || !properties.m_item->GetEnabled()) continue;
                    Selection selected;
                    if (!FindBatch(properties.m_item, m_pipeline->GetScene(), selected)) continue;
                    const auto& inputs = selected.faceCacheInputs;
                    if (!inputs.faces || !inputs.coefficients || inputs.maxCells != 65525u || !inputs.faceCount
                        || inputs.faceCount != selected.ownerCount || inputs.faces->GetBufferSize() < AZ::u64(inputs.faceCount) * 144u
                        || inputs.coefficients->GetBufferSize() < AZ::u64(inputs.faceCount) * FaceCoefficientBytes
                        || (inputs.proxyLodEnabled && !inputs.proxySelection)) continue;
                    if (AZStd::find_if(selections.begin(), selections.end(), [&](const Selection& other)
                        { return other.ownerBase == selected.ownerBase; }) == selections.end()) selections.push_back(AZStd::move(selected));
                }
                if (selections.empty()) { TraceUnavailable("no-eligible-visible-batches"); return; }
                AZStd::sort(selections.begin(), selections.end(), [](const Selection& a, const Selection& b)
                    { return a.ownerBase < b.ownerBase; });
                auto frame = AZStd::make_shared<FrameFaceCache>();
                frame->view = view;
                frame->tick = CurrentRenderTick();
                frame->shader = m_shader;
                frame->drawSrg = m_drawSrg;
                AZStd::vector<FaceCacheTableEntry> entries;
                AZ::u64 bytes = 0;
                for (auto& selected : selections)
                {
                    const auto& inputs = selected.faceCacheInputs;
                    const auto bindless = inputs.coefficients->GetBufferView()->GetBindlessReadIndex();
                    if (bindless.size() != 1 || bindless.begin()->second == AZ::u32(-1)) continue;
                    const AZ::u64 nextBytes = bytes + AZ::u64(inputs.faceCount) * FaceCacheRecordBytes;
                    if (nextBytes > std::numeric_limits<AZ::u32>::max()) return;
                    entries.push_back({selected.ownerBase, inputs.faceCount, aznumeric_cast<AZ::u32>(bytes),
                        inputs.meshBufferIndex, bindless.begin()->second, AZ::u32(selected.generation),
                        AZ::u32(selected.generation >> 32), 1u});
                    frame->dispatches.emplace_back(m_dispatchItem);
                    auto& dispatch = frame->dispatches.back();
                    dispatch.cacheOffsetBytes = aznumeric_cast<AZ::u32>(bytes);
                    frame->faceCount += inputs.faceCount;
                    const auto mix = [&](AZ::u64 value)
                    { frame->inputIdentity ^= value + 0x9e3779b97f4a7c15ull + (frame->inputIdentity << 6) + (frame->inputIdentity >> 2); };
                    mix(selected.generation); mix(selected.ownerBase);
                    mix(inputs.meshBufferIndex); mix(inputs.proxyBufferIndex); mix(inputs.maxCells); mix(inputs.neuralModel);
                    mix(inputs.faceCount); mix(AZ::u64(inputs.proxyLodEnabled));
                    for (float value : {inputs.scale, inputs.reference, inputs.reliefLodPixelBudget, inputs.maxReliefDistance})
                    { AZ::u32 bits; std::memcpy(&bits, &value, sizeof(bits)); mix(bits); }
                    dispatch.selected = AZStd::move(selected);
                    bytes = nextBytes;
                }
                if (frame->dispatches.empty()) { TraceUnavailable("coefficient-bindless-view-unavailable"); return; }
                const auto prefix = AZStd::string::format("%s.FaceCache.%llu", GetPathName().GetCStr(),
                    static_cast<unsigned long long>(frame->tick));
                frame->records = CreateFaceCacheBuffer(prefix + ".Records", bytes, AZ::RPI::CommonBufferPoolType::ReadWrite);
                frame->table = CreateFaceCacheBuffer(prefix + ".Table", entries.size() * sizeof(FaceCacheTableEntry),
                    AZ::RPI::CommonBufferPoolType::DynamicInputAssembly);
                // A host map is immediately visible to the consuming GPU and
                // cannot enter RPI's >64KiB asynchronous initial-data path.
                if (!frame->table || !frame->table->UpdateData(entries.data(), entries.size() * sizeof(FaceCacheTableEntry)))
                { TraceUnavailable("frame-table-upload-failed"); return; }
                frame->recordsReadView = FaceCacheRawView(frame->records, AZ::RHI::BufferBindFlags::ShaderRead);
                frame->recordsWriteView = FaceCacheRawView(frame->records,
                    AZ::RHI::BufferBindFlags::ShaderRead | AZ::RHI::BufferBindFlags::ShaderWrite);
                frame->tableView = FaceCacheRawView(frame->table, AZ::RHI::BufferBindFlags::ShaderRead);
                if (!frame->recordsReadView || !frame->recordsWriteView || !frame->tableView)
                { TraceUnavailable("frame-buffer-allocation-failed"); return; }
                AZ::RHI::MultiDeviceObject::IterateDevices(frame->records->GetRHIBuffer()->GetDeviceMask(), [&](int deviceIndex)
                {
                    if (const auto* state = m_dispatchItem.GetDeviceDispatchItem(deviceIndex).m_pipelineState)
                        frame->pipelineStates.emplace_back(state);
                    return true;
                });
                if (!UseFaceCacheBuffer(frameGraph, frame->records, AZ::RHI::ScopeAttachmentAccess::Write,
                    AZ::RHI::ScopeAttachmentStage::ComputeShader) || !ImportFaceCacheBuffer(frameGraph, frame->table)) return;
                for (auto& dispatch : frame->dispatches)
                {
                    const auto& inputs = dispatch.selected.faceCacheInputs;
                    // Every view declares the immutable coefficient dependency.
                    // The GPU only initializes entries whose version is absent;
                    // no CPU completion flag or first-view race is required.
                    if (!UseFaceCacheBuffer(frameGraph, inputs.coefficients, AZ::RHI::ScopeAttachmentAccess::ReadWrite,
                        AZ::RHI::ScopeAttachmentStage::ComputeShader)
                        || !UseFaceCacheBuffer(frameGraph, inputs.faces, AZ::RHI::ScopeAttachmentAccess::Read,
                            AZ::RHI::ScopeAttachmentStage::ComputeShader)) return;
                    if (inputs.proxyLodEnabled && !UseFaceCacheBuffer(frameGraph, inputs.proxySelection,
                        AZ::RHI::ScopeAttachmentAccess::Read, AZ::RHI::ScopeAttachmentStage::ComputeShader)) return;
                }
                m_frame = AZStd::move(frame);
                auto& registry = GetRegistry();
                AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
                registry.faceCaches[m_pipeline] = m_frame;
                frameGraph.SetEstimatedItemCount(aznumeric_cast<AZ::u32>(m_frame->dispatches.size()));
            }

            void CompileResources(const AZ::RHI::FrameGraphCompileContext&) override
            {
                if (!m_frame || !m_shader) return;
                if (m_drawSrg) m_drawSrg->Compile();
                for (auto& dispatch : m_frame->dispatches)
                {
                    const auto& selected = dispatch.selected;
                    const auto& inputs = selected.faceCacheInputs;
                    dispatch.srg = AZ::RPI::ShaderResourceGroup::Create(m_shader->GetAsset(), m_shader->GetSupervariantIndex(), AZ::Name("PassSrg"));
                    dispatch.facesView = FaceCacheRawView(inputs.faces, AZ::RHI::BufferBindFlags::ShaderRead);
                    dispatch.coefficientView = FaceCacheRawView(inputs.coefficients,
                        AZ::RHI::BufferBindFlags::ShaderRead | AZ::RHI::BufferBindFlags::ShaderWrite);
                    if (!dispatch.srg || !dispatch.facesView || !dispatch.coefficientView)
                    { TraceUnavailable("dispatch-resource-view-unavailable"); return; }
                    auto& srg = dispatch.srg;
                    const auto set = [&](const char* name, const auto& value)
                    {
                        const auto index = srg->FindShaderInputConstantIndex(AZ::Name(name));
                        return index.IsValid() && srg->SetConstant(index, value);
                    };
                    const auto setBuffer = [&](const char* name, const AZ::RHI::BufferView* value)
                    {
                        const auto index = srg->FindShaderInputBufferIndex(AZ::Name(name));
                        return index.IsValid() && srg->SetBufferView(index, value);
                    };
                    const auto setOptionalIdentity = [&](const char* name, AZ::u32 value)
                    {
                        // Identity is authoritative in the enclosing table;
                        // the kernel may strip these otherwise unused inputs.
                        const auto index = srg->FindShaderInputConstantIndex(AZ::Name(name));
                        return !index.IsValid() || srg->SetConstant(index, value);
                    };
                    if (!setBuffer("m_faces", dispatch.facesView.get())
                        || !setBuffer("m_faceCoefficients", dispatch.coefficientView.get())
                        || !setBuffer("m_faceCacheOutput", m_frame->recordsWriteView.get())
                        || !set("m_faceCount", inputs.faceCount) || !set("m_cacheOffset", dispatch.cacheOffsetBytes)
                        || !setOptionalIdentity("m_ownerBase", selected.ownerBase)
                        || !setOptionalIdentity("m_meshBufferIndex", inputs.meshBufferIndex)
                        || !set("m_neuralModel", inputs.neuralModel) || !set("m_maxCells", inputs.maxCells)
                        || !set("m_proxyBufferIndex", inputs.proxyBufferIndex) || !set("m_proxyLodEnabled", AZ::u32(inputs.proxyLodEnabled))
                        || !set("m_scale", inputs.scale) || !set("m_reference", inputs.reference)
                        || !set("m_reliefLodPixelBudget", inputs.reliefLodPixelBudget) || !set("m_maxReliefDistance", inputs.maxReliefDistance)
                        // TransformService stores Matrix3x4::CreateFromTransform
                        // with unit nonuniform scale. Matrix4x4's constructor
                        // uses those same float rows and adds (0,0,0,1).
                        || !set("m_objectToWorld", AZ::Matrix4x4::CreateFromTransform(selected.world)))
                    { TraceUnavailable("dispatch-srg-input-unavailable"); return; }
                    srg->Compile();
                    auto arguments = dispatch.item.GetArguments();
                    arguments.m_direct.m_totalNumberOfThreadsX = inputs.faceCount;
                    arguments.m_direct.m_totalNumberOfThreadsY = arguments.m_direct.m_totalNumberOfThreadsZ = 1;
                    dispatch.item.SetArguments(arguments);
                    const AZ::RHI::ShaderResourceGroup* srgs[] = {m_frame->view->GetRHIShaderResourceGroup(), srg->GetRHIShaderResourceGroup()};
                    dispatch.item.SetShaderResourceGroups(srgs);
                    if (m_drawSrg) dispatch.item.SetUniqueShaderResourceGroup(m_drawSrg->GetRHIShaderResourceGroup());
                }
                // This engine's FrameScheduler::CompileProducers visits scope
                // producers serially, in the declared prep-before-depth order.
                // Raster SRGs therefore observe readiness only after every
                // dispatch has been completely configured for this frame.
                m_frame->ready = true;
                if (r_silpomFaceCacheTrace && (m_traceFrames++ < 3 || m_frame->tick % 60 == 0 || m_traceIdentity != m_frame->inputIdentity))
                {
                    m_traceIdentity = m_frame->inputIdentity;
                    const auto generation = m_frame->dispatches.front().selected.generation;
                    AZ_Printf("SilPOM", "FaceCache pass=%s view=%s viewIdentity=%p tick=%llu batches=%zu faces=%u generation=%llu inputIdentity=%llu coefficientFacesChecked=%u coefficientWrites=GPU-version-guard output=ready table=ready\n",
                        GetPathName().GetCStr(), m_frame->view->GetName().GetCStr(), static_cast<void*>(m_frame->view.get()),
                        static_cast<unsigned long long>(m_frame->tick), m_frame->dispatches.size(), m_frame->faceCount,
                        static_cast<unsigned long long>(generation), static_cast<unsigned long long>(m_frame->inputIdentity), m_frame->faceCount);
                }
            }

            void BuildCommandListInternal(const AZ::RHI::FrameGraphExecuteContext& context) override
            {
                if (!m_frame || !m_frame->ready) return;
                const auto range = context.GetSubmitRange();
                const auto end = AZStd::min<size_t>(range.m_endIndex, m_frame->dispatches.size());
                for (size_t i = range.m_startIndex; i < end; ++i)
                    context.GetCommandList()->Submit(m_frame->dispatches[i].item.GetDeviceDispatchItem(context.GetDeviceIndex()), aznumeric_cast<AZ::u32>(i));
            }

            AZStd::shared_ptr<FrameFaceCache> m_frame;
            AZStd::array<AZStd::shared_ptr<FrameFaceCache>, 8> m_retired;
            size_t m_retireFrame = 0;
            size_t m_traceFrames = 0;
            AZ::u64 m_traceIdentity = 0;
            size_t m_unavailableFrames = 0;
            AZStd::string m_unavailableReason;
        };

        class HitRasterPass : public AZ::RPI::RasterPass
        {
            AZ_RPI_PASS(HitRasterPass);
        public:
            AZ_RTTI(HitRasterPass, "{41462BAF-49DB-4EBF-A965-1866251FC72B}", AZ::RPI::RasterPass);
            AZ_CLASS_ALLOCATOR(HitRasterPass, AZ::SystemAllocator);
            static AZ::RPI::Ptr<HitRasterPass> Create(const AZ::RPI::PassDescriptor& descriptor)
            { return aznew HitRasterPass(descriptor); }
        protected:
            explicit HitRasterPass(const AZ::RPI::PassDescriptor& descriptor) : RasterPass(descriptor) {}
            void FrameBeginInternal(FramePrepareParams params) override
            {
                m_faceCache.reset();
                m_retiredInventories[m_inventoryFrame++ % m_retiredInventories.size()] = AZStd::move(m_repairInventory);
                if (GetDrawListTag() == AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("silpomHitDepth")))
                {
                    auto& registry = GetRegistry();
                    AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
                    registry.repairInventories.erase(m_pipeline);
                }
                RasterPass::FrameBeginInternal(params);
            }
            void SetupFrameGraphDependencies(AZ::RHI::FrameGraphInterface frameGraph) override
            {
                CaptureRepairInventory();
                // A RasterPass binds one ViewSrg. Never reuse its first eye's
                // cache for a tag that resolves to multiple actual views.
                const auto& views = m_pipeline->GetViews(GetPipelineViewTag());
                m_faceCache = views.size() == 1 ? FindFaceCache(m_pipeline, views.front()) : AZStd::shared_ptr<FrameFaceCache>{};
                if (m_faceCache)
                {
                    constexpr auto stages = AZ::RHI::ScopeAttachmentStage::VertexShader | AZ::RHI::ScopeAttachmentStage::FragmentShader;
                    bool valid = UseFaceCacheBuffer(frameGraph, m_faceCache->records, AZ::RHI::ScopeAttachmentAccess::Read, stages)
                        && UseFaceCacheBuffer(frameGraph, m_faceCache->table, AZ::RHI::ScopeAttachmentAccess::Read, stages);
                    for (const auto& dispatch : m_faceCache->dispatches)
                    {
                        valid = UseFaceCacheBuffer(frameGraph, dispatch.selected.faceCacheInputs.coefficients,
                            AZ::RHI::ScopeAttachmentAccess::Read, stages) && valid;
                        // Once imported for the compute reader, the original
                        // bindless face/proxy reads also need their raster
                        // stages declared so RHI transitions include pixel SRV.
                        valid = UseFaceCacheBuffer(frameGraph, dispatch.selected.faceCacheInputs.faces,
                            AZ::RHI::ScopeAttachmentAccess::Read, stages) && valid;
                        const auto& inputs = dispatch.selected.faceCacheInputs;
                        if (inputs.proxyLodEnabled)
                            valid = UseFaceCacheBuffer(frameGraph, inputs.proxySelection,
                                AZ::RHI::ScopeAttachmentAccess::Read, stages) && valid;
                    }
                    if (!valid) m_faceCache.reset();
                }
                RasterPass::SetupFrameGraphDependencies(frameGraph);
            }
            void CompileResources(const AZ::RHI::FrameGraphCompileContext& context) override
            {
                if (m_shaderResourceGroup)
                {
                    const auto index = m_shaderResourceGroup->FindShaderInputConstantIndex(AZ::Name("m_silpomViewport"));
                    if (index.IsValid())
                        m_shaderResourceGroup->SetConstant(index, AZ::Vector4(m_viewportState.m_minX, m_viewportState.m_minY,
                            m_viewportState.m_maxX - m_viewportState.m_minX, m_viewportState.m_maxY - m_viewportState.m_minY));
                    const auto countIndex = m_shaderResourceGroup->FindShaderInputConstantIndex(AZ::Name("m_silpomFaceCacheBatchCount"));
                    if (countIndex.IsValid())
                    {
                        AZ::u32 count = 0;
                        if (m_faceCache && m_faceCache->ready && m_faceCache->tick == CurrentRenderTick())
                        {
                            const auto recordsIndex = m_shaderResourceGroup->FindShaderInputBufferIndex(AZ::Name("m_silpomFaceCache"));
                            const auto tableIndex = m_shaderResourceGroup->FindShaderInputBufferIndex(AZ::Name("m_silpomFaceCacheTable"));
                            if (recordsIndex.IsValid() && tableIndex.IsValid()
                                && m_shaderResourceGroup->SetBufferView(recordsIndex, m_faceCache->recordsReadView.get())
                                && m_shaderResourceGroup->SetBufferView(tableIndex, m_faceCache->tableView.get()))
                                count = aznumeric_cast<AZ::u32>(m_faceCache->dispatches.size());
                        }
                        m_shaderResourceGroup->SetConstant(countIndex, count);
                        if (r_silpomFaceCacheTrace && (count != m_lastCacheCount || CurrentRenderTick() % 60 == 0))
                        {
                            m_lastCacheCount = count;
                            const auto view = GetView();
                            AZ_Printf("SilPOM", "FaceCacheBind pass=%s view=%s tick=%llu batches=%u resources=%s\n",
                                GetPathName().GetCStr(), view ? view->GetName().GetCStr() : "none",
                                static_cast<unsigned long long>(CurrentRenderTick()), count, count ? "ready" : "reference");
                        }
                    }
                }
                RasterPass::CompileResources(context);
            }

            void CaptureRepairInventory()
            {
                const auto depthTag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("silpomHitDepth"));
                if (GetDrawListTag() != depthTag) return;
                auto inventory = AZStd::make_shared<RepairProducerInventory>();
                inventory->pipeline = m_pipeline;
                inventory->tick = CurrentRenderTick();
                inventory->viewport = m_viewportState;
                inventory->scissor = m_scissorState;
                const auto& views = m_pipeline->GetViews(GetPipelineViewTag());
                inventory->complete = views.size() == 1;
                if (inventory->complete) inventory->view = views.front().get();
                auto attachments = AZStd::make_shared<RepairAttachmentResources>();
                const auto captureAttachment = [&](const char* slot, const AZ::RPI::PassAttachment*& identity)
                {
                    const auto* binding = FindAttachmentBinding(AZ::Name(slot));
                    if (!binding || !binding->GetAttachment()
                        || binding->GetAttachment()->GetAttachmentType() != AZ::RHI::AttachmentType::Image)
                    { inventory->complete = false; return; }
                    identity = binding->GetAttachment().get();
                    attachments->attachments.push_back(binding->GetAttachment());
                    const auto& descriptor = binding->GetAttachment()->m_descriptor.m_image;
                    if (!inventory->width)
                    {
                        inventory->width = descriptor.m_size.m_width;
                        inventory->height = descriptor.m_size.m_height;
                        inventory->sampleCount = descriptor.m_multisampleState.m_samples;
                    }
                    const auto& imageView = binding->m_unifiedScopeDesc.GetAsImage().m_imageViewDescriptor;
                    if (!inventory->width || !inventory->height || !inventory->sampleCount
                        || descriptor.m_size.m_width != inventory->width || descriptor.m_size.m_height != inventory->height
                        || descriptor.m_multisampleState.m_samples != inventory->sampleCount
                        || descriptor.m_mipLevels != 1 || descriptor.m_arraySize != 1
                        || imageView.m_mipSliceMin || imageView.m_arraySliceMin)
                        inventory->complete = false;
                };
                captureAttachment("Hit", inventory->hitAttachment);
                captureAttachment("HitUvGradients", inventory->uvGradientsAttachment);
                captureAttachment("HitDepthGradients", inventory->depthGradientsAttachment);
                captureAttachment("Depth", inventory->depthAttachment);
                inventory->sources.emplace_back(attachments);
                // This list also includes dynamic draws for this particular
                // pass. An unregistered draw must not turn an empty registry
                // into proof that repair has no possible donors.
                for (const auto& properties : m_drawListView)
                {
                    if (!properties.m_item) { inventory->complete = false; continue; }
                    if (!properties.m_item->GetEnabled()
                        || !(properties.m_drawFilterMask & m_pipeline->GetDrawFilterMask())) continue;
                    Selection selected;
                    if (!FindBatch(properties.m_item, m_pipeline->GetScene(), selected))
                    { inventory->complete = false; continue; }
                    auto retained = AZStd::make_shared<const Selection>(selected);
                    inventory->sources.emplace_back(retained);
                    const auto capability = GetWallCapability(*selected.snapshot, selected.itemIndex, selected.neuralModel);
                    if (capability == WallCapability::Unknown) { inventory->complete = false; continue; }
                    bool potentialWall = capability == WallCapability::Wall;
                    for (size_t i = 0; !potentialWall && i < selected.snapshot->packet->GetDrawItemCount(); ++i)
                        if (selected.snapshot->packet->GetDrawListTag(i) == depthTag
                            && GetWallCapability(*selected.snapshot, i, selected.neuralModel) == WallCapability::Wall)
                            potentialWall = true;
                    if (!potentialWall) continue;

                    const auto* indexBuffer = selected.snapshot->geometry.GetIndexBufferView().GetBuffer();
                    if (!indexBuffer || !selected.localBounds.IsValid()) { inventory->complete = false; continue; }
                    bool compatible = true;
                    int sourceScissorCount = -1;
                    auto effectiveScissor = m_scissorState;
                    const auto* item = selected.snapshot->packet->GetDrawItem(selected.itemIndex);
                    AZ::RHI::MultiDeviceObject::IterateDevices(indexBuffer->GetDeviceMask(), [&](int deviceIndex)
                    {
                        const auto& deviceItem = item->GetDeviceDrawItem(deviceIndex);
                        const auto sourceGeometry = selected.snapshot->sourceGeometryViews.find(deviceIndex);
                        if (sourceGeometry == selected.snapshot->sourceGeometryViews.end()
                            || deviceItem.m_geometryView != sourceGeometry->second || deviceItem.m_viewportsCount != 0
                            || deviceItem.m_scissorsCount > 1 || deviceItem.m_drawInstanceArgs.m_instanceCount != 1)
                        { compatible = false; return false; }
                        const auto scissor = deviceItem.m_scissorsCount ? deviceItem.m_scissors[0] : m_scissorState;
                        if (sourceScissorCount >= 0 && (sourceScissorCount != deviceItem.m_scissorsCount
                            || effectiveScissor.m_minX != scissor.m_minX || effectiveScissor.m_minY != scissor.m_minY
                            || effectiveScissor.m_maxX != scissor.m_maxX || effectiveScissor.m_maxY != scissor.m_maxY))
                        { compatible = false; return false; }
                        sourceScissorCount = deviceItem.m_scissorsCount;
                        effectiveScissor = scissor; // Per-item scissors replace the pass's scissor.
                        return true;
                    });
                    if (!compatible || sourceScissorCount < 0) { inventory->complete = false; continue; }
                    inventory->producers.push_back({selected.generation, selected.localBounds, selected.world,
                        effectiveScissor, selected.coverageClusters});
                }
                m_repairInventory = AZStd::move(inventory);
                auto& registry = GetRegistry();
                AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
                const auto found = registry.repairInventories.find(m_pipeline);
                if (found != registry.repairInventories.end() && !found->second.expired())
                {
                    // Multiple depth passes for one pipeline need a combined
                    // inventory. Until supported, their union is full-domain.
                    auto duplicate = AZStd::make_shared<RepairProducerInventory>(*m_repairInventory);
                    duplicate->complete = false;
                    m_repairInventory = AZStd::move(duplicate);
                }
                registry.repairInventories[m_pipeline] = m_repairInventory;
            }

            AZStd::shared_ptr<FrameFaceCache> m_faceCache;
            AZStd::shared_ptr<const RepairProducerInventory> m_repairInventory;
            AZStd::array<AZStd::shared_ptr<const RepairProducerInventory>, 8> m_retiredInventories;
            size_t m_inventoryFrame = 0;
            AZ::u32 m_lastCacheCount = AZ::u32(-1);
        };

        class ResolvePass final : public HitRasterPass
        {
            AZ_RPI_PASS(ResolvePass);
        public:
            AZ_RTTI(ResolvePass, "{8E2937C1-C24B-47F3-9C87-B4CD1DF57ECA}", HitRasterPass);
            AZ_CLASS_ALLOCATOR(ResolvePass, AZ::SystemAllocator);

            static AZ::RPI::Ptr<ResolvePass> Create(const AZ::RPI::PassDescriptor& descriptor)
            {
                return aznew ResolvePass(descriptor);
            }

        private:
            explicit ResolvePass(const AZ::RPI::PassDescriptor& descriptor) : HitRasterPass(descriptor) {}

            void FrameBeginInternal(FramePrepareParams params) override
            {
                // Retain complete RPI and RHI resources beyond command recording
                // for the in-flight frame window. Each pass owns its scissors,
                // geometry and clones; other views never share mutable items.
                m_retiredDraws[m_retireFrame++ % m_retiredDraws.size()] = AZStd::move(m_resolveDraws);
                m_resolveList.clear();
                m_compiledRegions = false;
                HitRasterPass::FrameBeginInternal(params);
            }

            void SetupFrameGraphDependencies(AZ::RHI::FrameGraphInterface frameGraph) override
            {
                // Scene::UpdateSrgs has supplied the current jittered projection
                // before this callback. Expand the list now, so Atom partitions
                // command recording using the actual number of rectangle draws.
                PrepareRegions();
                HitRasterPass::SetupFrameGraphDependencies(frameGraph);
            }

            void PrepareRegions()
            {
                if (m_compiledRegions || !r_silpomResolveRegions || m_drawListView.empty()) return;
                m_compiledRegions = true;
                const auto& views = m_pipeline->GetViews(GetPipelineViewTag());
                const auto* hit = FindAttachmentBinding(AZ::Name("Hit"));
                if (views.empty() || !hit || !hit->GetAttachment()) return;
                const auto& size = hit->GetAttachment()->m_descriptor.m_image.m_size;
                if (!size.m_width || !size.m_height) return;

                const auto& worldToClip = views.front()->GetWorldToClipMatrixWithOffset();
                const auto sourceList = m_drawListView;
                m_resolveList.reserve(sourceList.size() * 2);
                m_resolveDraws.reserve(sourceList.size() * 2);
                size_t replaced = 0, empty = 0, rectangles = 0;
                AZ::u64 scheduledPixels = 0, referencePixels = 0;
                for (const auto& properties : sourceList)
                {
                    const auto retainOriginal = [&]() { m_resolveList.push_back(properties); };
                    Selection selection;
                    if (!FindBatch(properties.m_item, m_pipeline->GetScene(), selection) || !selection.resolveEligible)
                    { retainOriginal(); continue; }
                    auto args = selection.snapshot->geometry.GetDrawArguments();
                    const auto* indexBuffer = selection.snapshot->geometry.GetIndexBufferView().GetBuffer();
                    if (!indexBuffer || args.m_type != AZ::RHI::DrawType::Indexed || args.m_indexed.m_indexCount < 3)
                    { retainOriginal(); continue; }
                    const auto deviceMask = indexBuffer->GetDeviceMask();
                    const auto* sourcePacket = selection.snapshot->packet.get();
                    const auto* sourceItem = sourcePacket->GetDrawItem(selection.itemIndex);
                    if (!sourceItem->GetEnabled()) { retainOriginal(); continue; }

                    AZ::RHI::Scissor support = m_scissorState;
                    bool compatible = true;
                    int sourceScissorCount = -1;
                    AZ::RHI::Scissor sourceScissor;
                    AZ::RHI::MultiDeviceObject::IterateDevices(deviceMask, [&](int deviceIndex)
                    {
                        const auto& deviceItem = sourceItem->GetDeviceDrawItem(deviceIndex);
                        // Preserve unusual per-item viewport/scissor arrays and
                        // rewritten/instanced geometry through the reference path.
                        const auto sourceView = selection.snapshot->sourceGeometryViews.find(deviceIndex);
                        if (sourceView == selection.snapshot->sourceGeometryViews.end()
                            || deviceItem.m_geometryView != sourceView->second
                            || deviceItem.m_viewportsCount != 0 || deviceItem.m_scissorsCount > 1)
                        {
                            compatible = false;
                            return false;
                        }
                        if (sourceScissorCount >= 0
                            && (sourceScissorCount != deviceItem.m_scissorsCount
                                || (sourceScissorCount == 1
                                    && (sourceScissor.m_minX != deviceItem.m_scissors[0].m_minX
                                        || sourceScissor.m_minY != deviceItem.m_scissors[0].m_minY
                                        || sourceScissor.m_maxX != deviceItem.m_scissors[0].m_maxX
                                        || sourceScissor.m_maxY != deviceItem.m_scissors[0].m_maxY))))
                        {
                            compatible = false;
                            return false;
                        }
                        sourceScissorCount = deviceItem.m_scissorsCount;
                        if (deviceItem.m_scissorsCount == 1)
                        {
                            const auto& existing = deviceItem.m_scissors[0];
                            sourceScissor = existing;
                            support.m_minX = AZStd::max(support.m_minX, existing.m_minX);
                            support.m_minY = AZStd::max(support.m_minY, existing.m_minY);
                            support.m_maxX = AZStd::min(support.m_maxX, existing.m_maxX);
                            support.m_maxY = AZStd::min(support.m_maxY, existing.m_maxY);
                        }
                        return true;
                    });
                    if (!compatible) { retainOriginal(); continue; }

                    const auto worldMatrix = AZ::Matrix4x4::CreateFromTransform(selection.world);
                    const ResolveRegion::Viewport viewport{m_viewportState.m_minX, m_viewportState.m_minY,
                        m_viewportState.m_maxX, m_viewportState.m_maxY};
                    const ResolveRegion::Rect existing{support.m_minX, support.m_minY, support.m_maxX, support.m_maxY};
                    // The existing invalid-W fallback returns precisely the
                    // allowed viewport/scissor/attachment intersection.
                    const auto limit = ResolveRegion::Compute({}, viewport, existing, size.m_width, size.m_height);
                    double errorMatrix[4][4] = {};
                    for (int row = 0; row < 4; ++row)
                        for (int col = 0; col < 4; ++col)
                            for (int mid = 0; mid < 4; ++mid)
                                errorMatrix[row][col] += std::abs(double(worldToClip.GetElement(row, mid)))
                                    * std::abs(double(worldMatrix.GetElement(mid, col)));
                    const auto projectBounds = [&](const AZ::Aabb& bounds, bool clipped)
                    {
                        std::array<ResolveRegion::Clip, 8> corners, errors;
                        const auto lo = bounds.GetMin(), hi = bounds.GetMax();
                        for (size_t i = 0; i < corners.size(); ++i)
                        {
                            const AZ::Vector3 local((i & 1) ? hi.GetX() : lo.GetX(),
                                (i & 2) ? hi.GetY() : lo.GetY(), (i & 4) ? hi.GetZ() : lo.GetZ());
                            const AZ::Vector4 localH(local, 1.0f);
                            const auto clip = worldToClip * (worldMatrix * localH);
                            corners[i] = {clip.GetX(), clip.GetY(), clip.GetZ(), clip.GetW()};
                            double error[4] = {};
                            for (int row = 0; row < 4; ++row)
                            {
                                double magnitude = 0;
                                for (int col = 0; col < 4; ++col)
                                    magnitude += errorMatrix[row][col] * std::abs(double(localH.GetElement(col)));
                                error[row] = magnitude * (1.0 / 262144.0) + 1e-7;
                            }
                            errors[i] = {error[0], error[1], error[2], error[3]};
                        }
                        return clipped ? ResolveRegions::ComputeClipped(corners, viewport, existing, size.m_width, size.m_height, errors)
                            : ResolveRegion::Compute(corners, viewport, existing, size.m_width, size.m_height, errors);
                    };
                    std::vector<ResolveRegion::Rect> regions;
                    if (r_silpomResolveMultiRect && selection.coverageClusters && !selection.coverageClusters->empty())
                    {
                        std::vector<ResolveRegion::Rect> projected;
                        projected.reserve(selection.coverageClusters->size());
                        for (const auto& cluster : *selection.coverageClusters)
                        {
                            const auto region = projectBounds(cluster, true);
                            if (!region.Empty()) projected.push_back(region);
                        }
                        regions = ResolveRegions::Merge(projected, limit);
                    }
                    else regions.push_back(projectBounds(selection.localBounds, false));
                    // Keep an empty draw for empty support. This preserves the
                    // source-list statistics floor while producing no fragments.
                    if (regions.empty()) regions.push_back({});
                    args.m_indexed.m_indexCount = 3;
                    for (const auto& region : regions)
                    {
                        auto resolved = AZStd::make_unique<ResolveDraw>(selection);
                        resolved->scissor = AZ::RHI::Scissor(region.minX, region.minY, region.maxX, region.maxY);
                        resolved->geometry.SetDrawArguments(args);
                        AZ::RHI::DrawPacketBuilder builder(deviceMask);
                        resolved->packet = builder.Clone(sourcePacket);
                        auto* item = resolved->packet->GetDrawItem(selection.itemIndex);
                        AZ::RHI::MultiDeviceObject::IterateDevices(deviceMask, [&](int deviceIndex)
                        {
                            // Restore exact per-item overrides, including root
                            // constants, SRGs, stencil and instance arguments.
                            auto& deviceItem = const_cast<AZ::RHI::DeviceDrawItem&>(item->GetDeviceDrawItem(deviceIndex));
                            deviceItem = sourceItem->GetDeviceDrawItem(deviceIndex);
                            return true;
                        });
                        item->SetGeometryView(&resolved->geometry);
                        item->SetScissors(&resolved->scissor, 1);
                        auto replacement = properties;
                        replacement.m_item = item;
                        m_resolveList.push_back(replacement);
                        m_resolveDraws.push_back(AZStd::move(resolved));
                        ++rectangles;
                        if (region.Empty()) ++empty;
                        else scheduledPixels += AZ::u64(region.maxX - region.minX) * AZ::u64(region.maxY - region.minY);
                    }
                    ++replaced;
                    referencePixels += AZ::u64(AZStd::max(0, support.m_maxX - support.m_minX))
                        * AZ::u64(AZStd::max(0, support.m_maxY - support.m_minY));
                }
                AZ::RPI::PassSystemInterface::Get()->IncrementFrameDrawItemCount(
                    aznumeric_cast<AZ::u32>(m_resolveList.size() - sourceList.size()));
                m_drawItemCount = aznumeric_cast<AZ::u32>(m_resolveList.size());
                m_drawListView = m_resolveList;
                if (r_silpomResolveRegionTrace)
                    AZ_Printf("SilPOM", "ResolveRegions pass=%s view=%s items=%zu replaced=%zu rectangles=%zu multi=%u empty=%zu indices=3 scheduledPixels=%llu referencePixels=%llu\n",
                        GetPathName().GetCStr(), views.front()->GetName().GetCStr(), m_resolveList.size(), replaced, rectangles,
                        unsigned(bool(r_silpomResolveMultiRect)), empty,
                        static_cast<unsigned long long>(scheduledPixels), static_cast<unsigned long long>(referencePixels));
            }

            AZStd::vector<AZStd::unique_ptr<ResolveDraw>> m_resolveDraws;
            AZStd::array<AZStd::vector<AZStd::unique_ptr<ResolveDraw>>, 8> m_retiredDraws;
            size_t m_retireFrame = 0;
            AZ::RHI::DrawList m_resolveList;
            bool m_compiledRegions = false;
        };
    }

    AZStd::shared_ptr<const RepairProducerInventory> FindRepairProducerInventory(
        const AZ::RPI::RenderPipeline* pipeline, const AZ::RPI::View* view)
    {
        auto& registry = GetRegistry();
        AZStd::lock_guard<AZStd::mutex> lock(registry.mutex);
        const auto found = registry.repairInventories.find(pipeline);
        auto inventory = found == registry.repairInventories.end()
            ? AZStd::shared_ptr<const RepairProducerInventory>{} : found->second.lock();
        if (!inventory || inventory->pipeline != pipeline || inventory->view != view || inventory->tick != CurrentRenderTick())
            return {};
        return inventory;
    }

    void RegisterResolvePassCreator()
    {
        RegisterRepairPassCreators();
        auto* system = AZ::RPI::PassSystemInterface::Get();
        static AZ::RPI::PassSystemInterface* registered = nullptr;
        if (system && system != registered)
        {
            system->AddPassCreator(AZ::Name("SilPOMResolvePass"), &ResolvePass::Create);
            system->AddPassCreator(AZ::Name("SilPOMHitRasterPass"), &HitRasterPass::Create);
            system->AddPassCreator(AZ::Name("SilPOMFaceCachePass"), &FaceCachePass::Create);
            registered = system;
        }
    }
}

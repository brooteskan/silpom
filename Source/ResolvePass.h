// SPDX-License-Identifier: MIT
#pragma once

#include <AzCore/Math/Aabb.h>
#include <AzCore/Math/Transform.h>
#include <AtomCore/Instance/Instance.h>
#include <Atom/RHI.Reflect/Scissor.h>
#include <Atom/RHI.Reflect/Viewport.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>

namespace AZ::RPI
{
    class MeshDrawPacket;
    class Scene;
    class Buffer;
    class RenderPipeline;
    class View;
    struct PassAttachment;
}

namespace SilPOM
{
    class ResolveBatch;
    using ResolveBatchPtr = AZStd::shared_ptr<ResolveBatch>;

    // Immutable geometry/profile inputs of a published generation. The proxy
    // selection is replaced as one retained snapshot when its material binding
    // changes; view-local cache bindings never enter the shared material.
    struct FaceCacheBatchInputs
    {
        AZ::Data::Instance<AZ::RPI::Buffer> faces, coefficients, proxySelection;
        AZ::u32 meshBufferIndex = 0, proxyBufferIndex = 0, faceCount = 0;
        AZ::u32 reliefMode = 0, profileRole = 0; // SilPOM::ReliefMode, SilPOM::SurfaceRole
        float scale = 0, reference = 0, reliefLodPixelBudget = 0, maxReliefDistance = 0;
        bool proxyLodEnabled = false;
    };

    // Captured from the hit-depth pass's actual enabled/filter-selected list.
    // A missing/incomplete inventory means full-domain repair, never no work.
    // The immutable object retains every packet resource used by its sources.
    struct RepairProducer
    {
        AZ::u64 generation = 0;
        AZ::Aabb localBounds = AZ::Aabb::CreateNull();
        AZ::Transform world = AZ::Transform::CreateIdentity();
        AZ::RHI::Scissor scissor;
        AZStd::shared_ptr<const AZStd::vector<AZ::Aabb>> coverageClusters;
    };

    struct RepairProducerInventory
    {
        const AZ::RPI::RenderPipeline* pipeline = nullptr;
        const AZ::RPI::View* view = nullptr;
        AZ::u64 tick = 0;
        bool complete = false;
        AZ::RHI::Viewport viewport;
        AZ::RHI::Scissor scissor;
        const AZ::RPI::PassAttachment* hitAttachment = nullptr;
        const AZ::RPI::PassAttachment* uvGradientsAttachment = nullptr;
        const AZ::RPI::PassAttachment* depthGradientsAttachment = nullptr;
        const AZ::RPI::PassAttachment* depthAttachment = nullptr;
        AZ::u32 width = 0, height = 0, sampleCount = 0;
        AZStd::vector<RepairProducer> producers;
        AZStd::vector<AZStd::shared_ptr<const void>> sources;
    };

    AZStd::shared_ptr<const RepairProducerInventory> FindRepairProducerInventory(
        const AZ::RPI::RenderPipeline* pipeline, const AZ::RPI::View* view);

    // A registration belongs to one batch of one complete mesh generation.
    // Material assets alone are never used as resolve scheduling identities.
    ResolveBatchPtr CreateResolveBatch(
        AZ::RPI::Scene* scene, AZ::u64 generation, AZ::u32 ownerBase, AZ::u32 ownerCount,
        const AZ::Aabb& localBounds, const AZ::Transform& world, AZStd::vector<AZ::Aabb> coverageClusters,
        AZ::u32 profileRole, bool resolveEligible);
    void SetResolveBatchPublished(const ResolveBatchPtr& batch, bool published);
    void SetResolveBatchWorld(const ResolveBatchPtr& batch, const AZ::Transform& world);
    void SetResolveBatchFaceCacheInputs(const ResolveBatchPtr& batch, const FaceCacheBatchInputs& inputs);
    void SetResolveBatchProxySelection(const ResolveBatchPtr& batch,
        const AZ::Data::Instance<AZ::RPI::Buffer>& buffer, AZ::u32 index);
    void UpdateResolveBatchPacket(const ResolveBatchPtr& batch, const AZ::RPI::MeshDrawPacket& packet);

    void RegisterResolvePassCreator();
}

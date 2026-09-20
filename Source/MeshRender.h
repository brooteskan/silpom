// SPDX-License-Identifier: MIT
#pragma once
#include <Atom/RPI.Public/FeatureProcessor.h>
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <Atom/RPI.Reflect/Image/Image.h>
#include <AzCore/std/containers/array.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Math/Aabb.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/std/smart_ptr/weak_ptr.h>

namespace SilPOM
{
// Bounded first-integration allocation, including pending generations. No
// per-triangle component/material/dispatch. Each batch owns immutable inputs.
struct MeshRenderBatch
{
    AZ::Data::Instance<AZ::RPI::Buffer> geometry;
    AZ::Data::Instance<AZ::RPI::Image> height;
    AZ::Transform rigid = AZ::Transform::CreateIdentity();
    AZ::Aabb bounds = AZ::Aabb::CreateNull();
    AZ::Vector4 surface, uvTransform;
    AZ::u32 counts[4]{};
    AZ::u32 maxNodes = 2048, addressMode = 0, slot = 0;
    AZ::u32 previewBudget = 0, previewStride = 1, previewRays = 0;
    bool registered = true, prepared = false;
    AZ::u64 preparedTick = AZ::u64(-1);
    AZStd::string error;
};
class MeshRenderFeatureProcessor final : public AZ::RPI::FeatureProcessor
{
public:
    AZ_RTTI(MeshRenderFeatureProcessor, "{BF094B62-71CA-424A-9D2E-08E5693494B8}", AZ::RPI::FeatureProcessor);
    AZ_CLASS_ALLOCATOR(MeshRenderFeatureProcessor, AZ::SystemAllocator);
    static constexpr AZ::u32 MaxBatches = 8, MaxViews = 32, MaxRays = 65536;
    static constexpr AZ::u32 HeaderBytes = 16 + MaxViews * 128, RecordBytes = 96;
    static constexpr AZ::u32 BatchBytes = HeaderBytes + MaxRays * RecordBytes;
    static void Reflect(AZ::ReflectContext*);
    void Activate() override;
    void Deactivate() override;
    void AddRenderPasses(AZ::RPI::RenderPipeline*) override;
    void OnRenderPipelineChanged(AZ::RPI::RenderPipeline*, AZ::RPI::SceneNotification::RenderPipelineChangeType) override;
    bool Acquire(const AZStd::shared_ptr<MeshRenderBatch>& batch);
    AZ::u32 GetBindlessIndex() const;
    const auto& GetBatches() const { return m_batches; }
    const auto& GetHits() const { return m_hits; }
private:
    AZ::Data::Instance<AZ::RPI::Buffer> m_hits;
    AZStd::array<AZStd::weak_ptr<MeshRenderBatch>, MaxBatches> m_batches;
};
}

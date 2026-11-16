// SPDX-License-Identifier: MIT
#pragma once
#include <SilPOM/DecalPaint.h>
#include <SilPOM/DecalBakeMath.h>
#include <Atom/RPI.Public/Image/AttachmentImage.h>
#include <Atom/RPI.Public/Image/ImageSystemInterface.h>
#include <Atom/RPI.Public/Image/AttachmentImagePool.h>
#include <Atom/RPI.Public/Pass/FullscreenTrianglePass.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/Pass/PassSystemInterface.h>
#include <Atom/RPI.Reflect/Pass/FullscreenTrianglePassData.h>
#include <AzCore/Math/Vector4.h>
#include <map>
#include <chrono>

namespace SilPOM
{
struct PaintProjection
{
    AZ::Render::DecalData m_data;
    AZ::u32 m_flags=0,m_layer=0,m_layerEndNode=0,m_reserved=0;
    // m_reserved contains the resolved receiver face in the GPU copy; CPU
    // consolidation age is carried separately until that copy is produced.
};
static_assert(sizeof(PaintProjection)==96);
struct PaintBakeRecord
{
    PaintMath::Vec low;AZ::u32 endNode=0;
    PaintMath::Vec inverseSpan;AZ::u32 reserved=0;
    AZ::u32 x=0,y=0,z=0,columns=0;
    AZ::u32 scale=0,bias=0,normal=0,flags=0;
    PaintMath::Vec step;AZ::u32 reserved2=0;
    float inverseWidth=0,inverseHeight=0;AZ::u32 layer=0,receiver=0;
};
static_assert(sizeof(PaintBakeRecord)==96);
class PaintBakeCache
{
public:
    struct Entry
    {
        AZ::u64 key=0;AZ::u32 first=0,end=0,stage=0,age=0,row=0,rows=1;
        PaintMath::Bounds bounds;
        PaintMath::BakeGrid grid;
        PaintBakeRecord record;
        AZ::Data::Instance<AZ::RPI::Buffer> source;
        AZ::Data::Instance<AZ::RPI::ShaderResourceGroup> configured;
        AZ::Data::Instance<AZ::RPI::AttachmentImage> images[4],status;
        AZ::RPI::Ptr<AZ::RPI::FullscreenTrianglePass> pass,finalize;
        void Remove();
    };
    ~PaintBakeCache();
    // Entries are requested in original forward order. Only a fixed prefix of
    // each layer is baked; live strokes and unsupported blends remain ordered.
    void Begin();
    std::vector<Entry*> Prepare(const std::vector<PaintProjection>&,const std::vector<PaintMath::Bounds>&,
        const PaintMath::Bounds&,size_t perEntryBudget,float density,bool enabled,bool create);
    void End();
    bool Tick(AZ::RPI::Scene*,bool idle);
    size_t Count(unsigned stage) const;
    size_t Bytes() const;
    double WallMilliseconds() const{return m_wallMilliseconds;}
private:
    std::map<AZ::u64,Entry> m_entries;
    AZStd::unordered_set<AZ::u64> m_used;
    std::chrono::steady_clock::time_point m_started;
    double m_wallMilliseconds=0;
};
}

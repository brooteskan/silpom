// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Component/Component.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/EBus/EBus.h>
#include <AzCore/std/containers/vector.h>
#include <Atom/Feature/Mesh/MeshFeatureProcessorInterface.h>
#include <Atom/RPI.Public/Pass/Pass.h>

namespace SilPOM
{
// Editor-only fixture and telemetry. No benchmark work runs until requested.
class PlanarBenchmarkRequests : public AZ::EBusTraits
{
public:
    static constexpr AZ::EBusHandlerPolicy HandlerPolicy=AZ::EBusHandlerPolicy::Single;
    virtual bool CreateReference(AZ::EntityId patch,const AZStd::string& material,AZ::u32 cells,float width,float height)=0;
    virtual bool IsReady() const=0;
    virtual void SetReferenceVisible(bool visible)=0;
    virtual void SetOverlay(const AZStd::string& text)=0;
    virtual bool BeginCapture(AZ::u32 samples)=0;
    virtual bool IsCaptureDone() const=0;
    virtual AZStd::string EndCapture()=0;
    virtual void Release()=0;
};
using PlanarBenchmarkRequestBus=AZ::EBus<PlanarBenchmarkRequests>;

class PlanarBenchmark final : public AZ::Component, private AZ::TickBus::Handler,
    private PlanarBenchmarkRequestBus::Handler
{
public:
    AZ_COMPONENT(PlanarBenchmark,"{71C6C6A1-8325-41A2-8AC0-AB49036679B0}");
    static void Reflect(AZ::ReflectContext* context);
    void Activate() override;
    void Deactivate() override;
private:
    bool CreateReference(AZ::EntityId,const AZStd::string&,AZ::u32,float,float) override;
    bool IsReady() const override;
    void SetReferenceVisible(bool) override;
    void SetOverlay(const AZStd::string& text) override {m_overlay=text;}
    bool BeginCapture(AZ::u32) override;
    bool IsCaptureDone() const override {return m_requested!=0 && m_frames>=m_requested;}
    AZStd::string EndCapture() override;
    void Release() override;
    void OnTick(float,AZ::ScriptTimePoint) override;
    struct Query
    {
        AZ::RPI::Ptr<AZ::RPI::Pass> pass;
        bool previouslyEnabled=false;
        AZ::u64 lastBegin=0;
    };
    struct Sample {AZ::u32 frame,pass; AZ::u64 begin,durationTicks,nanoseconds;};
    AZStd::vector<Query> m_queries;
    AZStd::vector<Sample> m_samples;
    AZStd::vector<AZ::u64> m_graphicsSpans;
    AZ::Render::MeshFeatureProcessorInterface* m_processor=nullptr;
    AZ::Render::MeshFeatureProcessorInterface::MeshHandle m_mesh;
    AZ::Data::Asset<AZ::RPI::ModelAsset> m_model;
    AZ::Data::Instance<AZ::RPI::Material> m_material;
    AZ::EntityId m_patch;
    AZStd::string m_overlay;
    AZStd::string m_hardware;
    AZ::u32 m_width=0,m_height=0,m_msaa=0;
    AZ::u32 m_requested=0,m_frames=0,m_warmup=0,m_anchor=0;
};
}

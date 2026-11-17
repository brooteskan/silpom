// SPDX-License-Identifier: MIT
#include "PainterCameraCollision.h"
#include <SilPOM/CameraCollisionMath.h>
#include <SilPOM/Mesh.h>
#include <Atom/RPI.Public/ViewportContext.h>
#include <AtomToolsFramework/Viewport/RenderViewportWidget.h>
#include <AtomLyIntegration/CommonFeatures/Mesh/MeshComponentBus.h>
#include <AzFramework/Viewport/SingleViewportController.h>
#include <AzFramework/Viewport/ViewportControllerList.h>
#include <AzFramework/Physics/PhysicsScene.h>
#include <AzFramework/Physics/Common/PhysicsSimulatedBody.h>
#include <AzFramework/Render/IntersectorInterface.h>
#include <AzToolsFramework/API/EditorCameraBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzToolsFramework/Entity/EditorEntityInfoBus.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/smart_ptr/weak_ptr.h>
#include <QWidget>
#include <array>

namespace SilPOM
{
namespace
{
namespace Math=CameraCollisionMath;
Math::Vec Vector(const AZ::Vector3& p){return {p.GetX(),p.GetY(),p.GetZ()};}
AZ::Vector3 Vector(Math::Vec p){return {float(p.x),float(p.y),float(p.z)};}
constexpr float CameraRadius=.05f;
// Every ray follows a concrete point inside the moving sphere. A finite mesh
// hit is a swept-volume contact, without extending it into an infinite plane.
std::array<Math::Vec,18> CameraSphereSweepOffsets(Math::Vec direction,double radius)
{
    direction=Math::Normalized(direction);
    const auto axis=Math::Normalized(Math::Cross(direction,std::abs(direction.z)<.9?Math::Vec{0,0,1}:Math::Vec{1,0,0}));
    const auto other=Math::Cross(direction,axis);
    constexpr double diagonal=.7071067811865475244;
    constexpr double circle[8][2]={{1,0},{diagonal,diagonal},{0,1},{-diagonal,diagonal},
        {-1,0},{-diagonal,-diagonal},{0,-1},{diagonal,-diagonal}};
    // Center, leading tip, eight mid-latitude points and eight equator points.
    std::array<Math::Vec,18> offsets{};offsets[1]=direction*radius;
    for(unsigned i=0;i<8;++i)
    {
        const auto radial=axis*circle[i][0]+other*circle[i][1];
        offsets[2+i]=radial*(radius*diagonal)+direction*(radius*diagonal);
        offsets[10+i]=radial*radius;
    }
    return offsets;
}
}
class PainterCameraCollision::Controller final : public AzFramework::SingleViewportController
{
public:
    explicit Controller(AZ::RPI::ViewportContextPtr viewport):m_viewport(AZStd::move(viewport))
    {m_state.position=m_viewport->GetCameraTransform().GetTranslation();}
    AzFramework::ViewportControllerPriority GetPriority() const override{return AzFramework::ViewportControllerPriority::Highest;}
    void Configure(bool enabled,AZStd::function<void()> callback)
    {
        if(enabled!=m_enabled)m_state.position=m_viewport->GetCameraTransform().GetTranslation();
        m_enabled=enabled;m_afterUpdate=AZStd::move(callback);m_state.active=enabled;
    }
    void UpdateViewport(const AzFramework::ViewportControllerUpdateEvent&) override
    {
        auto transform=m_viewport->GetCameraTransform();const auto requested=transform.GetTranslation();
        bool gameplay=false;AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(
            gameplay,&AzToolsFramework::EditorEntityContextRequests::IsEditorRunningGame);
        AZ::EntityId cameraEntity;Camera::EditorCameraRequestBus::BroadcastResult(
            cameraEntity,&Camera::EditorCameraRequests::GetCurrentViewEntityId);
        const bool changedView=cameraEntity!=m_previousViewEntity;m_previousViewEntity=cameraEntity;
        m_state.active=m_enabled&&!gameplay&&!cameraEntity.IsValid();m_state.lastBlocked=false;
        if(m_state.active&&!changedView&&requested.IsFinite()&&m_state.position.IsFinite())
        {
            if((requested-m_state.position).GetLengthSq()>1.e-14f)
            {m_state.hitSource=0;m_state.hitFraction=1;m_state.hitEntity.SetInvalid();m_state.hitNormal=AZ::Vector3::CreateZero();}
            const auto movement=Math::Slide(Vector(m_state.position),Vector(requested),CameraRadius,
                [this](Math::Vec from,Math::Vec to,double radius,Math::Hit& hit)
                {++m_state.queryCount;return Sweep(Vector(from),Vector(to),float(radius),hit);});
            if(movement.blocked)
            {
                m_state.lastBlocked=true;++m_state.blockedCount;
                transform.SetTranslation(Vector(movement.position));
                // Normal fly-camera update has finished. This external transform
                // also resets its current/target camera and avoids wall bounce.
                m_viewport->SetCameraTransform(transform);
            }
        }
        m_state.position=m_viewport->GetCameraTransform().GetTranslation();
        if(m_afterUpdate)m_afterUpdate();
    }
    const State& GetState() const{return m_state;}
    const AZ::RPI::ViewportContextPtr& GetViewport() const{return m_viewport;}
private:
    bool Sweep(const AZ::Vector3& start,const AZ::Vector3& end,float radius,Math::Hit& hit)
    {
        bool found=false;
        auto record=[&](AZ::u32 source,float fraction,AZ::EntityId entity,const AZ::Vector3& normal)
        {
            hit.fraction=fraction;hit.normal=Vector(normal);found=true;
            m_state.hitSource=source;m_state.hitFraction=fraction;m_state.hitEntity=entity;m_state.hitNormal=normal;
        };
        MeshRequestBus::EnumerateHandlers([&](MeshRequests* mesh)
        {
            float fraction=float(hit.fraction);AZ::Vector3 normal=AZ::Vector3::CreateZero();
            if(mesh->SweepCameraSphere(start,end,radius,fraction,normal))
                record(1,fraction,MeshRequestBus::GetCurrentBusId()?*MeshRequestBus::GetCurrentBusId():AZ::EntityId{},normal);
            return true;
        });
        const auto delta=end-start;const float length=delta.GetLength();if(length<1.e-7f)return found;
        const auto direction=delta/length;
        if(auto* physics=AZ::Interface<AzPhysics::SceneInterface>::Get())
        {
            const auto scene=physics->GetSceneHandle(AzPhysics::EditorPhysicsSceneName);
            if(scene!=AzPhysics::InvalidSceneHandle)
            {
                auto request=AzPhysics::ShapeCastRequestHelpers::CreateSphereCastRequest(radius,
                    AZ::Transform::CreateTranslation(start),direction,length);
                request.m_hitFlags|=AzPhysics::SceneQuery::HitFlags::MeshBothSides;
                // SiLPOM's canonical sweep preserves hollow walls and openings.
                // A second, coarser collider on the same entity can be convex or
                // approximate and must not fill that correctly modelled space.
                request.m_filterCallback=[](const AzPhysics::SimulatedBody* body,const Physics::Shape*)
                {
                    const auto* mesh=body?MeshRequestBus::FindFirstHandler(body->GetEntityId()):nullptr;
                    return mesh&&mesh->IsReady()?AzPhysics::SceneQuery::QueryHitType::None:AzPhysics::SceneQuery::QueryHitType::Block;
                };
                const auto hits=physics->QueryScene(scene,&request);
                for(const auto& candidate:hits.m_hits)
                {
                    const auto normal=candidate.m_normal.GetNormalizedSafe();
                    const float fraction=std::clamp(candidate.m_distance/length,0.f,1.f);
                    if(fraction<=hit.fraction&&normal.GetLengthSq()>.5f&&normal.Dot(direction)<-1.e-6f)
                        record(2,fraction,candidate.m_entityId,normal);
                }
            }
        }
        // Ordinary visible editor meshes can have no Physics collider. Their
        // render intersector offers rays, so follow actual points of the moving
        // sphere. Extending finite triangles into planes invents contacts near
        // their edges; this engine also reports model-space normals as world
        // normals. Use the hit world point and stop for this fallback instead.
        AzFramework::EntityContextId context;
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(context,
            &AzToolsFramework::EditorEntityContextRequests::GetEditorEntityContextId);
        if(!AzFramework::RenderGeometry::IntersectorBus::HasHandlers(context))return found;
        for(const auto& sample:CameraSphereSweepOffsets(Vector(direction),radius))
        {
            const auto offset=Vector(sample);
            AzFramework::RenderGeometry::RayRequest ray;
            ray.m_startWorldPosition=start+offset;ray.m_endWorldPosition=end+offset;
            ray.m_entityFilter.m_filterFunction=[](AZ::EntityId entity)
            {
                const auto* mesh=MeshRequestBus::FindFirstHandler(entity);if(mesh&&mesh->IsReady())return false;
                // The engine mesh intersector ignores RayRequest::m_onlyVisible.
                // Honor that contract explicitly, including inherited editor
                // hiding and meshes hidden directly through their render bus.
                bool editorVisible=true,meshVisible=true;
                AzToolsFramework::EditorEntityInfoRequestBus::EventResult(editorVisible,entity,
                    &AzToolsFramework::EditorEntityInfoRequests::IsVisible);
                AZ::Render::MeshComponentRequestBus::EventResult(meshVisible,entity,
                    &AZ::Render::MeshComponentRequests::GetVisibility);
                return editorVisible&&meshVisible;
            };
            AzFramework::RenderGeometry::RayResult result;
            AzFramework::RenderGeometry::IntersectorBus::EventResult(result,context,
                &AzFramework::RenderGeometry::IntersectorInterface::RayIntersect,ray);
            if(!result||!result.m_worldPosition.IsFinite())continue;
            const float distance=(result.m_worldPosition-ray.m_startWorldPosition).Dot(direction);
            const float tolerance=std::max(1.e-4f,length*1.e-4f);
            if(distance < -tolerance||distance>length+tolerance)continue;
            const float fraction=std::clamp(distance/length,0.f,1.f);
            if(fraction<=hit.fraction)record(3,fraction,result.m_entityAndComponent.GetEntityId(),-direction);
        }
        return found;
    }
    AZ::RPI::ViewportContextPtr m_viewport;
    AZStd::function<void()> m_afterUpdate;
    State m_state;
    AZ::EntityId m_previousViewEntity;
    bool m_enabled=false;
};
PainterCameraCollision::~PainterCameraCollision(){Release();}
void PainterCameraCollision::Update(const AZ::RPI::ViewportContextPtr& viewport,bool enabled,AZStd::function<void()> afterUpdate)
{
    if(!viewport||(!enabled&&!afterUpdate)){Release();return;}
    if(m_controller&&m_controller->GetViewport()!=viewport)Release();
    if(!m_controller)
    {
        // RenderViewportWidget creates ViewportContext with its own winId().
        auto* widget=QWidget::find(reinterpret_cast<WId>(viewport->GetWindowHandle()));
        auto* render=dynamic_cast<AtomToolsFramework::RenderViewportWidget*>(widget);
        if(!render)return;
        const auto parent=render->GetControllerList();if(!parent)return;
        // This engine revision's Remove uses single-element erase(remove(...))
        // on every priority bucket, including buckets where remove returns end.
        // Keep a private nested list with exactly our one priority/child so its
        // removal is valid. Leave the empty engine-owned list in the parent:
        // it has no gem callback/vtable after Release, and reuse avoids growth.
        struct Group
        {
            AZStd::weak_ptr<AzFramework::ViewportControllerList> parent,nested;
        };
        static AZStd::unordered_map<AzFramework::ViewportId,Group> groups;
        auto& group=groups[viewport->GetId()];
        if(group.parent.lock()==parent)m_list=group.nested.lock();
        if(!m_list)
        {
            m_list=AZStd::make_shared<AzFramework::ViewportControllerList>();
            parent->Add(m_list);group.parent=parent;group.nested=m_list;
        }
        m_controller=AZStd::make_shared<Controller>(viewport);m_list->Add(m_controller);
    }
    m_controller->Configure(enabled,AZStd::move(afterUpdate));
}
void PainterCameraCollision::Release()
{
    if(m_controller)
    {
        m_inactive=m_controller->GetState();
        m_controller->Configure(false,{}); // Clear callbacks capturing the pane first.
    }
    m_inactive.active=false;m_inactive.lastBlocked=false;
    if(m_list&&m_controller)m_list->Remove(m_controller);
    m_controller.reset();m_list.reset();
}
const PainterCameraCollision::State& PainterCameraCollision::GetState() const
{return m_controller?m_controller->GetState():m_inactive;}
}

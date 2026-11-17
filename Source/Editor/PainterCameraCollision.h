// SPDX-License-Identifier: MIT
#pragma once
#include <Atom/RPI.Public/Base.h>
#include <AzCore/Component/EntityId.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/functional.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzFramework/Viewport/ViewportControllerInterface.h>

namespace SilPOM
{
// Transient free-Editor-camera navigation. The final controller runs after the
// normal fly/orbit controller, before rendering, without creating any entities.
class PainterCameraCollision
{
public:
    struct State
    {
        bool active=false,lastBlocked=false;
        AZ::u64 queryCount=0,blockedCount=0;
        AZ::Vector3 position=AZ::Vector3::CreateZero();
        AZ::u32 hitSource=0; // 0 none, 1 canonical SiLPOM, 2 Physics, 3 render geometry.
        float hitFraction=1;
        AZ::EntityId hitEntity;
        AZ::Vector3 hitNormal=AZ::Vector3::CreateZero();
    };
    PainterCameraCollision()=default;
    ~PainterCameraCollision();
    PainterCameraCollision(const PainterCameraCollision&)=delete;
    PainterCameraCollision& operator=(const PainterCameraCollision&)=delete;
    // A callback keeps camera lighting synchronized even with collision disabled.
    void Update(const AZ::RPI::ViewportContextPtr& viewport,bool enabled,AZStd::function<void()> afterUpdate={});
    void Release();
    const State& GetState() const;
    bool IsActive() const{return GetState().active;}
    AZ::u64 GetBlockedCount() const{return GetState().blockedCount;}
    AZ::u64 GetQueryCount() const{return GetState().queryCount;}
private:
    class Controller;
    AZStd::shared_ptr<Controller> m_controller;
    AzFramework::ViewportControllerListPtr m_list;
    State m_inactive;
};
}

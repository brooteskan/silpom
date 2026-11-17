// SPDX-License-Identifier: MIT
#pragma once

#include <Atom/Feature/CoreLights/DiskLightFeatureProcessorInterface.h>
#include <Atom/RPI.Public/Base.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/std/smart_ptr/weak_ptr.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

namespace SilPOM
{
// An editor aid owned by the painter pane, never a saved entity or scene setting.
// All calls, including destruction, belong on the Editor's main thread.
class PainterCameraLight final
    : private AzToolsFramework::EditorEntityContextNotificationBus::Handler
{
public:
    PainterCameraLight();
    ~PainterCameraLight();
    PainterCameraLight(const PainterCameraLight&) = delete;
    PainterCameraLight& operator=(const PainterCameraLight&) = delete;

    // On-axis illuminance at a one-metre reference distance; numerically candela.
    void Update(bool enabled, const AZ::RPI::ScenePtr& scene, const AZ::Transform& camera, float luxAtOneMetre = 10.f);
    void Release();

    bool IsActive() const;
    const AZ::Vector3& GetPosition() const { return m_position; }
    const AZ::Vector3& GetDirection() const { return m_direction; }
    float GetIlluminance() const { return m_illuminance; }

private:
    void OnPrepareForContextReset() override;
    void OnContextReset() override;
    void OnStartPlayInEditorBegin() override;
    void OnStopPlayInEditor() override;

    using Processor = AZ::Render::DiskLightFeatureProcessorInterface;
    AZStd::weak_ptr<AZ::RPI::Scene> m_scene;
    // Identity only: re-query the owning scene before accessing the processor.
    Processor* m_processor = nullptr;
    Processor::LightHandle m_handle;
    AZ::Vector3 m_position = AZ::Vector3::CreateZero();
    AZ::Vector3 m_direction = AZ::Vector3::CreateAxisY();
    float m_illuminance = 10.f;
    bool m_resetting = false;
    bool m_playing = false;
};
}

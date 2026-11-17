// SPDX-License-Identifier: MIT
#include "PainterCameraLight.h"

#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/Scene.h>
#include <algorithm>
#include <cmath>

namespace SilPOM
{
namespace
{
bool RegisteredScene(const AZ::RPI::ScenePtr& scene)
{
    const auto* rpi = AZ::RPI::RPISystemInterface::Get();
    return scene && rpi && rpi->GetScene(scene->GetId()) == scene.get();
}
}

PainterCameraLight::PainterCameraLight()
{
    AzToolsFramework::EditorEntityContextNotificationBus::Handler::BusConnect();
}

PainterCameraLight::~PainterCameraLight()
{
    AzToolsFramework::EditorEntityContextNotificationBus::Handler::BusDisconnect();
    Release();
}

void PainterCameraLight::Update(bool enabled, const AZ::RPI::ScenePtr& scene, const AZ::Transform& camera, float luxAtOneMetre)
{
    const float illuminance = std::isfinite(luxAtOneMetre) ? std::clamp(luxAtOneMetre, 0.f, 1.e6f) : 0.f;
    const bool intensityChanged = illuminance != m_illuminance;
    m_illuminance = illuminance;
    if (!enabled || m_resetting || m_playing || !RegisteredScene(scene) || !camera.IsFinite()
        || camera.GetBasisY().GetLengthSq() < 1.e-8f)
    {
        Release();
        return;
    }

    auto* processor = scene->GetFeatureProcessor<Processor>();
    if (m_scene.lock().get() != scene.get() || processor != m_processor)
    {
        Release();
    }
    if (!processor)
    {
        return;
    }

    bool acquired = false;
    if (!m_handle.IsValid())
    {
        m_handle = processor->AcquireLight();
        if (!m_handle.IsValid())
        {
            return;
        }
        acquired = true;
        m_scene = scene;
        m_processor = processor;
        processor->SetDiskRadius(m_handle, .03f);
        processor->SetAttenuationRadius(m_handle, 20.f);
        processor->SetConeAngles(m_handle, AZ::DegToRad(40.f), AZ::DegToRad(65.f));
        processor->SetShadowsEnabled(m_handle, false);
        processor->SetAffectsGI(m_handle, false);
        processor->SetLightingChannelMask(m_handle, 0xffffffffu);
    }
    if (acquired || intensityChanged)
    {
        processor->SetRgbIntensity(m_handle, AZ::Render::PhotometricColor<Processor::PhotometricUnitType>(
            AZ::Color(m_illuminance, m_illuminance, m_illuminance, 1.f)));
    }

    m_position = camera.GetTranslation();
    // Editor cameras look along local +Y, with local +Z as up.
    m_direction = camera.GetBasisY().GetNormalizedSafe();
    processor->SetPosition(m_handle, m_position);
    processor->SetDirection(m_handle, m_direction);
}

void PainterCameraLight::Release()
{
    if (m_handle.IsValid())
    {
        const auto scene = m_scene.lock();
        // The scene may already have been unregistered/deactivated during shutdown.
        // Its light storage is then gone; never call a cached feature-processor pointer.
        if (RegisteredScene(scene))
        {
            auto* processor = scene->GetFeatureProcessor<Processor>();
            if (processor && processor == m_processor)
            {
                processor->ReleaseLight(m_handle);
            }
        }
    }
    m_handle.Reset();
    m_scene.reset();
    m_processor = nullptr;
}

bool PainterCameraLight::IsActive() const
{
    const auto scene = m_scene.lock();
    return m_handle.IsValid() && RegisteredScene(scene) && scene->GetFeatureProcessor<Processor>() == m_processor;
}

void PainterCameraLight::OnPrepareForContextReset()
{
    m_resetting = true;
    Release();
}

void PainterCameraLight::OnContextReset()
{
    m_resetting = false;
}

void PainterCameraLight::OnStartPlayInEditorBegin()
{
    m_playing = true;
    Release();
}

void PainterCameraLight::OnStopPlayInEditor()
{
    m_playing = false;
}
}


#include <AzCore/Serialization/SerializeContext.h>

#include "SilPOMTowerTestSystemComponent.h"

#include <SilPOMTowerTest/SilPOMTowerTestTypeIds.h>

namespace SilPOMTowerTest
{
    AZ_COMPONENT_IMPL(SilPOMTowerTestSystemComponent, "SilPOMTowerTestSystemComponent",
        SilPOMTowerTestSystemComponentTypeId);

    void SilPOMTowerTestSystemComponent::Reflect(AZ::ReflectContext* context)
    {
        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<SilPOMTowerTestSystemComponent, AZ::Component>()
                ->Version(0)
                ;
        }
    }

    void SilPOMTowerTestSystemComponent::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
    {
        provided.push_back(AZ_CRC_CE("SilPOMTowerTestService"));
    }

    void SilPOMTowerTestSystemComponent::GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& incompatible)
    {
        incompatible.push_back(AZ_CRC_CE("SilPOMTowerTestService"));
    }

    void SilPOMTowerTestSystemComponent::GetRequiredServices([[maybe_unused]] AZ::ComponentDescriptor::DependencyArrayType& required)
    {
    }

    void SilPOMTowerTestSystemComponent::GetDependentServices([[maybe_unused]] AZ::ComponentDescriptor::DependencyArrayType& dependent)
    {
    }

    SilPOMTowerTestSystemComponent::SilPOMTowerTestSystemComponent()
    {
        if (SilPOMTowerTestInterface::Get() == nullptr)
        {
            SilPOMTowerTestInterface::Register(this);
        }
    }

    SilPOMTowerTestSystemComponent::~SilPOMTowerTestSystemComponent()
    {
        if (SilPOMTowerTestInterface::Get() == this)
        {
            SilPOMTowerTestInterface::Unregister(this);
        }
    }

    void SilPOMTowerTestSystemComponent::Init()
    {
    }

    void SilPOMTowerTestSystemComponent::Activate()
    {
        SilPOMTowerTestRequestBus::Handler::BusConnect();
    }

    void SilPOMTowerTestSystemComponent::Deactivate()
    {
        SilPOMTowerTestRequestBus::Handler::BusDisconnect();
    }
}

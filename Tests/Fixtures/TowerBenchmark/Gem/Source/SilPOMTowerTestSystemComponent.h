
#pragma once

#include <AzCore/Component/Component.h>

#include <SilPOMTowerTest/SilPOMTowerTestBus.h>

namespace SilPOMTowerTest
{
    class SilPOMTowerTestSystemComponent
        : public AZ::Component
        , protected SilPOMTowerTestRequestBus::Handler
    {
    public:
        AZ_COMPONENT_DECL(SilPOMTowerTestSystemComponent);

        static void Reflect(AZ::ReflectContext* context);

        static void GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided);
        static void GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& incompatible);
        static void GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& required);
        static void GetDependentServices(AZ::ComponentDescriptor::DependencyArrayType& dependent);

        SilPOMTowerTestSystemComponent();
        ~SilPOMTowerTestSystemComponent();

    protected:
        ////////////////////////////////////////////////////////////////////////
        // SilPOMTowerTestRequestBus interface implementation

        ////////////////////////////////////////////////////////////////////////

        ////////////////////////////////////////////////////////////////////////
        // AZ::Component interface implementation
        void Init() override;
        void Activate() override;
        void Deactivate() override;
        ////////////////////////////////////////////////////////////////////////
    };
}

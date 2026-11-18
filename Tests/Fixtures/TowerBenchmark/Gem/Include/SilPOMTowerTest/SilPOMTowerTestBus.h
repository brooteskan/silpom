
#pragma once

#include <SilPOMTowerTest/SilPOMTowerTestTypeIds.h>

#include <AzCore/EBus/EBus.h>
#include <AzCore/Interface/Interface.h>

namespace SilPOMTowerTest
{
    class SilPOMTowerTestRequests
    {
    public:
        AZ_RTTI(SilPOMTowerTestRequests, SilPOMTowerTestRequestsTypeId);
        virtual ~SilPOMTowerTestRequests() = default;
        // Put your public methods here
    };

    class SilPOMTowerTestBusTraits
        : public AZ::EBusTraits
    {
    public:
        //////////////////////////////////////////////////////////////////////////
        // EBusTraits overrides
        static constexpr AZ::EBusHandlerPolicy HandlerPolicy = AZ::EBusHandlerPolicy::Single;
        static constexpr AZ::EBusAddressPolicy AddressPolicy = AZ::EBusAddressPolicy::Single;
        //////////////////////////////////////////////////////////////////////////
    };

    using SilPOMTowerTestRequestBus = AZ::EBus<SilPOMTowerTestRequests, SilPOMTowerTestBusTraits>;
    using SilPOMTowerTestInterface = AZ::Interface<SilPOMTowerTestRequests>;

} // namespace SilPOMTowerTest

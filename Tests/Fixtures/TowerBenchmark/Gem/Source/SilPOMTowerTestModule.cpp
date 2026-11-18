
#include <AzCore/Memory/SystemAllocator.h>
#include <AzCore/Module/Module.h>

#include "SilPOMTowerTestSystemComponent.h"

#include <SilPOMTowerTest/SilPOMTowerTestTypeIds.h>

namespace SilPOMTowerTest
{
    class SilPOMTowerTestModule
        : public AZ::Module
    {
    public:
        AZ_RTTI(SilPOMTowerTestModule, SilPOMTowerTestModuleTypeId, AZ::Module);
        AZ_CLASS_ALLOCATOR(SilPOMTowerTestModule, AZ::SystemAllocator);

        SilPOMTowerTestModule()
            : AZ::Module()
        {
            // Push results of [MyComponent]::CreateDescriptor() into m_descriptors here.
            m_descriptors.insert(m_descriptors.end(), {
                SilPOMTowerTestSystemComponent::CreateDescriptor(),
            });
        }

        /**
         * Add required SystemComponents to the SystemEntity.
         */
        AZ::ComponentTypeList GetRequiredSystemComponents() const override
        {
            return AZ::ComponentTypeList{
                azrtti_typeid<SilPOMTowerTestSystemComponent>(),
            };
        }
    };
}// namespace SilPOMTowerTest

#if defined(O3DE_GEM_NAME)
AZ_DECLARE_MODULE_CLASS(AZ_JOIN(Gem_, O3DE_GEM_NAME), SilPOMTowerTest::SilPOMTowerTestModule)
#else
AZ_DECLARE_MODULE_CLASS(Gem_SilPOMTowerTest, SilPOMTowerTest::SilPOMTowerTestModule)
#endif

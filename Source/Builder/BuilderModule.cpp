// SPDX-License-Identifier: MIT
#include "MeshSurfaceBuilder.h"
#include "LearnedMaterialBuilder.h"
#include <AzCore/Module/Module.h>
namespace SilPOM
{
class BuilderModule final : public AZ::Module
{
public:
    AZ_RTTI(BuilderModule, "{9C6C394D-01FA-4363-B644-06A130DE40A5}", AZ::Module);
    AZ_CLASS_ALLOCATOR(BuilderModule, AZ::SystemAllocator);
    BuilderModule() { m_descriptors.push_back(MeshSurfaceBuilder::CreateDescriptor());m_descriptors.push_back(LearnedMaterialBuilder::CreateDescriptor()); }
    AZ::ComponentTypeList GetRequiredSystemComponents() const override { return {azrtti_typeid<MeshSurfaceBuilder>(),azrtti_typeid<LearnedMaterialBuilder>()}; }
};
}
AZ_DECLARE_MODULE_CLASS(Gem_SilPOM_Builder, SilPOM::BuilderModule)

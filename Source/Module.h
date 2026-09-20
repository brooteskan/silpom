// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Module/Module.h>
#include <SilPOM/Patch.h>
#include <SilPOM/MeshSurfaceAsset.h>
#include <SilPOM/Mesh.h>
namespace SilPOM
{
class Module : public AZ::Module
{
public:
    AZ_RTTI(Module,"{D1178D96-96F7-49B4-BF2A-07F3E75B6313}",AZ::Module);
    AZ_CLASS_ALLOCATOR(Module,AZ::SystemAllocator);
    Module()
    {
        m_descriptors.push_back(PatchComponent::CreateDescriptor());
        m_descriptors.push_back(MeshAssetSystem::CreateDescriptor());
        m_descriptors.push_back(MeshComponent::CreateDescriptor());
    }
    AZ::ComponentTypeList GetRequiredSystemComponents() const override
    { return {azrtti_typeid<MeshAssetSystem>()}; }
};
}

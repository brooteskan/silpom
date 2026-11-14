// SPDX-License-Identifier: MIT
#pragma once
#include <SilPOM/MaterialCodec.h>
#include <AzCore/Asset/AssetCommon.h>
#include <AzCore/std/containers/vector.h>
#include <Atom/RPI.Public/Image/StreamingImage.h>
namespace SilPOM
{
class LearnedMaterialAsset final : public AZ::Data::AssetData
{
public:
    AZ_RTTI(LearnedMaterialAsset,"{8B20F684-D4D9-47D2-8C98-A8B344F954B1}",AZ::Data::AssetData);
    AZ_CLASS_ALLOCATOR(LearnedMaterialAsset,AZ::SystemAllocator);
    static void Reflect(AZ::ReflectContext*);
    MaterialCodec::Header Header() const;
    bool Validate() const;
    const float* Weights() const {return reinterpret_cast<const float*>(m_bytes.data()+sizeof(MaterialCodec::Header));}
    AZStd::vector<AZ::u8> m_bytes;
    AZStd::string m_generation;
};
AZ::Data::Instance<AZ::RPI::StreamingImage> MakeLearnedMaterialImage(const LearnedMaterialAsset&,AZ::u32 half);
}

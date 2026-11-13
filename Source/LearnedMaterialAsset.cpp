// SPDX-License-Identifier: MIT
#include <SilPOM/LearnedMaterialAsset.h>
#include <SilPOM/AppearanceCodec.h>
#include <SilPOM/ExperimentalCodecs.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AtomCore/Instance/InstanceDatabase.h>
#include <Atom/RPI.Public/Image/ImageSystemInterface.h>
#include <Atom/RPI.Public/Image/StreamingImagePool.h>
#include <Atom/RPI.Reflect/Image/ImageMipChainAssetCreator.h>
#include <Atom/RPI.Reflect/Image/StreamingImageAssetCreator.h>
#include <Atom/RHI.Reflect/ImageSubresource.h>
namespace SilPOM
{
void LearnedMaterialAsset::Reflect(AZ::ReflectContext* context)
{
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context))
        sc->Class<LearnedMaterialAsset,AZ::Data::AssetData>()->Version(1)
            ->Field("Bytes",&LearnedMaterialAsset::m_bytes)->Field("Generation",&LearnedMaterialAsset::m_generation);
}
MaterialCodec::Header LearnedMaterialAsset::Header() const
{
    MaterialCodec::Header h{};if(m_bytes.size()>=sizeof(h))memcpy(&h,m_bytes.data(),sizeof(h));return h;
}
bool LearnedMaterialAsset::Validate() const
{
    if(m_bytes.size()<sizeof(MaterialCodec::Header))return false;
    const auto h=Header();
    if(h.weights>DepthCodec::Weights||m_bytes.size()<sizeof(h)+size_t(h.weights)*4)return false;
    return MaterialCodec::Validate(h,m_bytes.size(),Weights())||AppearanceCodec::Validate(h,m_bytes.size(),Weights())
        ||SharedCodec::Validate(h,m_bytes.size(),Weights())||DepthCodec::Validate(h,m_bytes.size(),Weights());
}
AZ::Data::Instance<AZ::RPI::StreamingImage> MakeLearnedMaterialImage(const LearnedMaterialAsset& asset,AZ::u32 half)
{
    if(!asset.Validate())return {};
    const auto h=asset.Header();
    const AZ::u32 channels=!memcmp(h.magic,"SILPSHR4",8)?12u:8u;
    if(half>=channels/4)return {};
    const auto key=AZStd::string::format("SilPOM.L4:%s:%u",asset.m_generation.c_str(),half);
    const AZ::Uuid id=AZ::Uuid::CreateName(key.c_str());const auto instance=AZ::Data::InstanceId::CreateUuid(id);
    auto& database=AZ::Data::InstanceDatabase<AZ::RPI::StreamingImage>::Instance();
    if(auto existing=database.Find(instance))return existing;
    AZ::RHI::ImageDescriptor desc;
    desc.m_bindFlags=AZ::RHI::ImageBindFlags::ShaderRead;desc.m_dimension=AZ::RHI::ImageDimension::Image2D;
    desc.m_size={h.width,h.height,1};desc.m_format=AZ::RHI::Format::R8G8B8A8_UNORM;desc.m_mipLevels=aznumeric_cast<AZ::u16>(h.mips);
    AZ::Data::Asset<AZ::RPI::ImageMipChainAsset> chain;
    AZ::RPI::ImageMipChainAssetCreator mipCreator;mipCreator.Begin(AZ::Uuid::CreateRandom(),desc.m_mipLevels,1);
    size_t offset=sizeof(h)+size_t(h.weights)*4;AZ::u32 w=h.width,v=h.height;
    for(AZ::u32 mip=0;mip<h.mips;++mip)
    {
        AZStd::vector<AZ::u8> pixels(size_t(w)*v*4);
        for(size_t i=0;i<size_t(w)*v;++i)memcpy(pixels.data()+i*4,asset.m_bytes.data()+offset+i*channels+half*4,4);
        const auto layout=AZ::RHI::GetImageSubresourceLayout(desc,AZ::RHI::ImageSubresource{aznumeric_cast<AZ::u16>(mip),0});
        mipCreator.BeginMip(layout);mipCreator.AddSubImage(pixels.data(),pixels.size());mipCreator.EndMip();
        offset+=size_t(w)*v*channels;w=AZStd::max(1u,w/2);v=AZStd::max(1u,v/2);
    }
    if(!mipCreator.End(chain))return {};
    AZ::Data::Asset<AZ::RPI::StreamingImageAsset> image;
    AZ::RPI::StreamingImageAssetCreator creator;creator.Begin(id);creator.SetImageDescriptor(desc);creator.AddMipChainAsset(*chain);
    creator.SetFlags(AZ::RPI::StreamingImageFlags::NotStreamable);
    creator.SetPoolAssetId(AZ::RPI::ImageSystemInterface::Get()->GetSystemStreamingPool()->GetAssetId());
    if(!creator.End(image))return {};
    return database.FindOrCreate(instance,image);
}
}

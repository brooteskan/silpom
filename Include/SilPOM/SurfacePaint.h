// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Component/EntityId.h>
#include <AzCore/Math/Color.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/std/string/string.h>
#include <cstdint>
#include <vector>

namespace SilPOM
{
// Serialized source paint. The receiver and canonical face identify the
// texture domain; texels is a lossless SurfacePaintImage payload. Neither dabs
// nor the receiver's current material normal are needed to restore this data.
struct SurfacePaintChart
{
    AZ_TYPE_INFO(SurfacePaintChart,"{D16A55EC-C082-42F9-9192-E3F60467144E}");
    AZ::EntityId receiver;
    AZStd::string face;
    AZ::u64 layer=0;
    bool front=true;
    AZ::Vector4 rectangle=AZ::Vector4(0,0,1,1);
    AZ::u32 resolution=256;
    AZStd::string texels;
};
struct SurfacePaintBrush
{
    AZ::Vector3 position=AZ::Vector3::CreateZero();
    AZ::Vector3 normal=AZ::Vector3::CreateAxisZ();
    AZ::Vector3 tangent=AZ::Vector3::CreateAxisX();
    float diameter=.3f,depth=.12f,opacity=1,normalOpacity=0;
    AZ::Color color=AZ::Color::CreateOne();
    AZ::u32 blendMode=0,heightMask=7;
    bool erase=false;
};
// Ephemeral brush input, decoded from authoring images by the editor. No
// source-image or material dependency is needed once affected texels are saved.
struct SurfaceBrushPixels
{
    std::vector<uint8_t> color,normal;
    uint32_t width=0,height=0,normalWidth=0,normalHeight=0;
};
}

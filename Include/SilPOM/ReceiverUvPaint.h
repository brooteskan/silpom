// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Component/EntityId.h>
#include <AzCore/Math/Vector4.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector3.h>
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>

namespace SilPOM
{
// Authoritative stamp address. A missing receiver/face is unresolved legacy
// data, never a projector waiting for a surface to enter a volume.
struct PaintSurfacePoint
{
    AZ_TYPE_INFO(PaintSurfacePoint,"{722A1D3E-91B8-432A-9D4C-F963019DAC11}");
    AZ::EntityId receiver;
    AZStd::string face;
    AZ::Vector2 uv=AZ::Vector2::CreateZero();
    bool front=true;
    bool IsValid() const {return receiver.IsValid()&&!face.empty()&&uv.IsFinite();}
};
struct PaintSurfaceSample
{
    AZ::Vector3 position=AZ::Vector3::CreateZero(),normal=AZ::Vector3::CreateAxisZ();
    AZ::u32 hitFace=0;
};
bool FindPaintSurface(const AZ::Vector3&,const AZ::Vector3&,PaintSurfacePoint&);
// Persistent ownership is receiver + canonical source face. No camera or world
// grid coordinates occur in this key. The UV rectangle is chart-local.
struct ReceiverUvBinding
{
    AZ_TYPE_INFO(ReceiverUvBinding,"{535BA789-B5FA-4908-BB52-F90C86E81604}");
    AZ::EntityId receiver;
    AZStd::string face;
    AZ::Vector4 rectangle=AZ::Vector4(0,0,1,1);
    AZ::u32 resolution=1024;
};
// Immutable, ordered fixed prefix. The existing native batches retain the live
// tail and replay the complete document on receivers which do not own it.
struct ReceiverPaintInput
{
    AZ::Data::Instance<AZ::RPI::Buffer> buffer;
    AZ::u64 revision=0;
    AZ::u32 token=0,fixed=0,live=0;
    AZ::u32 hitFace=0;
    AZStd::string error;
};
struct ReceiverUvMip
{
    AZ::u32 color=0,normalFront=0,normalBack=0,size=0;
};
static_assert(sizeof(ReceiverUvMip)==16);
}

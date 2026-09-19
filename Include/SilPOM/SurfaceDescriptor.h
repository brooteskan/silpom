// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstddef>
namespace SilPOM
{
// Backend-independent wire contract. BLAS/object space is the resolved metre-space
// patch frame. No pointers, entity IDs, raster SRGs, or camera-relative coordinates.
struct SurfaceDescriptor
{
    float width, height, scale, reference;
    float tileU, tileV, offsetU, offsetV;
    std::uint32_t textureWidth, textureHeight, addressMode, maxCells;
};
struct ProceduralPatchDescriptor
{
    SurfaceDescriptor surface;
    std::uint32_t heightTextureIndex;
    std::uint32_t surfaceVersion;
    std::uint32_t generation;
    std::uint32_t reserved;
};
static_assert(sizeof(SurfaceDescriptor)==48);
static_assert(sizeof(ProceduralPatchDescriptor)==64);
static_assert(offsetof(ProceduralPatchDescriptor,heightTextureIndex)==48);
inline constexpr std::uint32_t SurfaceVersion=1;
}

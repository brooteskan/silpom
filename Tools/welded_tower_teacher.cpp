// SPDX-License-Identifier: MIT
#include <SilPOM/FaceNormals.h>
#include "../Tests/BlendedFaceCpu.h"

using namespace SilPOM::Cpu;

namespace
{
float3 ReadVector(const float* p) { return {p[0], p[1], p[2]}; }
}

extern "C" __declspec(dllexport) bool BlendDirections(
    const float* positions, const uint* vertices, const uint* tagged, uint count, float* directions)
{
    std::vector<SilPOM::FaceNormals::Face> faces(count);
    for (uint i = 0; i < count; ++i)
    {
        faces[i].tagged = tagged[i] != 0;
        for (uint j = 0; j < 3; ++j)
        {
            const auto p = ReadVector(positions + i * 9 + j * 3);
            faces[i].vertex[j] = vertices[i * 3 + j];
            faces[i].position[j] = {p.x, p.y, p.z};
        }
    }
    const auto blended = SilPOM::FaceNormals::Build(faces);
    if (!blended.error.empty()) return false;
    for (uint i = 0; i < count; ++i)
        for (uint j = 0; j < 3; ++j)
        {
            const auto d = blended.directions[i][j];
            auto* out = directions + i * 9 + j * 3;
            out[0] = float(d.x);
            out[1] = float(d.y);
            out[2] = float(d.z);
        }
    return true;
}

extern "C" __declspec(dllexport) void TraceRays(
    const float* source, uint width, uint height, float scale, float reference, uint address,
    const float* input, uint faceCount, const float* rays, const uint* ids, uint count, float* output)
{
    struct Surface { SilPomMeshFace face; SilPomPatch patch; float2 low; };
    std::vector<Surface> surfaces(faceCount);
    for (uint i = 0; i < faceCount; ++i)
    {
        const auto* p = input + i * 27;
        auto& s = surfaces[i];
        auto& f = s.face;
        f.origin = ReadVector(p);
        f.du = ReadVector(p + 3);
        f.dv = ReadVector(p + 6);
        f.uv0 = {p[9], p[10]};
        f.uv1 = {p[11], p[12]};
        f.uv2 = {p[13], p[14]};
        f.d0 = ReadVector(p + 15);
        f.d1 = ReadVector(p + 18);
        f.d2 = ReadVector(p + 21);
        f.normal = ReadVector(p + 24);
        s.low = {min(f.uv0.x, min(f.uv1.x, f.uv2.x)), min(f.uv0.y, min(f.uv1.y, f.uv2.y))};
        const float dx = max(f.uv0.x, max(f.uv1.x, f.uv2.x)) - s.low.x;
        const float dy = max(f.uv0.y, max(f.uv1.y, f.uv2.y)) - s.low.y;
        s.patch = {dx, dy, scale, reference, dx, dy, s.low.x, s.low.y, width, height, address, 65536};
    }
    const Texture texture{width, height, std::vector<float>(source, source + size_t(width) * height)};
    for (uint i = 0; i < count; ++i)
    {
        const auto& s = surfaces[ids[i]];
        const auto* p = rays + i * 6;
        const SilPomRay ray{ReadVector(p), ReadVector(p + 3), 0, 150};
        const auto hit = SilPomIntersectFace(texture, s.patch, s.face, ray, s.low);
        output[i * 2] = float(hit.status);
        output[i * 2 + 1] = hit.t;
    }
}

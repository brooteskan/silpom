// Build as a DLL with the same intersection kernel used by the renderer.
#include "../Tests/CpuShader.h"
#include <cstdint>

extern "C" __declspec(dllexport) void TraceStoneWall(
    const float* height, unsigned width, unsigned heightRows,
    const float* rays, unsigned count, float* output)
{
    using namespace SilPOM::Cpu;
    Texture texture{width, heightRows, std::vector<float>(height, height + size_t(width) * heightRows)};
    SilPomPatch patch{2.f, 2.f, .1f, .05000000074505806f, 1.f, 1.f, 0.f, 0.f,
                      width, heightRows, 0, 4096};
    for (unsigned index = 0; index < count; ++index)
    {
        const auto* input = rays + index * 6;
        SilPomRay ray{{input[0], input[1], input[2]}, {input[3], input[4], input[5]}, 0, 1e7f};
        const auto hit = SilPomIntersectMode(texture, patch, ray, true);
        auto* result = output + index * 8;
        result[0] = float(hit.status);
        result[1] = hit.t;
        result[2] = hit.position.x;
        result[3] = hit.position.y;
        result[4] = hit.position.z;
        result[5] = hit.uv.x;
        result[6] = hit.uv.y;
        result[7] = float(hit.cells);
    }
}

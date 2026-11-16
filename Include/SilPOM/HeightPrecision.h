// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>

namespace SilPOM
{
// Caller validates finite, normalized source heights before conversion. Use a
// double intermediate so 16-bit source PNG values survive an R32 round trip.
inline uint16_t EncodeHeightUnorm16(float height)
{
    return static_cast<uint16_t>(std::floor(static_cast<double>(height) * 65535.0 + 0.5));
}
inline float DecodeHeightUnorm16(uint16_t height)
{
    return static_cast<float>(height) / 65535.0f;
}

enum class HeightPixelFormat { Unorm8, Unorm16, Float32, Unsupported };

inline size_t HeightPixelStride(HeightPixelFormat format)
{
    switch (format)
    {
    case HeightPixelFormat::Unorm8: return 1;
    case HeightPixelFormat::Unorm16: return 2;
    case HeightPixelFormat::Float32: return 4;
    default: return 0;
    }
}

// Decode the same normalized source values consumed by the renderer, including
// its optional R32-to-R16 upload conversion. Reject malformed buffers before
// forming a pixel pointer; one-byte heightmaps must never take a two-byte path.
inline bool DecodeHeightPixel(const uint8_t* pixels, size_t byteCount, size_t index,
    HeightPixelFormat format, float& value, bool quantizeFloat32 = false)
{
    const size_t stride = HeightPixelStride(format);
    if (!pixels || !stride || byteCount < stride || index > (byteCount - stride) / stride)
        return false;
    const auto* pixel = pixels + index * stride;
    switch (format)
    {
    case HeightPixelFormat::Unorm8:
        value = float(*pixel) / 255.0f;
        return true;
    case HeightPixelFormat::Unorm16:
    {
        uint16_t encoded;
        std::memcpy(&encoded, pixel, sizeof(encoded));
        value = DecodeHeightUnorm16(encoded);
        return true;
    }
    case HeightPixelFormat::Float32:
        std::memcpy(&value, pixel, sizeof(value));
        if (!std::isfinite(value) || value < 0 || value > 1) return false;
        if (quantizeFloat32) value = DecodeHeightUnorm16(EncodeHeightUnorm16(value));
        return true;
    default: return false;
    }
}
}

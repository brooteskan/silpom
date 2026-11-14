// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include <cstdint>

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
}

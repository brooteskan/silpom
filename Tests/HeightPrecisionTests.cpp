// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/HeightPrecision.h"
#include <iostream>
#include <random>
#include <stdexcept>

int main()
{
    try
    {
        for (uint32_t value = 0; value <= 65535; ++value)
        {
            const float decoded = SilPOM::DecodeHeightUnorm16(static_cast<uint16_t>(value));
            if (SilPOM::EncodeHeightUnorm16(decoded) != value)
                throw std::runtime_error("16-bit source height changed after conversion");
            if (value && decoded <= SilPOM::DecodeHeightUnorm16(static_cast<uint16_t>(value - 1)))
                throw std::runtime_error("Decoded heights are not monotonic");
        }
        std::mt19937 generator(17);
        std::uniform_real_distribution<float> random(0, 1);
        for (int i = 0; i < 1000000; ++i)
        {
            const float height = random(generator);
            const float converted = SilPOM::DecodeHeightUnorm16(SilPOM::EncodeHeightUnorm16(height));
            if (std::abs(double(converted) - height) > .5 / 65535.0 + 3e-8)
                throw std::runtime_error("UNORM16 exceeds the quantization error bound");
        }
        if (SilPOM::EncodeHeightUnorm16(0) != 0 || SilPOM::EncodeHeightUnorm16(1) != 65535)
            throw std::runtime_error("Normalized endpoints changed");
        std::cout << "All 65536 source values and 1000000 normalized heights passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

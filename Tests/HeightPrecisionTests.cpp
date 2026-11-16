// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/HeightPrecision.h"
#include <iostream>
#include <random>
#include <stdexcept>

int main()
{
    try
    {
        const uint8_t bytes[]={0,85,170,255};
        float height;
        for(size_t i=0;i<sizeof(bytes);++i)
            if(!SilPOM::DecodeHeightPixel(bytes,sizeof(bytes),i,SilPOM::HeightPixelFormat::Unorm8,height)
                ||std::abs(height-float(bytes[i])/255.f)>1e-7f)
                throw std::runtime_error("8-bit source height decoded incorrectly");
        for(const size_t index:{sizeof(bytes),std::numeric_limits<size_t>::max()})
            if(SilPOM::DecodeHeightPixel(bytes,sizeof(bytes),index,SilPOM::HeightPixelFormat::Unorm8,height))
                throw std::runtime_error("Out-of-range height read accepted");
        if(SilPOM::DecodeHeightPixel(nullptr,sizeof(bytes),0,SilPOM::HeightPixelFormat::Unorm8,height)
            ||SilPOM::DecodeHeightPixel(bytes,1,0,SilPOM::HeightPixelFormat::Unorm16,height)
            ||SilPOM::DecodeHeightPixel(bytes,sizeof(bytes),0,SilPOM::HeightPixelFormat::Unsupported,height))
            throw std::runtime_error("Malformed height payload accepted");
        const float invalidHeights[]={-.01f,1.01f,std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity()};
        for(const float invalid:invalidHeights)
            if(SilPOM::DecodeHeightPixel(reinterpret_cast<const uint8_t*>(&invalid),sizeof(invalid),0,
                SilPOM::HeightPixelFormat::Float32,height,true))
                throw std::runtime_error("Invalid float height accepted before quantization");
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

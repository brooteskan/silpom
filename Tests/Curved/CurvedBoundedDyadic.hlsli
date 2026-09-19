// SPDX-License-Identifier: MIT
#pragma once
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
namespace SilPOM::Curved::BoundedExact
{
using uint = std::uint32_t;
using std::min;
using std::abs;
inline uint ExactBits(float value) { uint bits; std::memcpy(&bits, &value, sizeof(bits)); return bits; }
inline uint ExactTrailingZeros(uint value) { uint count = 0; while ((value & 1u) == 0) { value >>= 1; ++count; } return count; }
#define SILPOM_EXACT_LOOP
#else
uint ExactBits(float value) { return asuint(value); }
uint ExactTrailingZeros(uint value) { return firstbitlow(value); }
#define SILPOM_EXACT_LOOP [loop]
#endif
struct ExactValue {int mantissa;int exponent;bool valid;};
inline ExactValue ExactNormalize(int mantissa,int exponent,bool valid)
{
    ExactValue result;result.valid=valid;result.mantissa=mantissa;result.exponent=exponent;
    if(mantissa==0){result.exponent=0;return result;}
    uint shift=ExactTrailingZeros(uint(abs(mantissa)));
    result.mantissa=mantissa/int(1u<<shift);result.exponent+=int(shift);return result;
}
inline ExactValue ExactFloat(float value)
{
    uint bits=ExactBits(value),encoded=(bits>>23)&255u;
    int mantissa=int((bits&0x7fffffu)|(encoded==0?0:0x800000u));
    if((bits&0x80000000u)!=0)mantissa=-mantissa;
    return ExactNormalize(mantissa,encoded==0?-149:int(encoded)-150,encoded!=255);
}
inline ExactValue ExactNegate(ExactValue a) {a.mantissa=-a.mantissa;return a;}
inline ExactValue ExactAdd(ExactValue a,ExactValue b)
{
    bool valid=a.valid&&b.valid;
    if(a.mantissa==0){b.valid=valid;return b;}if(b.mantissa==0){a.valid=valid;return a;}
    int exponent=min(a.exponent,b.exponent);
    uint sa=uint(a.exponent-exponent),sb=uint(b.exponent-exponent);
    if(sa>29||sb>29)return ExactNormalize(0,0,false);
    if(uint(abs(a.mantissa))>(0x3fffffffu>>sa)||uint(abs(b.mantissa))>(0x3fffffffu>>sb))
        return ExactNormalize(0,0,false);
    return ExactNormalize(a.mantissa*int(1u<<sa)+b.mantissa*int(1u<<sb),exponent,valid);
}
inline ExactValue ExactSubtract(ExactValue a,ExactValue b) {return ExactAdd(a,ExactNegate(b));}
inline ExactValue ExactMultiply(ExactValue a,ExactValue b)
{
    if(a.mantissa!=0&&uint(abs(b.mantissa))>0x3fffffffu/uint(abs(a.mantissa)))return ExactNormalize(0,0,false);
    return ExactNormalize(a.mantissa*b.mantissa,a.exponent+b.exponent,a.valid&&b.valid);
}
inline ExactValue ExactTriple(ExactValue a,ExactValue b,ExactValue c) {return ExactMultiply(ExactMultiply(a,b),c);}
inline uint ExactGcd(uint a,uint b)
{
    SILPOM_EXACT_LOOP while(b!=0){uint remainder=a%b;a=b;b=remainder;}return a;
}
#undef SILPOM_EXACT_LOOP
#ifdef __cplusplus
}
#endif

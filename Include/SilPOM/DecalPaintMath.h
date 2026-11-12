// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace SilPOM::PaintMath
{
inline unsigned BrushFlags(bool erase,unsigned heightMask){return (erase?1u:0u)|((heightMask&7u)<<8);}
inline bool HeightSelected(unsigned mask,float height)
{
    mask&=7u;if(mask==7)return true;if(!std::isfinite(height)||height<0)return false;
    const unsigned band=height<1.f/3.f?0u:(height<2.f/3.f?1u:2u);
    return (mask&(1u<<band))!=0;
}
struct Vec
{
    float x=0, y=0, z=0;
    Vec operator+(Vec b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vec operator-(Vec b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vec operator*(float s) const { return {x*s,y*s,z*s}; }
    float Length() const { return std::sqrt(x*x+y*y+z*z); }
    Vec Normalized() const { const float l=Length(); return l>1.e-8f ? *this*(1.f/l) : Vec{0,0,1}; }
};
struct Point { Vec position, normal{0,0,1}; float pressure=1; bool start=false; };
inline float Pressure(float p) { return std::isfinite(p) ? std::clamp(p,0.f,1.f) : 0.f; }
inline float Diameter(float diameter,float pressure,bool usePressure,float minimum)
{ return diameter*(usePressure ? std::max(std::clamp(minimum,.01f,1.f),Pressure(pressure)) : 1.f); }
inline Point Lerp(const Point& a,const Point& b,float t)
{ return {a.position+(b.position-a.position)*t,(a.normal+(b.normal-a.normal)*t).Normalized(),a.pressure+(b.pressure-a.pressure)*t,false}; }

// The spacing phase outlives the editable tail of a long gesture. Pruning an
// old dab must never restart spacing or emit a second dab at the cut point.
struct StrokeSampler
{
    Point previous{};
    float remaining=0;
    bool started=false;
    template<class Emit>
    void Push(Point point,Emit onDab,float diameter,float spacing,bool pressureSize,float minimum)
    {
        point.pressure=Pressure(point.pressure);
        auto step=[&](const Point& p){return std::max(.001f,Diameter(diameter,p.pressure,pressureSize,minimum)*std::clamp(spacing,.05f,2.f));};
        if(!started||point.start)
        {
            onDab(point);remaining=step(point);started=true;
        }
        else
        {
            const float length=(point.position-previous.position).Length();
            if(std::isfinite(length)&&length>1.e-8f)
            {
                float travelled=0;
                while(travelled+remaining<=length+1.e-6f)
                {
                    travelled=std::min(length,travelled+remaining);
                    const auto dab=Lerp(previous,point,travelled/length);
                    onDab(dab);remaining=step(dab);
                }
                remaining-=length-travelled;
            }
        }
        previous=point;
    }
};

// Arc-length spacing survives arbitrary event rates and segment boundaries.
// Emit returns false at capacity. Never bridge an explicit gap/off-surface excursion.
template<class Get,class Emit>
size_t Sample(size_t count,Get get,Emit onDab,float diameter,float spacing,bool pressureSize,float minimum,size_t limit)
{
    if(!count || !limit || !std::isfinite(diameter) || diameter<=0 || !std::isfinite(spacing)) return 0;
    size_t emitted=0;
    float remaining=0;
    Point previous{};
    for(size_t i=0;i<count;++i)
    {
        Point point=get(i);
        point.pressure=Pressure(point.pressure);
        if(i==0 || point.start)
        {
            if(!onDab(point)) break;
            if(++emitted==limit) break;
            remaining=std::max(.001f,Diameter(diameter,point.pressure,pressureSize,minimum)*std::clamp(spacing,.05f,2.f));
        }
        else
        {
            const float length=(point.position-previous.position).Length();
            if(std::isfinite(length) && length>1.e-8f)
            {
                float travelled=0;
                while(travelled+remaining<=length+1.e-6f)
                {
                    travelled=std::min(length,travelled+remaining);
                    const Point dab=Lerp(previous,point,travelled/length);
                    if(!onDab(dab)) return emitted;
                    if(++emitted==limit) return emitted;
                    remaining=std::max(.001f,Diameter(diameter,dab.pressure,pressureSize,minimum)*std::clamp(spacing,.05f,2.f));
                }
                remaining-=length-travelled;
            }
        }
        previous=point;
    }
    return emitted;
}
}

// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/MeshSettingRules.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace SilPOM::MeshSettingRules;
static void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void Valid(const Choices& c)
{
    Require(!(c.material&&c.shared),"4A and 4B conflict");
    Require(!(c.material&&c.proxy),"4A and proxy conflict");
    Require(!c.proxyDepth||c.proxy,"5B requires 5A");
    Require(!c.lighting||(c.shared&&c.shadow==3),"Comparison requires shared neural-only");
    Require(!(c.material||c.shared||c.proxy||c.directional||c.proxyDirectional)||c.shadow==3,"Neural-only dependency");
    Require(!c.trained||c.neuralDepth,"Trained profiles require neural depth");
    Require(!(c.bounds&&c.neuralDepth),"Bounds and neural depth conflict");
    Require(!c.diffuseAmbient||(c.lighting>=2&&c.shared&&c.shadow==3),"Ambient requires a hemisphere model");
    Require(Normalize(c)==c,"Normalization must be idempotent");
}
int main()
{
    try
    {
        unsigned transitions=0;
        constexpr bool Choices::* switches[]={&Choices::material,&Choices::shared,&Choices::proxy,&Choices::proxyDepth,
            &Choices::directional,&Choices::proxyDirectional,&Choices::bounds,&Choices::neuralDepth,&Choices::diffuseAmbient};
        for(unsigned bits=0;bits<1024;++bits)for(unsigned shadow=0;shadow<4;++shadow)for(unsigned light=0;light<6;++light)
        {
            Choices raw;raw.material=bits&1;raw.shared=bits&2;raw.proxy=bits&4;raw.proxyDepth=bits&8;
            raw.directional=bits&16;raw.proxyDirectional=bits&32;raw.bounds=bits&64;raw.neuralDepth=bits&128;raw.trained=bits&256;
            raw.shadow=shadow;raw.lighting=light;
            raw.diffuseAmbient=bits&512;
            auto before=Normalize(raw);Valid(before);Require(Edit(before,before)==before,"Unrelated edit changed accepted choices");
            for(auto member:switches)
            {
                auto after=before;after.*member=!(before.*member);const auto result=Edit(before,after);Valid(result);++transitions;
                if(member==&Choices::material&&after.material)Require(result.material&&!result.shared&&!result.proxy&&!result.proxyDepth&&!result.lighting,"Selecting 4A must win");
                if(member==&Choices::shared&&after.shared)Require(result.shared&&!result.material,"Selecting 4B must win");
                if(member==&Choices::proxy&&after.proxy)Require(result.proxy&&!result.material,"Selecting 5A must win");
                if(member==&Choices::proxy&&!after.proxy)Require(!result.proxy&&!result.proxyDepth,"Disabling 5A must disable 5B");
                if(member==&Choices::proxyDepth&&after.proxyDepth)Require(result.proxyDepth&&result.proxy&&!result.material,"Selecting 5B must enable 5A");
                if(member==&Choices::shared&&!after.shared)Require(!result.shared&&!result.lighting,"Disabling shared must clear comparison");
                if(member==&Choices::bounds&&after.bounds&&!after.trained)Require(result.bounds&&!result.neuralDepth,"Selecting legacy bounds must win");
                if(member==&Choices::neuralDepth&&after.neuralDepth)Require(result.neuralDepth&&!result.bounds,"Selecting neural depth must win");
            }
            for(unsigned method=0;method<4;++method)
            {
                auto after=before;after.shadow=method;auto result=Edit(before,after);Valid(result);++transitions;
                Require(result.shadow==method,"Selecting shadow method must win");
            }
            for(unsigned estimator=0;estimator<6;++estimator)
            {
                auto after=before;after.lighting=estimator;auto result=Edit(before,after);Valid(result);++transitions;
                Require(result.lighting==estimator,"Selecting lighting estimator must win");
            }
        }
        Choices approved;approved.shared=approved.proxy=approved.proxyDepth=approved.neuralDepth=approved.trained=true;approved.shadow=3;
        Require(Normalize(approved)==approved,"Approved compatible routing changed");
        approved.material=true;approved.lighting=4;
        auto repaired=Normalize(approved);Require(!repaired.material&&repaired.shared&&repaired.proxy&&repaired.proxyDepth&&repaired.lighting==4,"Repair must retain the user's comparison/proxy choices");
        std::cout<<transitions<<" setting transitions preserve exclusions and dependencies; approved routing unchanged.\n";
        return EXIT_SUCCESS;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
}

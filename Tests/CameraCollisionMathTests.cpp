// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/CameraCollisionMath.h"
#include <cstdio>
#include <cstdlib>
using namespace SilPOM::CameraCollisionMath;
namespace
{
void Require(bool value,const char* message){if(!value){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
bool Near(double a,double b,double tolerance=1.e-7){return std::abs(a-b)<tolerance;}
constexpr Vec A{-10,0,-10},B{10,0,-10},C{0,0,10};
bool Wall(Vec start,Vec end,double radius,Hit& hit){return SweepTriangle(start,end,radius,A,B,C,hit);}
}
int main()
{
    Hit hit;
    Require(Wall({0,-1,0},{0,1000,0},.2,hit)&&Near(hit.fraction,.8/1001)&&Near(hit.normal.y,-1),"Continuous face sweep tunnelled through thin wall");
    hit={};Require(Wall({0,1,0},{0,-1000,0},.2,hit)&&Near(hit.normal.y,1),"Triangle must block from both sides");
    hit={};Require(!Wall({30,-1,0},{30,1,0},.2,hit),"Sphere collided outside triangle footprint");
    hit={};Require(SweepTriangle({-.1,-1,-.1},{-.1,1,-.1},.2,{0,0,0},{2,0,0},{0,0,2},hit),"Sphere must collide with triangle vertex");
    Require(hit.fraction>.4&&hit.fraction<.5&&hit.normal.x<0&&hit.normal.z<0,"Vertex collision normal or fraction is wrong");
    hit={};Require(SweepTriangle({1,-1,-.1},{1,1,-.1},.2,{0,0,0},{2,0,0},{0,0,2},hit)&&hit.normal.z<0,"Sphere must collide with triangle edge");
    hit={};Require(!Wall({0,-.1,0},{0,-1,0},.2,hit),"Initially overlapping camera must be able to retreat");
    hit={};Require(Wall({0,-.1,0},{0,1,0},.2,hit)&&hit.fraction==0,"Initially overlapping camera moved deeper into wall");
    hit={};Require(!Wall({0,-1,0},{0,-1,0},.2,hit),"Stationary sphere should not sweep");
    auto result=Slide({0,-1,0},{2,1,0},.2,Wall);
    Require(result.blocked&&result.queries>=2&&result.position.y<-.2&&Near(result.position.x,2),"Camera should slide along wall and retain clearance");
    auto corner=[](Vec from,Vec to,double radius,Hit& candidate)
    {
        bool found=Wall(from,to,radius,candidate);
        return SweepTriangle(from,to,radius,{1,-10,-10},{1,10,-10},{1,0,10},candidate)||found;
    };
    result=Slide({0,-1,0},{2,1,0},.2,corner);
    Require(result.blocked&&result.position.y<-.2&&result.position.x<.8,"Sliding camera crossed a corner");
    result=Slide({0,-1,0},{0,-2,0},.2,Wall);
    Require(!result.blocked&&Near(result.position.y,-2),"Unobstructed camera movement changed");
    hit={};Require(!SweepTriangle({0,-1,0},{0,1,0},.2,{0,0,0},{0,0,0},{0,0,0},hit),"Degenerate triangle must be ignored");
    std::puts("Camera sphere sweep, two-sided wall, edge, vertex, tunnelling, retreat and sliding tests passed");
}

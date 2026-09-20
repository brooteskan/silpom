// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace SilPOM::FaceNormals
{
struct Vector
{
    double x=0, y=0, z=0;
    Vector operator+(Vector b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vector operator-(Vector b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vector operator*(double s) const { return {x*s,y*s,z*s}; }
    double Dot(Vector b) const { return x*b.x+y*b.y+z*b.z; }
    Vector Cross(Vector b) const { return {y*b.z-z*b.y,z*b.x-x*b.z,x*b.y-y*b.x}; }
    double Length() const { return std::sqrt(Dot(*this)); }
    Vector Unit() const { return *this*(1/Length()); }
};
struct Face
{
    std::array<std::uint32_t,3> vertex;
    std::array<Vector,3> position;
    bool tagged=false;
};
struct Result
{
    std::vector<std::array<Vector,3>> directions;
    std::string error;
    unsigned blendedEdges=0;
};

// Logical identities, not positions or render vertices, define connectivity.
// Union CORNERS through tagged/tagged edges: disconnected fans that merely
// touch at a vertex must not influence one another. Materials, UVs and profile
// IDs deliberately do not participate in the smoothing decision.
inline Result Build(const std::vector<Face>& faces)
{
    Result result; result.directions.resize(faces.size());
    std::vector<unsigned> parent(faces.size()*3);
    for(unsigned i=0;i<parent.size();++i) parent[i]=i;
    auto root=[&](unsigned i) { while(parent[i]!=i) { parent[i]=parent[parent[i]]; i=parent[i]; } return i; };
    struct Edge { unsigned face,corner; bool reverse; };
    std::map<std::pair<unsigned,unsigned>,std::vector<Edge>> edges;
    std::vector<Vector> normals(faces.size());
    for(unsigned f=0;f<faces.size();++f)
    {
        const auto& face=faces[f];
        Vector n=(face.position[1]-face.position[0]).Cross(face.position[2]-face.position[0]);
        if(!std::isfinite(n.Length()) || n.Length()<1e-12) { result.error="Degenerate face in normal preprocessing"; return result; }
        normals[f]=n.Unit();
        for(unsigned c=0;c<3;++c)
        {
            unsigned a=face.vertex[c],b=face.vertex[(c+1)%3];
            edges[{std::min(a,b),std::max(a,b)}].push_back({f,c,a>b});
        }
    }
    for(const auto& entry:edges)
    {
        const auto& e=entry.second;
        if(e.size()>2 || (e.size()==2 && e[0].reverse==e[1].reverse))
        { result.error="Nonmanifold or inconsistently oriented edge in normal preprocessing"; return result; }
        if(e.size()!=2 || !faces[e[0].face].tagged || !faces[e[1].face].tagged) continue;
        ++result.blendedEdges;
        for(unsigned k=0;k<2;++k)
        {
            unsigned a=e[0].face*3+(e[0].corner+k)%3;
            unsigned b=e[1].face*3+(e[1].corner+1-k)%3;
            parent[root(b)]=root(a);
        }
    }
    std::vector<Vector> sums(parent.size());
    for(unsigned f=0;f<faces.size();++f) for(unsigned c=0;c<3;++c)
    {
        const auto& p=faces[f].position;
        Vector a=(p[(c+1)%3]-p[c]).Unit(),b=(p[(c+2)%3]-p[c]).Unit();
        // atan2 is stable for both narrow and nearly straight corner angles.
        double angle=std::atan2(a.Cross(b).Length(),a.Dot(b));
        unsigned r=root(f*3+c); sums[r]=sums[r]+normals[f]*angle;
    }
    for(unsigned f=0;f<faces.size();++f) for(unsigned c=0;c<3;++c)
    {
        Vector n=sums[root(f*3+c)];
        if(n.Length()<1e-8) { result.error="Tagged fan has cancelling normals"; return result; }
        n=n.Unit();
        // A generated direction must stay in the face's outward hemisphere.
        // Reject an unsupported fold rather than silently insert a hard seam.
        if(faces[f].tagged && n.Dot(normals[f])<.25)
        { result.error="Tagged fold exceeds supported normal-blending hemisphere"; return result; }
        result.directions[f][c]=n;
    }
    return result;
}
}

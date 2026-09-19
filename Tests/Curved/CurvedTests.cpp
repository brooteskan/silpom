// SPDX-License-Identifier: MIT
#include "CurvedPrototype.h"
#include "CurvedExactReference.h"
#include "CurvedPacking.h"
#include "CurvedBoundedDyadic.hlsli"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>

using namespace SilPOM::Curved;

namespace
{
int g_checks = 0;

void Require(bool condition, const std::string& message)
{
    ++g_checks;
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

Texture MakeTexture(bool curved)
{
    Texture texture{ 4, 4, std::vector<double>(16, 0.5) };
    if (curved)
    {
        texture.pixels = {
            0.00, 0.25, 0.75, 1.00,
            0.20, 0.90, 0.10, 0.80,
            0.80, 0.10, 0.90, 0.20,
            1.00, 0.75, 0.25, 0.00
        };
    }
    return texture;
}

std::vector<Triangle> MakeTriangles()
{
    const Vec3 a{ -1, -1, 0 };
    const Vec3 b{ 0, -1, 0 };
    const Vec3 c{ 0, 1, 0 };
    const Vec3 d{ 1, -1, 0.35 };
    const Vec3 up{ 0, 0, 1 };
    const Vec3 tilted{ 0, 0.6, 0.8 };
    const Vec3 tiltedX{ 0.6, 0, 0.8 };
    Triangle left{ { a, b, c }, { Vec2{ 0, 0 }, Vec2{ .5, 0 }, Vec2{ .5, 1 } },
        { up, tilted, tiltedX }, 7 };
    Triangle right{ { b, d, c }, { Vec2{ .5, 0 }, Vec2{ 1, 0 }, Vec2{ .5, 1 } },
        { tilted, up, tiltedX }, 11 };
    return { left, right };
}

Ray Through(Vec3 point, Vec3 direction, double distance = 2.0)
{
    return { point - direction * distance, direction, 0.0, 4.0 };
}

void ContractTests()
{
    auto triangles = MakeTriangles();
    const Texture flat = MakeTexture(false);
    const Texture curved = MakeTexture(true);
    Surface surface{ 1.0, 0.65, 0.5, 1 };

    Hit hit = Intersect(triangles, surface, flat, { { -.4, -.2, 1 }, { 0, 0, -1 }, 0, 4 });
    Require(hit.status == Status::Hit, "flat two-triangle front hit");
    Require(std::abs(hit.tUpper - 1.0) < 1e-6, "flat hit distance");
    Require(hit.primitiveId == 7, "flat hit primitive identity");
    Require(std::abs(Length(hit.normal) - 1.0) < 1e-10, "unit geometric normal");

    Require(Intersect(triangles, surface, flat, { { -3, 0, 1 }, { 0, 0, -1 }, 0, 4 }).status == Status::Miss,
        "finite mesh miss");
    Require(Intersect(triangles, surface, flat, { { -.4, -.2, 1 }, { 0, 0, -1 }, 0, .5 }).status == Status::Miss,
        "finite ray interval miss");

    auto invalid = triangles;
    invalid[0].direction[0] = { 0, 0, 2 };
    Require(Intersect(invalid, surface, flat, { { 0, 0, 1 }, { 0, 0, -1 }, 0, 4 }).status == Status::Invalid,
        "non-unit corner direction is invalid");

    const Vec3 sharedPoint = Evaluate(triangles[0], surface, curved, { 0, .5, .5 }).position;
    const Ray sharedRay{ sharedPoint + Vec3{ 0, 0, 1 }, { 0, 0, -1 }, 0, 4 };
    hit = Intersect(triangles, surface, curved, sharedRay);
    if (hit.status != Status::Hit)
    {
        std::cerr << std::setprecision(17) << "shared-edge diagnostic: status=" << uint(hit.status) << " nodes=" << hit.subdivisionNodes
                  << " depth=" << hit.maximumDepth << " interval=[" << hit.tLower << ',' << hit.tUpper
                  << "] primitive=" << hit.primitiveId << " unresolved=[" << hit.lowestUnresolvedT << ','
                  << hit.highestUnresolvedT << "]\n";
    }
    Require(hit.status == Status::Hit, "shared-edge hit");
    Require(hit.primitiveId == 7, "shared-edge ownership uses lower primitive id");
    const Exact::Result exactShared = Exact::IntersectAll(triangles, surface, curved, sharedRay);
    Require(exactShared.status == Exact::Status::Complete && !exactShared.roots.empty(),
        "exact oracle includes the shared-triangle-edge root");
    Require(exactShared.roots.front().primitiveId == 7, "exact shared-edge ownership uses lower primitive id");

    const Vec3 b{ .30, .30, .40 };
    const Sample sample = Evaluate(triangles[0], surface, curved, b);
    const Vec3 expectedDerivative = sample.du;
    const double step = 1e-6;
    const Sample before = Evaluate(triangles[0], surface, curved, { b.x + step, b.y - step, b.z });
    const Sample after = Evaluate(triangles[0], surface, curved, { b.x - step, b.y + step, b.z });
    const Vec3 finiteDifference = (after.position - before.position) / (2.0 * step);
    Require(Length(finiteDifference - expectedDerivative) < 2e-5,
        "surface derivative includes height and direction variation");

    Limits exhaustedLimits;
    exhaustedLimits.maxNodes = 1;
    Hit exhausted = Intersect(triangles, surface, curved, { { -.4, -.2, 1 }, { .2, .05, -1 }, 0, 4 }, exhaustedLimits);
    Require(exhausted.status == Status::Exhausted, "budget exhaustion differs from miss");
    Require(std::isfinite(exhausted.lowestUnresolvedT), "exhaustion records nearest unresolved interval");
    Require((exhausted.exhaustionReasons & ExhaustionNodeBudget) != 0, "budget exhaustion has an explicit reason");
    auto invalidTexture = flat;
    for (double value : { -0.1, 1.1, std::numeric_limits<double>::quiet_NaN() })
    {
        invalidTexture.pixels[0] = value;
        Require(Intersect(triangles, surface, invalidTexture, sharedRay).status == Status::Invalid,
            "invalid height data is rejected before exact arithmetic");
    }
    Limits invalidLimits;
    invalidLimits.tTolerance = std::numeric_limits<double>::quiet_NaN();
    Require(Intersect(triangles, surface, flat, sharedRay, invalidLimits).status == Status::Invalid,
        "non-finite error budget is invalid");
}

void BoundarySweepTests()
{
    const Texture texture = MakeTexture(false);
    auto triangles = MakeTriangles();
    for (auto& triangle : triangles) for (auto& direction : triangle.direction) direction = { 0, 0, 1 };
    uint rays = 0;
    for (double scale : { .125, 1.0, 8.0 }) for (double amplitude : { -.5, .5 })
    {
        const Surface surface{ scale, amplitude, .25, 0 };
        for (size_t primitive = 0; primitive != triangles.size(); ++primitive)
            for (int edge = 0; edge != 3; ++edge) for (int i = 0; i <= 8; ++i)
        {
            Vec3 bary{}; bary[edge] = 1.0 - double(i) / 8; bary[(edge + 1) % 3] = double(i) / 8;
            const auto point = Evaluate(triangles[primitive], surface, texture, bary).position;
            const Ray ray{ point + Vec3{ 0, 0, 2 }, { 0, 0, -1 }, 0, 4 };
            const Hit hit = Intersect(triangles, surface, texture, ray);
            Require(hit.status == Status::Hit && hit.rationalCertificates != 0, "closed edge/vertex has an exact rational certificate");
            Require(hit.tLower <= 2 + 4e-15 && hit.tUpper >= 2 - 4e-15, "closed boundary depth");
            const uint expected = primitive == 0 || point.x == 0.0 ? 7 : 11;
            Require(hit.primitiveId == expected, "shared-edge ownership remains stable along the full edge");
            auto reversed = triangles; std::reverse(reversed.begin(), reversed.end());
            const Hit reverse = Intersect(reversed, surface, texture, ray);
            Require(reverse.status == hit.status && reverse.primitiveId == hit.primitiveId &&
                reverse.tLower == hit.tLower && reverse.tUpper == hit.tUpper, "boundary ownership is independent of candidate order");
            ++rays;
        }
        // UV texel boundaries include both sides of a closed cell domain.
        for (double u : { .125, .375 }) for (double v : { .125, .375, .625 })
        {
            const Vec3 bary{ 1 - 2 * u, 2 * u - v, v };
            if (!Contains(bary, 0)) continue;
            const auto point = Evaluate(triangles[0], surface, texture, bary).position;
            const Hit hit = Intersect(triangles, surface, texture, { point + Vec3{ 0, 0, 2 }, { 0, 0, -1 }, 0, 4 });
            Require(hit.status == Status::Hit && hit.rationalCertificates != 0, "closed texel-boundary certificate");
            ++rays;
        }
    }
    Interval::Range third;
    Require(RationalEnclosure(Exact::Dyadic(1.0), Exact::Dyadic(3.0), third) && third.lo < third.hi,
        "non-dyadic rational gets an outward depth enclosure");
    const auto coefficients = Exact::AffineCorners(1e16, 1.0, -1.0);
    Require((coefficients.coefficient[1][0] - (Exact::Dyadic(1.0) - Exact::Dyadic(1e16))).IsZero(),
        "oracle does not round corner subtraction before exact conversion");
    std::cout << "Curved boundary sweeps: " << rays << " edge, vertex and texel-boundary rays.\n";
}

void BoundedExactTests()
{
    using namespace BoundedExact;
    auto asDyadic = [](ExactValue value) { return Exact::Dyadic(Exact::Integer(value.mantissa), value.exponent); };
    auto check = [&](ExactValue actual, const Exact::Dyadic& expected)
    {
        // A bounded proof may decline an operation. Every accepted operation
        // must agree exactly with the arbitrary-size independent arithmetic.
        if (actual.valid) Require((asDyadic(actual) - expected).IsZero(), "bounded shader arithmetic is exact when accepted");
    };
    std::mt19937 random(0xD1AD1Cu);
    for (int i = 0; i != 2048; ++i)
    {
        std::uint32_t bitsA = random() & 0xfeffffffu, bitsB = random() & 0xfeffffffu;
        float a, b; std::memcpy(&a, &bitsA, sizeof(a)); std::memcpy(&b, &bitsB, sizeof(b));
        const ExactValue ea = ExactFloat(a), eb = ExactFloat(b);
        const Exact::Dyadic da{ double(a) }, db{ double(b) };
        check(ea, da); check(eb, db);
        check(ExactAdd(ea, eb), da + db); check(ExactSubtract(ea, eb), da - db);
        check(ExactMultiply(ea, eb), da * db);
        check(ExactSubtract(ea, ea), Exact::Dyadic{});
    }
    Require(!ExactMultiply(ExactFloat(16777215.f), ExactFloat(16777215.f)).valid, "integer multiplication overflow declines the proof");
    Require(!ExactAdd(ExactFloat(1.f), ExactFloat(std::ldexp(1.f, -40))).valid, "excessive exponent alignment declines the proof");
    Require(!ExactMultiply(ExactNormalize(0, 0, false), ExactFloat(0)).valid, "invalid exact arithmetic stays invalid through multiplication by zero");
    Require(ExactGcd(177147, 177147) == 177147 && ExactGcd(0, 27) == 27, "rational proof removes common odd factors");
}

void TangentTest()
{
    Texture texture{ 8, 8, std::vector<double>(64) };
    for (uint y = 0; y != texture.height; ++y)
        for (uint x = 0; x != texture.width; ++x)
            texture.pixels[size_t(y) * texture.width + x] = double(x) / 8.0;
    const Vec3 up{ 0, 0, 1 };
    const Vec3 tilted{ 1, 0, 0 };
    std::vector<Triangle> triangles{
        Triangle{ { Vec3{ 0, 0, 0 }, Vec3{ 1, 0, 0 }, Vec3{ 0, 1, 0 } },
            { Vec2{ .125, .125 }, Vec2{ .375, .125 }, Vec2{ .125, .375 } }, { up, tilted, up }, 3 },
        Triangle{ { Vec3{ 1, 0, 0 }, Vec3{ 2, 0, .25 }, Vec3{ 0, 1, 0 } },
            { Vec2{ .375, .125 }, Vec2{ .625, .125 }, Vec2{ .125, .375 } }, { tilted, up, up }, 5 }
    };
    Surface surface{ 1.0, 0.5, 0.5, 0 };
    const Vec3 barycentric{ .5, .25, .25 };
    const Sample contact = Evaluate(triangles[0], surface, texture, barycentric);
    Ray tangent = Through(contact.position, contact.du, 1.0);
    tangent.tMax = 2.0;
    Limits limits;
    limits.maxNodes = 1048576;
    limits.maxDepth = 40;
    limits.parameterTolerance = 1e-11;
    limits.tTolerance = 1e-12;
    Hit hit = Intersect(triangles, surface, texture, tangent, limits);
    const Exact::Result exact = Exact::IntersectAll(triangles, surface, texture, tangent);
    const auto tangentReference = ReferenceAll(triangles, surface, texture, tangent, 512);
    if (hit.status != Status::Hit)
    {
        std::cerr << std::setprecision(17) << "tangent diagnostic: status=" << uint(hit.status)
                  << " nodes=" << hit.subdivisionNodes << " depth=" << hit.maximumDepth
                  << " hit=[" << hit.tLower << ',' << hit.tUpper << "] primitive=" << hit.primitiveId
                  << " unresolved=[" << hit.lowestUnresolvedT << ',' << hit.highestUnresolvedT << "]\n";
    }
    Require(hit.status == Status::Hit && hit.rationalCertificates != 0, "exact quadratic certificate resolves the tangent");
    Require(hit.tUpper - hit.tLower <= 2 * limits.tTolerance, "tangent respects the requested depth budget");
    Require(hit.subdivisionNodes < 32, "exact tangent does not consume a subdivision budget");
    Require(hit.singularPath, "tangent candidate uses the singular-root path");
    if (!(hit.tLower <= 1.0 && hit.tUpper >= 1.0))
    {
        std::cerr << std::setprecision(17) << "tangent hit interval=[" << hit.tLower << ',' << hit.tUpper
                  << "] primitive=" << hit.primitiveId << " singular=" << hit.singularPath
                  << " bary=(" << hit.barycentric.x << ',' << hit.barycentric.y << ',' << hit.barycentric.z
                  << ") reference=" << (tangentReference.empty() ? -1.0 : tangentReference.front().first) << '\n';
    }
    Require(hit.tLower <= 1.0 && hit.tUpper >= 1.0, "tangent t enclosure contains constructed contact");
    Require(exact.status == Exact::Status::Complete, "exact tangent oracle completes");
    if (exact.roots.empty() || std::abs(exact.roots.front().t - 1.0) >= 1e-8)
    {
        const auto projection = MakeProjection(tangent.direction);
        const auto system = Exact::BuildSystem(triangles[0], surface, texture, tangent, projection, 1, 1);
        const auto resultantX = Exact::Resultant(system.first, system.second, true);
        const auto resultantY = Exact::Resultant(system.first, system.second, false);
        const auto rootsX = Exact::IsolateUnitRoots(resultantX);
        const auto rootsY = Exact::IsolateUnitRoots(resultantY);
        std::cerr << "exact tangent roots=" << exact.roots.size();
        std::cerr << " resultants=" << resultantX.size() << ',' << resultantY.size()
                  << " coordinates=" << rootsX.size() << ',' << rootsY.size()
                  << " signs-x=" << Exact::Evaluate(resultantX, 0.0).Sign() << ','
                  << Exact::Evaluate(resultantX, .25).Sign() << ',' << Exact::Evaluate(resultantX, 1.0).Sign();
        for (const auto& root : rootsX) std::cerr << " x[" << root.lower << ',' << root.upper << ']';
        for (const auto& root : rootsY) std::cerr << " y[" << root.lower << ',' << root.upper << ']';
        for (const auto& root : exact.roots)
            std::cerr << " [t=" << std::setprecision(17) << root.t << " b=" << root.barycentric.x << ','
                      << root.barycentric.y << ',' << root.barycentric.z << "]";
        std::cerr << '\n';
    }
    Require(!exact.roots.empty() && std::abs(exact.roots.front().t - 1.0) < 1e-8,
        "exact resultant/Sturm oracle retains the repeated tangent root");
    Ray clipped = tangent;
    clipped.tMin = clipped.tMax = 1.0;
    Require(Intersect(triangles, surface, texture, clipped).status == Status::Hit, "closed finite interval contains tangent endpoint");
    clipped.tMin = std::nextafter(1.0, 2.0); clipped.tMax = 2.0;
    Require(Intersect(triangles, surface, texture, clipped).status == Status::Miss, "tangent outside near endpoint is a miss");
    clipped.tMin = 0.0; clipped.tMax = std::nextafter(1.0, 0.0);
    Require(Intersect(triangles, surface, texture, clipped).status == Status::Miss, "tangent outside far endpoint is a miss");
    for (int exponent : { 8, 12, 16, 20 }) for (int sign : { -1, 1 })
    {
        Ray nearby = tangent;
        nearby.origin.z += sign * std::ldexp(1.0, -exponent);
        const Hit actual = Intersect(triangles, surface, texture, nearby);
        const auto reference = Exact::IntersectAll(triangles, surface, texture, nearby);
        Require(reference.status == Exact::Status::Complete, "near-tangent exact reference completes");
        Require(actual.status == (reference.roots.empty() ? Status::Miss : Status::Hit), "near-tangent hit/miss classification");
        if (!reference.roots.empty())
        {
            Require(!actual.singularPath, "two distinct near-tangent roots are not merged into a tangent");
            Require(reference.roots.front().t >= actual.tLower - 2e-10 && reference.roots.front().t <= actual.tUpper + 2e-10,
                "near-tangent nearest root enclosure");
        }
    }
    Limits insufficient = limits;
    insufficient.maxNodes = 0;
    const Hit unfinished = Intersect(triangles, surface, texture, tangent, insufficient);
    Require(unfinished.status == Status::Exhausted && (unfinished.exhaustionReasons & ExhaustionNodeBudget) != 0,
        "zero tangent work budget remains explicit exhaustion");
}

void ReferenceAndFuzzTests()
{
    const Texture texture = MakeTexture(true);
    auto triangles = MakeTriangles();
    Surface surface{ 1.0, 0.35, 0.5, 0 };
    std::mt19937 random(0x51A0u);
    std::uniform_real_distribution<double> xy(-1.15, 1.15);
    std::uniform_real_distribution<double> slope(-.45, .45);
    int compared = 0;
    int hits = 0;
    int exactCompared = 0;
    uint maximumNodes = 0;
    for (int i = 0; i != 300; ++i)
    {
        Ray ray{ { xy(random), xy(random), 1.2 }, { slope(random), slope(random), -1.0 }, 0, 4 };
        const auto coarse = ReferenceAll(triangles, surface, texture, ray, 96);
        const auto fine = ReferenceAll(triangles, surface, texture, ray, 192);
        if (coarse.empty() != fine.empty() || (!coarse.empty() && std::abs(coarse.front().first - fine.front().first) > 8e-5))
        {
            continue;
        }
        ++compared;
        const Hit actual = Intersect(triangles, surface, texture, ray);
        maximumNodes = std::max(maximumNodes, actual.subdivisionNodes);
        Require(actual.status != Status::Invalid, "valid fuzz input is not invalid");
        if (actual.status == Status::Exhausted)
        {
            std::cerr << std::setprecision(17) << "fuzz exhaustion case=" << i << " nodes=" << actual.subdivisionNodes
                      << " depth=" << actual.maximumDepth << " unresolved=[" << actual.lowestUnresolvedT << ','
                      << actual.highestUnresolvedT << "] hit=[" << actual.tLower << ',' << actual.tUpper
                      << "] reference=" << (fine.empty() ? -1.0 : fine.front().first) << '\n';
        }
        Require(actual.status != Status::Exhausted, "bounded fuzz corpus completes");
        Require((actual.status == Status::Hit) == !fine.empty(), "candidate/reference hit classification");
        if (!fine.empty())
        {
            ++hits;
            Require(std::abs((actual.tLower + actual.tUpper) * .5 - fine.front().first) < 3e-4,
                "candidate/reference nearest distance");
        }
        if (exactCompared < 12)
        {
            const Exact::Result exact = Exact::IntersectAll(triangles, surface, texture, ray);
            Require(exact.status == Exact::Status::Complete, "exact fuzz oracle completes");
            Require((actual.status == Status::Hit) == !exact.roots.empty(), "exact oracle hit classification");
            if (!exact.roots.empty())
            {
                const Exact::Root& reference = exact.roots.front();
                Require(reference.t >= actual.tLower - 2e-8 && reference.t <= actual.tUpper + 2e-8,
                    "exact nearest distance is enclosed");
                Require(reference.primitiveId == actual.primitiveId, "exact primitive ownership");
                Require(Length(reference.barycentric - actual.barycentric) < 2e-6, "exact barycentric coordinates");
                Require(std::abs(reference.uv.x - actual.uv.x) < 2e-6 &&
                    std::abs(reference.uv.y - actual.uv.y) < 2e-6, "exact UV coordinates");
                Require(Dot(reference.normal, actual.normal) > 1.0 - 2e-8, "exact geometric normal");
            }
            ++exactCompared;
        }
    }
    Require(compared >= 250, "reference refinement converges for the fuzz corpus");
    Require(exactCompared == 12, "exact oracle covers the deterministic fuzz prefix");
    Require(hits >= 40, "fuzz corpus contains useful hits");
    std::cout << "Curved fuzz: " << compared << " converged rays, " << hits << " hits, " << exactCompared
              << " exact resultant/Sturm comparisons, max "
              << maximumNodes << " subdivision nodes.\n";
}

void PackingTests()
{
    auto triangles=MakeTriangles();
    const Texture texture=MakeTexture(true);
    const Ray ray{{2,-1,.5},{.25,.5,-1},0,10};
    const auto projection=MakeProjection(ray.direction);
    for(uint address: {0u,1u})for(double scale: {.125,1.,8.})for(double amplitude: {-.35,.35})
    {
        const Surface surface{scale,amplitude,.5,address};
        const PackedMesh packed=PackMesh(triangles,surface,texture);
        const PackedMesh bounded=PackMesh(triangles,surface,texture,1048576,true);
        Require(bounded.data.size()==bounded.triangleCount*PackedTriangleStride+bounded.fragmentCount*5u,
            "bounded compact representation has an 80-byte fragment stride");
        Require(packed.data.size()==packed.triangleCount*PackedTriangleStride+packed.fragmentCount*PackedFragmentStride,
            "compact representation has shared triangle and bounded fragment strides");
        auto twice=packed;
        twice.data=packed.ReversedFragments();
        const auto reversedTwice=twice.ReversedFragments();
        Require(std::memcmp(reversedTwice.data(),packed.data.data(),packed.data.size()*sizeof(PackedFloat4))==0,
            "reversing fragments preserves triangle table and all fragment fields");
        size_t fragmentIndex=0;
        for(const Triangle& triangle:triangles)for(const Fragment& fragment:BuildFragments(triangle,texture))
        {
            const auto algebraic=AnalyticControls(fragment,surface,texture);
            const auto sampled=CubicControls(fragment,surface,texture,ray,projection);
            for(size_t i=0;i!=10;++i)
            {
                const auto actual=Project(ray,projection,algebraic[i]);
                Require(std::abs(actual.x-sampled[i].x)<2e-12&&std::abs(actual.y-sampled[i].y)<2e-12&&
                    std::abs(actual.t-sampled[i].t)<2e-12,"analytic controls match independent sampled reconstruction");
            }
            const size_t offset=bounded.triangleCount*PackedTriangleStride+fragmentIndex++*5;
            const auto& packedLow=bounded.data[offset+3];const auto& packedHigh=bounded.data[offset+4];
            const Vec3 lo{packedLow.z,packedLow.w,packedHigh.x},hi{packedHigh.y,packedHigh.z,packedHigh.w};
            for(Vec3 bary: {Vec3{1,0,0},Vec3{0,1,0},Vec3{0,0,1},Vec3{.2,.3,.5},Vec3{.5,.5,0}})
            {
                const auto point=Evaluate(triangle,surface,texture,DomainPoint(fragment.domain,bary)).position;
                Require(point.x>=lo.x&&point.y>=lo.y&&point.z>=lo.z&&point.x<=hi.x&&point.y<=hi.y&&point.z<=hi.z,
                    "cached bounds enclose the surface under signed displacement and resolved scale");
            }
        }
    }
    bool rejected=false;
    try {PackMesh(triangles,Surface{1,.35,.5,0},texture,1);}catch(const std::invalid_argument&){rejected=true;}
    Require(rejected,"preprocessing rejects excessive cell expansion before allocation");
    auto invalidTexture=texture;invalidTexture.pixels[0]=std::numeric_limits<double>::quiet_NaN();
    rejected=false;
    try {PackMesh(triangles,Surface{1,.35,.5,0},invalidTexture);}catch(const std::invalid_argument&){rejected=true;}
    Require(rejected,"packing rejects non-finite height data");
}

void CostSmoke()
{
    const Texture texture = MakeTexture(true);
    const auto triangles = MakeTriangles();
    Surface surface{ 1.0, 0.35, 0.5, 0 };
    std::vector<Ray> rays;
    for (int y = 0; y != 32; ++y)
    {
        for (int x = 0; x != 32; ++x)
        {
            const Triangle& triangle = triangles[(x + y) & 1];
            const double u = (x + .5) / 64.0;
            const double v = (y + .5) / 64.0;
            const Vec3 barycentric{ 1.0 - u - v, u, v };
            const Vec3 point = Evaluate(triangle, surface, texture, barycentric).position;
            const Vec3 direction{ 1.0, .03, .08 };
            rays.push_back(Through(point, direction, 2.0));
        }
    }
    const auto begin = std::chrono::steady_clock::now();
    uint hits = 0;
    uint exhausted = 0;
    std::vector<size_t> exhaustedIndices;
    std::uint64_t nodes = 0;
    std::vector<uint> nodeCounts;
    std::vector<Hit> actualHits;
    Limits grazingLimits;
    grazingLimits.maxNodes=131072;
    grazingLimits.maxDepth=32;
    const std::vector<size_t> formerExhaustions{ 148, 150, 152, 154, 156, 158, 171, 173, 175, 177, 960 };
    for (size_t rayIndex = 0; rayIndex != rays.size(); ++rayIndex)
    {
        const Ray& ray = rays[rayIndex];
        const Hit hit = Intersect(triangles, surface, texture, ray,grazingLimits);
        if (hit.status == Status::Exhausted)
        {
            exhaustedIndices.push_back(rayIndex);
            std::cerr << std::setprecision(17) << "grazing case=" << rayIndex << " nodes=" << hit.subdivisionNodes
                << " certificates=" << hit.regularCertificates << " hit=[" << hit.tLower << ',' << hit.tUpper
                << "] unresolved=[" << hit.lowestUnresolvedT << ',' << hit.highestUnresolvedT << "]\n";
            Require((hit.exhaustionReasons & ExhaustionUncertifiedLeaf) != 0,
                "grazing exhaustion identifies an uncertified leaf");
            Require((hit.exhaustionReasons & ExhaustionParameterResolution) != 0,
                "grazing exhaustion identifies the parameter-resolution boundary");
        }
        hits += hit.status == Status::Hit;
        exhausted += hit.status == Status::Exhausted;
        nodes += hit.subdivisionNodes;
        nodeCounts.push_back(hit.subdivisionNodes);
        actualHits.push_back(hit);
    }
    const auto end = std::chrono::steady_clock::now();
    for (size_t rayIndex : formerExhaustions)
    {
        const Ray& ray = rays[rayIndex];
        const Hit& hit = actualHits[rayIndex];
        const auto exact = Exact::IntersectAll(triangles, surface, texture, ray);
        Require(exact.status == Exact::Status::Complete && !exact.roots.empty(), "exact grazing reference completes");
        Require(hit.status == Status::Hit && hit.regularCertificates != 0, "former grazing exhaustion has interval inclusion");
        const auto& root = exact.roots.front();
        Require(root.t >= hit.tLower - 2e-11 && root.t <= hit.tUpper + 2e-11, "grazing nearest depth enclosure");
        Require(hit.tUpper - hit.tLower <= 2 * grazingLimits.tTolerance, "grazing depth budget");
        Require(hit.primitiveId == root.primitiveId, "grazing primitive identity");
        Require(Length(hit.barycentric - root.barycentric) < 2e-8, "grazing barycentric accuracy");
        Require(std::abs(hit.uv.x - root.uv.x) < 2e-8 && std::abs(hit.uv.y - root.uv.y) < 2e-8,
            "grazing UV accuracy");
        Require(Dot(hit.normal, root.normal) > 1.0 - 2e-12, "grazing normal accuracy");
        auto reversed = triangles;
        std::reverse(reversed.begin(), reversed.end());
        const Hit reverse = Intersect(reversed, surface, texture, ray, grazingLimits);
        Require(reverse.status == hit.status && reverse.primitiveId == hit.primitiveId &&
            reverse.tLower <= hit.tUpper && reverse.tUpper >= hit.tLower, "grazing candidate order independence");
        for (uint budget : { 0u, 1u, 2u, 4u, 8u, 16u, 32u })
        {
            auto limited = grazingLimits; limited.maxNodes = budget;
            const Hit bounded = Intersect(triangles, surface, texture, ray, limited);
            Require(bounded.status == Status::Hit || bounded.status == Status::Exhausted, "limited valid grazing ray never becomes a miss");
            if (bounded.status == Status::Hit)
                Require(root.t >= bounded.tLower - 2e-11 && root.t <= bounded.tUpper + 2e-11, "budget-limited nearest hit remains enclosed");
            else
                Require(std::isfinite(bounded.lowestUnresolvedT) && bounded.highestUnresolvedT >= bounded.lowestUnresolvedT &&
                    (bounded.exhaustionReasons & ExhaustionNodeBudget) != 0, "budget-limited pending intervals remain diagnosed");
        }
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(end - begin).count();
    std::sort(nodeCounts.begin(), nodeCounts.end());
    const uint p95 = nodeCounts[size_t(nodeCounts.size() * .95)];
    const uint p99 = nodeCounts[size_t(nodeCounts.size() * .99)];
    const std::vector<size_t> expectedUnsupported{};
    if (exhaustedIndices != expectedUnsupported)
    {
        std::cerr << "grazing exhausted indices:";
        for (size_t index : exhaustedIndices) std::cerr << ' ' << index;
        std::cerr << '\n';
    }
    Require(exhaustedIndices == expectedUnsupported, "the entire required grazing corpus completes");
    std::cout << std::fixed << std::setprecision(3) << "Curved CPU grazing smoke: " << rays.size() << " rays in "
              << milliseconds << " ms, " << hits << " hits, " << exhausted << " exhausted, "
              << double(nodes) / rays.size() << " mean nodes/ray, p95=" << p95 << ", p99=" << p99 << ".\n";
}
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--grazing")
    {
        CostSmoke();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--tangent")
    {
        TangentTest();
        std::cout << "Tangent checks: " << g_checks << '\n';
        return 0;
    }
    BoundedExactTests();
    ContractTests();
    BoundarySweepTests();
    TangentTest();
    ReferenceAndFuzzTests();
    PackingTests();
    CostSmoke();
    std::cout << "SilPOM curved prototype: " << g_checks << " checks passed.\n";
}

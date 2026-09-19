// SPDX-License-Identifier: MIT
// Bounded exact dyadics. Overflow means unsupported, never rounded acceptance.
// This path needs only ordinary 32-bit integer shader operations.
#include "CurvedBoundedDyadic.hlsli"
struct ExactLinear {ExactValue a;ExactValue u;ExactValue v;};
ExactLinear ExactCorners(float a,float b,float c)
{
    ExactLinear result;result.a=ExactFloat(a);result.u=ExactSubtract(ExactFloat(b),result.a);
    result.v=ExactSubtract(ExactFloat(c),result.a);return result;
}
ExactLinear ExactLinearAdd(ExactLinear a,ExactLinear b)
{
    ExactLinear result;result.a=ExactAdd(a.a,b.a);result.u=ExactAdd(a.u,b.u);result.v=ExactAdd(a.v,b.v);return result;
}
ExactLinear ExactLinearScale(ExactLinear a,float scale)
{
    ExactValue s=ExactFloat(scale);a.a=ExactMultiply(a.a,s);a.u=ExactMultiply(a.u,s);a.v=ExactMultiply(a.v,s);return a;
}
struct ExactPolynomial {ExactValue value[6];}; // 1,u,v,uu,uv,vv
ExactPolynomial ExactSurfaceCoordinate(ExactLinear base,ExactLinear direction,ExactLinear height)
{
    ExactPolynomial result;
    result.value[0]=ExactAdd(base.a,ExactMultiply(height.a,direction.a));
    result.value[1]=ExactAdd(base.u,ExactAdd(ExactMultiply(height.a,direction.u),ExactMultiply(height.u,direction.a)));
    result.value[2]=ExactAdd(base.v,ExactAdd(ExactMultiply(height.a,direction.v),ExactMultiply(height.v,direction.a)));
    result.value[3]=ExactMultiply(height.u,direction.u);
    result.value[4]=ExactAdd(ExactMultiply(height.u,direction.v),ExactMultiply(height.v,direction.u));
    result.value[5]=ExactMultiply(height.v,direction.v);return result;
}
ExactPolynomial ExactProject(ExactPolynomial first,ExactPolynomial major,float origin,float majorOrigin,float direction,float majorDirection)
{
    first.value[0]=ExactSubtract(first.value[0],ExactFloat(origin));
    major.value[0]=ExactSubtract(major.value[0],ExactFloat(majorOrigin));
    ExactPolynomial result;
    [unroll]for(uint i=0;i!=6;++i)result.value[i]=ExactSubtract(ExactMultiply(first.value[i],ExactFloat(majorDirection)),
        ExactMultiply(major.value[i],ExactFloat(direction)));
    return result;
}
bool ExactIsLinear(ExactPolynomial p)
{
    return p.value[3].mantissa==0&&p.value[4].mantissa==0&&p.value[5].mantissa==0;
}
bool ExactPolynomialValid(ExactPolynomial p)
{
    bool valid=true;[unroll]for(uint i=0;i!=6;++i)valid=valid&&p.value[i].valid;return valid;
}
bool ExactClosedUnit(ExactValue value,ExactValue denominator)
{
    ExactValue remaining=ExactSubtract(denominator,value);
    return value.valid&&denominator.valid&&remaining.valid&&value.mantissa>=0&&remaining.mantissa>=0;
}
bool ExactRatio(ExactValue numerator,ExactValue denominator,out float2 enclosure)
{
    enclosure=0;
    if(!numerator.valid||!denominator.valid||denominator.mantissa==0)return false;
    int power=numerator.exponent-denominator.exponent;
    if(power< -120||power>120)return false;
    // Integer-to-float conversion may round. Enclose it separately from division
    // and the exact power-of-two scale. The denominator conversion is enclosed
    // too, rather than treated as an exact float constant.
    float n=float(numerator.mantissa),d=float(denominator.mantissa);
    float2 ni=float2(ProofDown(n),ProofUp(n)),di=float2(ProofDown(d),ProofUp(d));
    if(di.x<=0&&di.y>=0)return false;
    precise float low=1.0f/di.y,high=1.0f/di.x;
    float2 reciprocal=float2(ProofDown(low),ProofUp(high));
    float scale=asfloat(uint(power+127)<<23);
    enclosure=ProofMultiply(ProofMultiply(ni,reciprocal),scale.xx);
    return all(isfinite(enclosure));
}

bool ExactLinearRange(ExactValue constant,ExactValue slope,ExactValue denominator,float2 u,out float2 range)
{
    float2 a,b;range=0;
    if(!ExactRatio(constant,denominator,a)||!ExactRatio(slope,denominator,b))return false;
    // Preserve exact zeros: outward arithmetic otherwise obscures closed cell
    // boundaries even when this expression is identically zero or one.
    if(slope.mantissa==0)
    {
        if(constant.mantissa==0){range=0;return true;}
        ExactValue difference=ExactSubtract(constant,denominator);
        if(difference.valid&&difference.mantissa==0){range=1;return true;}
    }
    range=ProofAdd(a,ProofMultiply(b,u));return all(isfinite(range));
}

uint ProofQuadraticPair(Fragment f,Ray ray,Projection projection,ExactPolynomial position,
    ExactValue a,ExactValue b,ExactValue c,ExactValue discriminant,ExactValue n0,ExactValue d,
    ExactLinear x,ExactLinear y,out float3 barycentric,out float2 depth)
{
    barycentric=0;depth=0;
    float2 disc,bi,denominator,vr;
    if(!ExactRatio(discriminant,ExactFloat(1),disc)||disc.x<=0||
        !ExactRatio(b,ExactFloat(1),bi)||!ExactRatio(ExactMultiply(ExactFloat(2),a),ExactFloat(1),denominator)||
        !ExactRatio(n0,d,vr))return 0;
    if(denominator.x<=0&&denominator.y>=0)return 0;
    // sqrt is only a locator. Squaring outward-rounded endpoints proves the
    // enclosure without assuming an accuracy guarantee for the instruction.
    float estimate=sqrt((disc.x+disc.y)*.5f);
    float2 root=float2(ProofDown(estimate),ProofUp(estimate));bool enclosed=false;
    [loop]for(uint i=0;i!=16;++i)
    {
        float2 lowSquare=ProofMultiply(root.xx,root.xx),highSquare=ProofMultiply(root.yy,root.yy);
        if(root.x>=0&&lowSquare.y<=disc.x&&highSquare.x>=disc.y){enclosed=true;break;}
        root=float2(max(0,ProofDown(root.x)),ProofUp(root.y));
    }
    if(!enclosed)return 0;
    precise float reciprocalLow=1/denominator.y,reciprocalHigh=1/denominator.x;
    float2 reciprocal=float2(ProofDown(reciprocalLow),ProofUp(reciprocalHigh));
    ExactValue positiveD=d,positiveN=n0;
    if(positiveD.mantissa<0){positiveD=ExactNegate(positiveD);positiveN=ExactNegate(positiveN);}
    ExactValue remaining=ExactSubtract(positiveD,positiveN);
    if(!remaining.valid)return 0;
    if(positiveN.mantissa<0||remaining.mantissa<0)return 2;
    ExactValue tc=ExactAdd(ExactAdd(ExactTriple(position.value[0],d,d),ExactTriple(position.value[2],d,n0)),ExactTriple(position.value[5],n0,n0));
    ExactValue tb=ExactAdd(ExactTriple(position.value[1],d,d),ExactTriple(position.value[4],d,n0));
    ExactValue ta=ExactTriple(position.value[3],d,d);
    ExactValue td=ExactMultiply(d,d);
    float2 ci,li,qi;
    if(!ExactRatio(tc,td,ci)||!ExactRatio(tb,td,li)||!ExactRatio(ta,td,qi))return 0;
    bool found=false;
    [unroll]for(uint side=0;side!=2;++side)
    {
        float2 u=ProofMultiply(ProofAdd(-bi.yx,side==0?-root.yx:root),reciprocal);
        float2 w=ProofSubtract(ProofSubtract(float2(1,1),u),vr);
        if(u.y<0||u.x>1||w.y<0||w.x>1)continue;
        if(u.x<0||u.y>1||w.x<0||w.y>1)return 0;
        float2 cx,cy;
        if(!ExactLinearRange(ExactAdd(ExactMultiply(x.a,d),ExactMultiply(x.v,n0)),ExactMultiply(x.u,d),d,u,cx)||
            !ExactLinearRange(ExactAdd(ExactMultiply(y.a,d),ExactMultiply(y.v,n0)),ExactMultiply(y.u,d),d,u,cy))return 0;
        if(cx.y<0||cx.x>1||cy.y<0||cy.x>1)continue;
        if(cx.x<0||cx.y>1||cy.x<0||cy.y>1)return 0;
        float2 hit=ProofDivide(ProofSubtract(ProofAdd(ci,ProofMultiply(u,ProofAdd(li,ProofMultiply(qi,u)))),
            ray.origin[projection.major].xx),ray.direction[projection.major]);
        if(hit.y<ray.tMin||hit.x>ray.tMax)continue;
        if(any(!isfinite(hit))||hit.x<ray.tMin||hit.y>ray.tMax||hit.y-hit.x>requiredDepthAccuracy||
            max(u.y-u.x,vr.y-vr.x)>5e-5f)return 0;
        if(found&&hit.x<=depth.y&&hit.y>=depth.x)return 0; // distinct roots cannot be silently merged
        if(!found||hit.y<depth.x)
        {
            found=true;depth=hit;float ru=(u.x+u.y)*.5f,rv=(vr.x+vr.y)*.5f;
            barycentric=float3(1-ru-rv,ru,rv);
        }
    }
    return found?1:2;
}

// 0: outside supported exact arithmetic; 1: all reduced roots classified,
// nearest root returned; 2: all roots excluded. The proof covers the entire
// fixed-cell polynomial. Distinct quadratic roots currently require constant v.
uint ProofRational(Fragment f,Ray ray,Projection projection,out float3 barycentric,out float2 depth,out bool singular)
{
    barycentric=0;depth=0;singular=false;
    if(f.height.w!=0)return 0;
    ExactLinear x=ExactLinearScale(ExactCorners(f.uv[0].x,f.uv[1].x,f.uv[2].x),float(textureWidth));
    ExactLinear y=ExactLinearScale(ExactCorners(f.uv[0].y,f.uv[1].y,f.uv[2].y),float(textureHeight));
    x.a=ExactSubtract(ExactSubtract(x.a,ExactFloat(.5f)),ExactFloat(f.cell.x));
    y.a=ExactSubtract(ExactSubtract(y.a,ExactFloat(.5f)),ExactFloat(f.cell.y));
    ExactLinear h=ExactLinearAdd(ExactLinearScale(x,f.height.y),ExactLinearScale(y,f.height.z));
    h.a=ExactAdd(h.a,ExactSubtract(ExactFloat(f.height.x),ExactFloat(reference)));h=ExactLinearScale(h,amplitude);
    ExactPolynomial positions[3];
    [unroll]for(uint axis=0;axis!=3;++axis)
        positions[axis]=ExactSurfaceCoordinate(ExactLinearScale(ExactCorners(f.position[0][axis],f.position[1][axis],f.position[2][axis]),baseScale),
            ExactCorners(f.direction[0][axis],f.direction[1][axis],f.direction[2][axis]),h);
    ExactPolynomial constraint=ExactProject(positions[projection.first],positions[projection.major],ray.origin[projection.first],
        ray.origin[projection.major],ray.direction[projection.first],ray.direction[projection.major]);
    ExactPolynomial q=ExactProject(positions[projection.second],positions[projection.major],ray.origin[projection.second],
        ray.origin[projection.major],ray.direction[projection.second],ray.direction[projection.major]);
    if(!ExactPolynomialValid(constraint)||!ExactPolynomialValid(q))return 0;
    if(!ExactIsLinear(constraint)){ExactPolynomial temporary=constraint;constraint=q;q=temporary;}
    if(!ExactIsLinear(constraint))return 0;
    bool swapped=constraint.value[2].mantissa==0;
    if(swapped)
    {
        ExactValue temporary=constraint.value[1];constraint.value[1]=constraint.value[2];constraint.value[2]=temporary;
        temporary=q.value[1];q.value[1]=q.value[2];q.value[2]=temporary;
        temporary=q.value[3];q.value[3]=q.value[5];q.value[5]=temporary;
    }
    ExactValue d=constraint.value[2],n0=ExactNegate(constraint.value[0]),n1=ExactNegate(constraint.value[1]);
    if(d.mantissa==0)return 0;
    ExactValue a=ExactAdd(ExactAdd(ExactTriple(q.value[3],d,d),ExactTriple(q.value[4],d,n1)),ExactTriple(q.value[5],n1,n1));
    ExactValue b=ExactAdd(ExactAdd(ExactTriple(q.value[1],d,d),ExactTriple(q.value[2],d,n1)),
        ExactAdd(ExactTriple(q.value[4],d,n0),ExactMultiply(ExactFloat(2),ExactTriple(q.value[5],n0,n1))));
    ExactValue c=ExactAdd(ExactAdd(ExactTriple(q.value[0],d,d),ExactTriple(q.value[2],d,n0)),ExactTriple(q.value[5],n0,n0));
    if(!a.valid||!b.valid||!c.valid)return 0;
    uint polynomialCommon=ExactGcd(ExactGcd(uint(abs(a.mantissa)),uint(abs(b.mantissa))),uint(abs(c.mantissa)));
    if(polynomialCommon>1){a.mantissa/=int(polynomialCommon);b.mantissa/=int(polynomialCommon);c.mantissa/=int(polynomialCommon);}
    ExactValue numerator,denominator;
    if(a.mantissa==0)
    {
        if(b.mantissa==0)return c.mantissa==0?0:2;
        numerator=ExactNegate(c);denominator=b;
    }
    else
    {
        ExactValue discriminant=ExactSubtract(ExactMultiply(b,b),ExactTriple(ExactFloat(4),a,c));
        if(!discriminant.valid)return 0;
        if(discriminant.mantissa<0)return 2;
        if(discriminant.mantissa!=0)
        {
            if(swapped||n1.mantissa!=0)return 0;
            return ProofQuadraticPair(f,ray,projection,positions[projection.major],a,b,c,discriminant,n0,d,x,y,barycentric,depth);
        }
        numerator=ExactNegate(b);denominator=ExactMultiply(ExactFloat(2),a);singular=true;
    }
    ExactValue u=ExactMultiply(numerator,d),v=ExactAdd(ExactMultiply(n0,denominator),ExactMultiply(n1,numerator));
    denominator=ExactMultiply(denominator,d);
    if(swapped){ExactValue temporary=u;u=v;v=temporary;}
    if(denominator.mantissa<0){denominator=ExactNegate(denominator);u=ExactNegate(u);v=ExactNegate(v);}
    if(!u.valid||!v.valid||!denominator.valid||denominator.mantissa==0)return 0;
    uint common=ExactGcd(ExactGcd(uint(abs(u.mantissa)),uint(abs(v.mantissa))),uint(denominator.mantissa));
    if(common>1){u.mantissa/=int(common);v.mantissa/=int(common);denominator.mantissa/=int(common);}
    ExactValue w=ExactSubtract(ExactSubtract(denominator,u),v);
    ExactValue cx=ExactAdd(ExactAdd(ExactMultiply(x.a,denominator),ExactMultiply(x.u,u)),ExactMultiply(x.v,v));
    ExactValue cy=ExactAdd(ExactAdd(ExactMultiply(y.a,denominator),ExactMultiply(y.u,u)),ExactMultiply(y.v,v));
    if(!u.valid||!v.valid||!w.valid||!cx.valid||!cy.valid||!denominator.valid)return 0;
    // Check validity before interpreting a failed closed-domain test as a miss.
    if(!ExactSubtract(denominator,cx).valid||!ExactSubtract(denominator,cy).valid||
        !ExactSubtract(denominator,u).valid||!ExactSubtract(denominator,v).valid||!ExactSubtract(denominator,w).valid)return 0;
    if(!ExactClosedUnit(u,denominator)||!ExactClosedUnit(v,denominator)||!ExactClosedUnit(w,denominator)||
        !ExactClosedUnit(cx,denominator)||!ExactClosedUnit(cy,denominator))return 2;
    ExactPolynomial t=positions[projection.major];
    t.value[0]=ExactSubtract(t.value[0],ExactFloat(ray.origin[projection.major]));
    ExactValue nt=ExactAdd(ExactAdd(ExactTriple(t.value[0],denominator,denominator),ExactTriple(t.value[1],u,denominator)),
        ExactAdd(ExactTriple(t.value[2],v,denominator),ExactAdd(ExactTriple(t.value[3],u,u),
        ExactAdd(ExactTriple(t.value[4],u,v),ExactTriple(t.value[5],v,v)))));
    ExactValue dt=ExactTriple(denominator,denominator,ExactFloat(ray.direction[projection.major]));
    if(dt.mantissa<0){nt=ExactNegate(nt);dt=ExactNegate(dt);}
    if(!nt.valid||!dt.valid)return 0;
    ExactValue nearTest=ExactSubtract(nt,ExactMultiply(ExactFloat(ray.tMin),dt));
    ExactValue farTest=ExactFloat(0);
    if(isfinite(ray.tMax))farTest=ExactSubtract(ExactMultiply(ExactFloat(ray.tMax),dt),nt);
    if(!nearTest.valid||!farTest.valid)return 0;
    if(nearTest.mantissa<0||farTest.mantissa<0)return 2;
    float2 rootU,rootV;
    if(!ExactRatio(u,denominator,rootU)||!ExactRatio(v,denominator,rootV)||!ExactRatio(nt,dt,depth))return 0;
    if(max(rootU.y-rootU.x,rootV.y-rootV.x)>5e-5f||depth.y-depth.x>requiredDepthAccuracy)return 0;
    float ru=(rootU.x+rootU.y)*.5f,rv=(rootV.x+rootV.y)*.5f;
    barycentric=float3(1-ru-rv,ru,rv);return 1;
}

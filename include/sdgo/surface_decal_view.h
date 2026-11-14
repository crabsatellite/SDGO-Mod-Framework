#pragma once
#include "sdgo/surface_decal.h"
#include <algorithm>
#include <cmath>

namespace sdgo::surface_decal {
// Row-vector matrices, matching native D3D9 transforms. No device/native
// pointers escape into hit testing; input coordinates are viewport pixels.
using Matrix = std::array<double,16>;
struct View {Matrix world{},camera{},projection{};double x=0,y=0,width=0,height=0;};
inline Matrix Multiply(const Matrix& a,const Matrix& b) {
    Matrix c{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)c[i*4+j]+=a[i*4+k]*b[k*4+j];return c;
}
inline bool Invert(const Matrix& m,Matrix& out) {
    double rows[4][8]{};
    for(unsigned i=0;i<4;++i){for(unsigned j=0;j<4;++j){if(!std::isfinite(m[i*4+j]))return false;rows[i][j]=m[i*4+j];}rows[i][i+4]=1;}
    for(unsigned i=0;i<4;++i){unsigned pivot=i;for(unsigned j=i+1;j<4;++j)if(std::abs(rows[j][i])>std::abs(rows[pivot][i]))pivot=j;
        if(std::abs(rows[pivot][i])<1e-14)return false;
        for(unsigned j=0;j<8;++j)std::swap(rows[i][j],rows[pivot][j]);
        const auto divisor=rows[i][i];for(auto& x:rows[i])x/=divisor;
        for(unsigned r=0;r<4;++r)if(r!=i){const auto scale=rows[r][i];for(unsigned j=0;j<8;++j)rows[r][j]-=scale*rows[i][j];}
    }
    Matrix inverse{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j){inverse[i*4+j]=rows[i][j+4];if(!std::isfinite(inverse[i*4+j]))return false;}out=inverse;return true;
}
inline bool Transform(Vec3 p,const Matrix& m,Vec3& out) {
    const double w=p.x*m[3]+p.y*m[7]+p.z*m[11]+m[15];if(!std::isfinite(w)||std::abs(w)<1e-12)return false;
    Vec3 q{(p.x*m[0]+p.y*m[4]+p.z*m[8]+m[12])/w,(p.x*m[1]+p.y*m[5]+p.z*m[9]+m[13])/w,(p.x*m[2]+p.y*m[6]+p.z*m[10]+m[14])/w};
    if(!std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z))return false;out=q;return true;
}
inline bool ScreenRay(const View& v,double x,double y,Ray& out) {
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(v.width)||!std::isfinite(v.height)||v.width<=0||v.height<=0||
       x<v.x||y<v.y||x>=v.x+v.width||y>=v.y+v.height)return false;
    Matrix inverse;if(!Invert(Multiply(Multiply(v.world,v.camera),v.projection),inverse))return false;
    Vec3 nearPoint,farPoint;const double nx=2*(x-v.x)/v.width-1,ny=1-2*(y-v.y)/v.height;
    if(!Transform({nx,ny,0},inverse,nearPoint)||!Transform({nx,ny,1},inverse,farPoint))return false;
    Ray ray{nearPoint,{farPoint.x-nearPoint.x,farPoint.y-nearPoint.y,farPoint.z-nearPoint.z}};
    if(std::hypot(ray.direction.x,ray.direction.y,ray.direction.z)<1e-12)return false;out=ray;return true;
}
} // namespace sdgo::surface_decal

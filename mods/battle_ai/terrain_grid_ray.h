#pragma once

#include "combat_math.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace battle_ai {

// Accelerates ray tests for the native terrain layout without replacing its
// triangle predicate. X/Z must form one regular affine grid; Y remains live in
// the original vertex buffer and is read only by the supplied native callback.
class TerrainGridRay {
public:
    using TriangleFn=bool(*)(void*,const Vec3*,const Vec3*,const Vec3*,float*);

private:
    static constexpr std::uint32_t kMaximumAxis=4096;
    static constexpr std::size_t kMaximumCells=4u*1024u*1024u;
    static constexpr std::size_t kMaximumStride=4096;
    static constexpr float kInitialDistance=100000000.0f;
    static constexpr double kGridCoordinateTolerance=1.0e-3;

    const std::uint8_t* vertices_=nullptr;
    std::size_t stride_=0;
    std::uint32_t width_=0,height_=0;
    double baseX_=0,baseZ_=0;
    double inverse00_=0,inverse01_=0,inverse10_=0,inverse11_=0;
    std::vector<std::uint32_t> marks_;
    std::uint32_t stamp_=0;

    void Reset() noexcept {
        vertices_=nullptr;stride_=0;width_=height_=0;
        baseX_=baseZ_=0;
        inverse00_=inverse01_=inverse10_=inverse11_=0;
        marks_.clear();stamp_=0;
    }
    bool Load(std::size_t index,Vec3& value)const noexcept {
        std::memcpy(&value,vertices_+index*stride_,sizeof(value));
        return Finite(value);
    }
    const Vec3* Vertex(std::uint32_t column,std::uint32_t row)const noexcept {
        const std::size_t index=std::size_t(row)*(std::size_t(width_)+1)+column;
        return reinterpret_cast<const Vec3*>(vertices_+index*stride_);
    }
    void BeginQuery() {
        if(stamp_==std::numeric_limits<std::uint32_t>::max()) {
            std::fill(marks_.begin(),marks_.end(),0);
            stamp_=1;
        } else {
            ++stamp_;
            if(!stamp_)stamp_=1;
        }
    }
    void TestCell(std::int64_t column,std::int64_t row,TriangleFn triangle,void* context,
                  float& closest,bool& hit,std::uint64_t& tested) {
        if(column<0||row<0||column>=width_||row>=height_)return;
        const std::size_t cell=std::size_t(row)*width_+std::size_t(column);
        if(marks_[cell]==stamp_)return;
        marks_[cell]=stamp_;
        const auto x=static_cast<std::uint32_t>(column);
        const auto z=static_cast<std::uint32_t>(row);
        const Vec3* p00=Vertex(x,z);
        const Vec3* p01=Vertex(x+1,z);
        const Vec3* p10=Vertex(x,z+1);
        const Vec3* p11=Vertex(x+1,z+1);
        const auto test=[&](const Vec3* a,const Vec3* b,const Vec3* c) {
            float candidate=0;
            ++tested;
            if(triangle(context,a,b,c,&candidate)&&candidate>0&&candidate<closest) {
                closest=candidate;hit=true;
            }
        };
        // Exact source order from collision.terrain_ray (BFA900).
        test(p00,p01,p10);
        test(p01,p10,p11);
    }
    void TestNeighborhood(std::int64_t column,std::int64_t row,TriangleFn triangle,void* context,
                          float& closest,bool& hit,std::uint64_t& tested) {
        // The adjacent ring preserves cells touched on a grid edge/corner and
        // absorbs the final float-to-grid rounding at an affine boundary.
        for(std::int64_t dz=-1;dz<=1;++dz)
            for(std::int64_t dx=-1;dx<=1;++dx)
                TestCell(column+dx,row+dz,triangle,context,closest,hit,tested);
    }

public:
    bool Initialize(const std::uint8_t* vertices,std::size_t stride,
                    std::uint32_t width,std::uint32_t height) {
        Reset();
        if(!vertices||stride<sizeof(Vec3)||stride>kMaximumStride||
           reinterpret_cast<std::uintptr_t>(vertices)%alignof(float)!=0||
           stride%alignof(float)!=0||!width||!height||
           width>kMaximumAxis||height>kMaximumAxis)
            return false;
        const std::size_t cells=std::size_t(width)*height;
        if(cells>kMaximumCells)return false;
        const std::size_t columns=std::size_t(width)+1,rows=std::size_t(height)+1;
        if(columns>std::numeric_limits<std::size_t>::max()/rows)return false;
        const std::size_t vertexCount=columns*rows;
        if(vertexCount-1>std::numeric_limits<std::size_t>::max()/stride)return false;

        vertices_=vertices;stride_=stride;width_=width;height_=height;
        Vec3 base{},nextColumn{},nextRow{};
        if(!Load(0,base)||!Load(1,nextColumn)||!Load(columns,nextRow)) {
            Reset();return false;
        }
        const double columnX=double(nextColumn.x)-base.x;
        const double columnZ=double(nextColumn.z)-base.z;
        const double rowX=double(nextRow.x)-base.x;
        const double rowZ=double(nextRow.z)-base.z;
        const double columnLength=std::hypot(columnX,columnZ);
        const double rowLength=std::hypot(rowX,rowZ);
        const double determinant=columnX*rowZ-columnZ*rowX;
        if(!std::isfinite(columnLength)||!std::isfinite(rowLength)||
           !std::isfinite(determinant)||columnLength<1.0e-6||rowLength<1.0e-6||
           std::abs(determinant)<=columnLength*rowLength*1.0e-4) {
            Reset();return false;
        }
        const double inverse00=rowZ/determinant;
        const double inverse01=-rowX/determinant;
        const double inverse10=-columnZ/determinant;
        const double inverse11=columnX/determinant;
        for(std::uint32_t row=0;row<=height;++row) {
            for(std::uint32_t column=0;column<=width;++column) {
                Vec3 point{};
                if(!Load(std::size_t(row)*columns+column,point)) {
                    Reset();return false;
                }
                const double relativeX=double(point.x)-base.x;
                const double relativeZ=double(point.z)-base.z;
                const double gridColumn=inverse00*relativeX+inverse01*relativeZ;
                const double gridRow=inverse10*relativeX+inverse11*relativeZ;
                if(!std::isfinite(gridColumn)||!std::isfinite(gridRow)||
                   std::abs(gridColumn-column)>kGridCoordinateTolerance||
                   std::abs(gridRow-row)>kGridCoordinateTolerance) {
                    Reset();return false;
                }
            }
        }
        try {
            marks_.assign(cells,0);
        } catch(...) {
            Reset();return false;
        }
        baseX_=base.x;baseZ_=base.z;
        inverse00_=inverse00;inverse01_=inverse01;
        inverse10_=inverse10;inverse11_=inverse11;
        return true;
    }

    bool Intersect(Vec3 origin,Vec3 direction,TriangleFn triangle,void* context,
                   float& distance,std::uint64_t& tested) {
        distance=kInitialDistance;tested=0;
        if(!vertices_||marks_.empty()||!triangle||!Finite(origin)||!Finite(direction))
            return false;
        const double relativeX=double(origin.x)-baseX_;
        const double relativeZ=double(origin.z)-baseZ_;
        const double originColumn=inverse00_*relativeX+inverse01_*relativeZ;
        const double originRow=inverse10_*relativeX+inverse11_*relativeZ;
        const double directionColumn=inverse00_*direction.x+inverse01_*direction.z;
        const double directionRow=inverse10_*direction.x+inverse11_*direction.z;
        if(!std::isfinite(originColumn)||!std::isfinite(originRow)||
           !std::isfinite(directionColumn)||!std::isfinite(directionRow))
            return false;

        BeginQuery();
        float closest=kInitialDistance;bool hit=false;
        constexpr double directionEpsilon=1.0e-14;
        constexpr double clipPadding=2.0e-3;
        if(std::abs(directionColumn)<=directionEpsilon&&
           std::abs(directionRow)<=directionEpsilon) {
            if(originColumn < -clipPadding||originColumn > double(width_)+clipPadding||
               originRow < -clipPadding||originRow > double(height_)+clipPadding)
                return false;
            const double column=(std::max)(0.0,(std::min)(double(width_),originColumn));
            const double row=(std::max)(0.0,(std::min)(double(height_),originRow));
            const std::int64_t cellColumn=column>=width_?std::int64_t(width_)-1:
                static_cast<std::int64_t>(std::floor(column));
            const std::int64_t cellRow=row>=height_?std::int64_t(height_)-1:
                static_cast<std::int64_t>(std::floor(row));
            TestNeighborhood(cellColumn,cellRow,triangle,context,closest,hit,tested);
            distance=closest;return hit;
        }

        double enter=0,leave=kInitialDistance;
        const auto clip=[&](double position,double velocity,double maximum) {
            if(std::abs(velocity)<=directionEpsilon)
                return position>=-clipPadding&&position<=maximum+clipPadding;
            double first=(-clipPadding-position)/velocity;
            double last=(maximum+clipPadding-position)/velocity;
            if(first>last)std::swap(first,last);
            enter=(std::max)(enter,first);leave=(std::min)(leave,last);
            return enter<=leave;
        };
        if(!clip(originColumn,directionColumn,width_)||
           !clip(originRow,directionRow,height_)||leave<0)
            return false;
        enter=(std::max)(0.0,enter);
        leave=(std::min)(double(kInitialDistance),leave);
        if(enter>leave)return false;

        double column=originColumn+directionColumn*enter;
        double row=originRow+directionRow*enter;
        column=(std::max)(0.0,(std::min)(double(width_),column));
        row=(std::max)(0.0,(std::min)(double(height_),row));
        std::int64_t cellColumn=column>=width_?std::int64_t(width_)-1:
            static_cast<std::int64_t>(std::floor(column));
        std::int64_t cellRow=row>=height_?std::int64_t(height_)-1:
            static_cast<std::int64_t>(std::floor(row));
        const int stepColumn=directionColumn>directionEpsilon?1:
            directionColumn<-directionEpsilon?-1:0;
        const int stepRow=directionRow>directionEpsilon?1:
            directionRow<-directionEpsilon?-1:0;
        const double infinity=std::numeric_limits<double>::infinity();
        const double deltaColumn=stepColumn?std::abs(1.0/directionColumn):infinity;
        const double deltaRow=stepRow?std::abs(1.0/directionRow):infinity;
        const double columnBoundary=stepColumn>0?double(cellColumn+1):double(cellColumn);
        const double rowBoundary=stepRow>0?double(cellRow+1):double(cellRow);
        double nextColumn=stepColumn?enter+(columnBoundary-column)/directionColumn:infinity;
        double nextRow=stepRow?enter+(rowBoundary-row)/directionRow:infinity;
        const double traversalEpsilon=1.0e-9;
        const std::size_t maximumSteps=std::size_t(width_)+height_+4;
        for(std::size_t step=0;step<maximumSteps;++step) {
            if(cellColumn<0||cellRow<0||cellColumn>=width_||cellRow>=height_)break;
            TestNeighborhood(cellColumn,cellRow,triangle,context,closest,hit,tested);
            const double next=(std::min)(nextColumn,nextRow);
            if(next>leave+traversalEpsilon)break;
            if(nextColumn<nextRow-traversalEpsilon) {
                cellColumn+=stepColumn;nextColumn+=deltaColumn;
            } else if(nextRow<nextColumn-traversalEpsilon) {
                cellRow+=stepRow;nextRow+=deltaRow;
            } else {
                cellColumn+=stepColumn;cellRow+=stepRow;
                nextColumn+=deltaColumn;nextRow+=deltaRow;
            }
        }
        distance=closest;return hit;
    }
};

}

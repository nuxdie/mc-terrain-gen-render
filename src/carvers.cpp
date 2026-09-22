#include "terrain_internal.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace mcworld::detail {
namespace {
constexpr float pi=std::numbers::pi_v<float>;
// Minecraft's lookup-table trigonometry avoids platform-dependent sin calls
// inside the random walk. Table construction is still subject to libm rounding.
const std::array<float,65536>& trigTable() {
    static const auto table=[] {
        std::array<float,65536> values{};
        for(int i=0;i<65536;++i) values[i]=static_cast<float>(std::sin(i*2.0*std::numbers::pi/65536));
        return values;
    }();
    return table;
}
float sine(float angle) {return trigTable()[static_cast<int>(angle*10430.378F)&65535];}
float cosine(float angle) {return trigTable()[static_cast<int>(angle*10430.378F+16384.0F)&65535];}
float uniform(LegacyRandom& r,float low,float high) {return low+r.nextFloat()*(high-low);}
float trapezoid(LegacyRandom& r,float scale) {
    float a=r.nextFloat(),b=r.nextFloat();return a*(scale*2)+b*scale;
}
float perturb(LegacyRandom& r,float scale) {
    float a=r.nextFloat(),b=r.nextFloat(),c=r.nextFloat();return (a-b)*c*scale;
}
class Mask {
    TerrainChunk& chunk;
    std::vector<bool> bits=std::vector<bool>(16*16*384);
public:
    explicit Mask(TerrainChunk& c):chunk(c){}
    bool at(int x,int y,int z) const {return bits[(z*16+x)*384+y+64];}
    bool reachable(double x,double z,int step,int distance,float width) const {
        double dx=x-(chunk.chunkX*16+8),dz=z-(chunk.chunkZ*16+8),left=distance-step,radius=width+18;
        return dx*dx+dz*dz-left*left<=radius*radius;
    }
    void ellipsoid(double x,double y,double z,double horizontal,double vertical,double floor,
        const std::array<float,384>* canyon=nullptr) {
        int ox=chunk.chunkX*16,oz=chunk.chunkZ*16;
        double maxDelta=16+horizontal*2;
        if(std::abs(x-(ox+8))>maxDelta || std::abs(z-(oz+8))>maxDelta) return;
        int x0=std::max(0,static_cast<int>(std::floor(x-horizontal))-ox-1);
        int x1=std::min(15,static_cast<int>(std::floor(x+horizontal))-ox);
        int z0=std::max(0,static_cast<int>(std::floor(z-horizontal))-oz-1);
        int z1=std::min(15,static_cast<int>(std::floor(z+horizontal))-oz);
        int y0=std::max(-64,static_cast<int>(std::floor(y-vertical))-1);
        int y1=std::min(319,static_cast<int>(std::floor(y+vertical))+1);
        for(int bx=x0;bx<=x1;++bx) for(int bz=z0;bz<=z1;++bz) {
            double dx=(ox+bx+.5-x)/horizontal,dz=(oz+bz+.5-z)/horizontal;
            if(dx*dx+dz*dz>=1) continue;
            for(int by=y1;by>y0;--by) {
                double dy=(by-.5-y)/vertical;
                bool inside=canyon ? (dx*dx+dz*dz)*(*canyon)[by+63]+dy*dy/6<1 :
                    dy>floor && dx*dx+dy*dy+dz*dz<1;
                if(inside) bits[(bz*16+bx)*384+by+64]=true;
            }
        }
    }
    void tunnel(std::uint64_t seed,double x,double y,double z,double hm,double vm,float thickness,
        float yaw,float pitch,int start,int distance,double yScale,double floor) {
        LegacyRandom r(seed);int split=r.nextInt(distance/2)+distance/4;bool steep=r.nextInt(6)==0;
        float yawDrift=0,pitchDrift=0;
        for(int step=start;step<distance;++step) {
            double horizontal=1.5+sine(pi*step/distance)*thickness,vertical=horizontal*yScale;
            float cp=cosine(pitch);x+=cosine(yaw)*cp;y+=sine(pitch);z+=sine(yaw)*cp;
            pitch*=steep?.92F:.7F;pitch+=pitchDrift*.1F;yaw+=yawDrift*.1F;
            pitchDrift*=.9F;yawDrift*=.75F;
            pitchDrift+=perturb(r,2);yawDrift+=perturb(r,4);
            if(step==split && thickness>1) {
                auto left=r.nextLong();float leftWidth=r.nextFloat()*.5F+.5F;
                tunnel(left,x,y,z,hm,vm,leftWidth,yaw-pi/2,pitch/3,step,distance,1,floor);
                auto right=r.nextLong();float rightWidth=r.nextFloat()*.5F+.5F;
                tunnel(right,x,y,z,hm,vm,rightWidth,yaw+pi/2,pitch/3,step,distance,1,floor);
                return;
            }
            if(r.nextInt(4)!=0) {
                if(!reachable(x,z,step,distance,thickness)) return;
                ellipsoid(x,y,z,horizontal*hm,vertical*vm,floor);
            }
        }
    }
    void caves(LegacyRandom& r,int sx,int sz,bool extra) {
        int count=r.nextInt(r.nextInt(r.nextInt(15)+1)+1);
        for(int cave=0;cave<count;++cave) {
            double x=sx*16+r.nextInt(16),y=-56+r.nextInt(extra?104:237),z=sz*16+r.nextInt(16);
            double hm=uniform(r,.7F,1.4F),vm=uniform(r,.8F,1.3F),floor=uniform(r,-1,-.4F);
            int tunnels=1;
            if(r.nextInt(4)==0) {
                double ys=uniform(r,.1F,.9F);float thickness=1+r.nextFloat()*6;
                ellipsoid(x+1,y,z,1.5+thickness,(1.5+thickness)*ys,floor);
                tunnels+=r.nextInt(4);
            }
            for(int i=0;i<tunnels;++i) {
                float yaw=r.nextFloat()*(pi*2),pitch=(r.nextFloat()-.5F)/4,thickness=trapezoid(r,1);
                if(r.nextInt(10)==0) {float a=r.nextFloat(),b=r.nextFloat();thickness*=a*b*3+1;}
                int distance=112-r.nextInt(28);auto seed=r.nextLong();
                tunnel(seed,x,y,z,hm,vm,thickness,yaw,pitch,0,distance,1,floor);
            }
        }
    }
    void canyon(LegacyRandom& source,int sx,int sz) {
        double x=sx*16+source.nextInt(16),y=10+source.nextInt(58),z=sz*16+source.nextInt(16);
        float yaw=source.nextFloat()*(pi*2),pitch=uniform(source,-.125F,.125F),thickness=trapezoid(source,2);
        int distance=static_cast<int>(112*uniform(source,.75F,1));LegacyRandom r(source.nextLong());
        std::array<float,384> widths{};float width=1;
        for(int i=0;i<384;++i) {
            if(i==0 || r.nextInt(3)==0) {float a=r.nextFloat(),b=r.nextFloat();width=1+a*b;}
            widths[i]=width*width;
        }
        float yawDrift=0,pitchDrift=0;
        for(int step=0;step<distance;++step) {
            double radius=1.5+sine(step*pi/distance)*thickness,vertical=radius*3;
            radius*=uniform(r,.75F,1);vertical*=uniform(r,.75F,1);
            float cp=cosine(pitch);x+=cosine(yaw)*cp;y+=sine(pitch);z+=sine(yaw)*cp;
            pitch*=.7F;pitch+=pitchDrift*.05F;yaw+=yawDrift*.05F;
            pitchDrift*=.8F;yawDrift*=.5F;pitchDrift+=perturb(r,2);yawDrift+=perturb(r,4);
            if(r.nextInt(4)!=0) {
                if(!reachable(x,z,step,distance,thickness)) return;
                ellipsoid(x,y,z,radius,vertical,-1,&widths);
            }
        }
    }
};
}
void carve(TerrainChunk& chunk,const OverworldNoiseRouter& router,const BiomeSource& biomes,Aquifer& aquifer,bool veins) {
    Mask mask(chunk);
    for(int sx=chunk.chunkX-8;sx<=chunk.chunkX+8;++sx) for(int sz=chunk.chunkZ-8;sz<=chunk.chunkZ+8;++sz) {
        // All standard Overworld biomes register these same three carvers.
        (void)biomes.sample(router,sx*16,0,sz*16);
        for(int i=0;i<3;++i) {
            auto seed=static_cast<std::uint64_t>(router.seed())+i;
            LegacyRandom seedRandom(seed);auto a=seedRandom.nextLong(),b=seedRandom.nextLong();
            LegacyRandom random((static_cast<std::uint64_t>(sx)*a)^(static_cast<std::uint64_t>(sz)*b)^seed);
            constexpr float probability[]{.15F,.07F,.01F};
            if(random.nextFloat()>probability[i]) continue;
            if(i==2) mask.canyon(random,sx,sz);else mask.caves(random,sx,sz,i==1);
        }
    }
    auto topMaterial=makeTopMaterialRule(chunk,router,veins);
    for(int z=0;z<16;++z) for(int x=0;x<16;++x) {
        bool surface=false;
        for(int y=319;y>=-64;--y) {
            if(!mask.at(x,y,z)) {surface=false;continue;}
            auto old=chunk.at(x,y,z);
            if(old==Block::Bedrock) continue;
            if(old==Block::Grass || old==Block::Mycelium) surface=true;
            auto substance=aquifer.sample(chunk.chunkX*16+x,y,chunk.chunkZ*16+z,0);
            if(substance.block==Block::Stone) continue;
            chunk.set(x,y,z,substance.block);
            if(substance.schedule && isFluid(substance.block)) chunk.fluidPostProcessing.push_back({x,y,z});
            if(surface && y>-64 && chunk.at(x,y-1,z)==Block::Dirt) {
                if(auto material=topMaterial(x,y-1,z,isFluid(substance.block))) {
                    chunk.set(x,y-1,z,*material);
                    if(isFluid(*material)) chunk.fluidPostProcessing.push_back({x,y-1,z});
                }
            }
        }
    }
}
} // namespace mcworld::detail

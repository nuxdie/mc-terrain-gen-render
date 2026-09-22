#include "terrain_internal.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace mcworld::detail {
std::uint64_t positionalSeed(std::int64_t seed,const char* key,int x,int y,int z) {
    // Project-keyed deterministic convention, not Minecraft's positional factory.
    auto mix=[](std::uint64_t v) {v=(v^(v>>30))*0xbf58476d1ce4e5b9ULL;v=(v^(v>>27))*0x94d049bb133111ebULL;return v^(v>>31);};
    return mix(deriveSeed(seed,key)^mix(static_cast<std::uint64_t>(x))^
        mix(static_cast<std::uint64_t>(y)+0x9e3779b97f4a7c15ULL)^mix(static_cast<std::uint64_t>(z)+0xd1b54a32d192ed03ULL));
}
Aquifer::Aquifer(const OverworldNoiseRouter& router,bool enabled):router_(router),enabled_(enabled),
    barrier_(router.seed(),"minecraft:aquifer_barrier",-3,{1}),
    floodedness_(router.seed(),"minecraft:aquifer_fluid_level_floodedness",-7,{1}),
    spread_(router.seed(),"minecraft:aquifer_fluid_level_spread",-5,{1}),lava_(router.seed(),"minecraft:aquifer_lava",-1,{1}){}

int Aquifer::surface(int x,int z) {
    x=floorDiv(x,4)*4;z=floorDiv(z,4)*4;
    auto [it,inserted]=surfaces_.try_emplace({x,z},0);
    if(inserted) it->second=static_cast<int>(std::floor(router_.samplePreliminarySurface(x,z)));
    return it->second;
}
Aquifer::Fluid Aquifer::fluid(int x,int y,int z) {
    Fluid global=y<-54?Fluid{-54,Block::Lava}:Fluid{63,Block::Water};
    int lowest=std::numeric_limits<int>::max(); bool underwater=false;
    constexpr int offsets[][2]{{0,0},{-2,-1},{-1,-1},{0,-1},{1,-1},{-3,0},{-2,0},{-1,0},{1,0},{-2,1},{-1,1},{0,1},{1,1}};
    for(auto& offset:offsets) {
        int level=surface(x+offset[0]*16,z+offset[1]*16); int adjusted=level+8;
        bool atCenter=offset[0]==0 && offset[1]==0;
        if(atCenter && y-12>adjusted) return global;
        if(y+12>adjusted || atCenter) {
            Fluid atSurface=adjusted<-54?Fluid{-54,Block::Lava}:Fluid{63,Block::Water};
            if(atSurface.at(adjusted)!=Block::Air) {
                if(atCenter) underwater=true;
                if(y+12>adjusted) return atSurface;
            }
        }
        lowest=std::min(lowest,level);
    }
    auto climate=router_.sample(x,y,z);
    int level=-1000000;
    if(!(climate.erosion<-.225F && climate.depth>.9F)) {
        double factor=underwater?1.0-std::clamp((lowest+8-y)/64.0,0.0,1.0):0.0;
        double flood=std::clamp(static_cast<double>(floodedness_.sample(x,y*.67,z)),-1.0,1.0);
        if(flood>.8-1.1*factor) level=global.level;
        else if(flood>.4-1.2*factor) {
            float spread=spread_.sample(floorDiv(x,16),floorDiv(y,40)*.7142857142857143,floorDiv(z,16))*10.0F;
            level=std::min(lowest,floorDiv(y,40)*40+20+static_cast<int>(std::floor(spread/3))*3);
        }
    }
    Block type=global.type;
    if(level<=-10 && level!=-1000000 && type!=Block::Lava &&
        std::abs(lava_.sample(floorDiv(x,64),floorDiv(y,40),floorDiv(z,64)))>.3F) type=Block::Lava;
    return {level,type};
}
const Aquifer::Center& Aquifer::center(int x,int y,int z) {
    auto key=std::tuple{x,y,z}; auto it=centers_.find(key);
    if(it!=centers_.end()) return it->second;
    LegacyRandom random(positionalSeed(router_.seed(),"minecraft:aquifer",x,y,z));
    int bx=x*16+random.nextInt(10),by=y*12+random.nextInt(9),bz=z*16+random.nextInt(10);
    return centers_.emplace(key,Center{bx,by,bz,fluid(bx,by,bz)}).first->second;
}
double Aquifer::pressure(int x,int y,int z,Fluid a,Fluid b,double& noise) {
    auto ta=a.at(y),tb=b.at(y);
    if((ta==Block::Water && tb==Block::Lava)||(ta==Block::Lava && tb==Block::Water)) return 2;
    int diff=std::abs(a.level-b.level); if(diff==0) return 0;
    double above=y+.5-.5*(a.level+b.level),edge=diff/2.0-std::abs(above),gradient;
    if(above>0) gradient=edge/(edge>0?1.5:2.5);
    else {edge+=3;gradient=edge/(edge>0?3:10);}
    double value=0;
    if(gradient>=-2 && gradient<=2) {
        if(std::isnan(noise)) noise=barrier_.sample(x,y*.5,z);
        value=noise;
    }
    return 2*(gradient+value);
}
Substance Aquifer::sample(int x,int y,int z,float density) {
    if(density>0) return {Block::Stone};
    if(y<-54) return {Block::Lava};
    if(!enabled_) return {y<63?Block::Water:Block::Air};
    std::array<const Center*,4> near{};
    std::array<int,4> distances; distances.fill(std::numeric_limits<int>::max());
    for(int dx=0;dx<=1;++dx) for(int dy=-1;dy<=1;++dy) for(int dz=0;dz<=1;++dz) {
        const auto& c=center(floorDiv(x-5,16)+dx,floorDiv(y+1,12)+dy,floorDiv(z-5,16)+dz);
        int distance=(x-c.x)*(x-c.x)+(y-c.y)*(y-c.y)+(z-c.z)*(z-c.z);
        for(int i=0;i<4;++i) if(distance<=distances[i]) {
            for(int j=3;j>i;--j) {distances[j]=distances[j-1];near[j]=near[j-1];}
            distances[i]=distance;near[i]=&c;break;
        }
    }
    auto similar=[&](int a,int b){return 1.0-(distances[b]-distances[a])/25.0;};
    auto different=[&](int a,int b){return near[a]->fluid!=near[b]->fluid;};
    Block block=near[0]->fluid.at(y); double s12=similar(0,1);
    constexpr double flow=-.76;
    if(s12<=0) return {block,s12>=flow && different(0,1)};
    if(block==Block::Water && y==-54) return {block,true};
    double noise=std::numeric_limits<double>::quiet_NaN();
    if(density+s12*pressure(x,y,z,near[0]->fluid,near[1]->fluid,noise)>0) return {Block::Stone};
    double s13=similar(0,2),s23=similar(1,2);
    if(s13>0 && density+s12*s13*pressure(x,y,z,near[0]->fluid,near[2]->fluid,noise)>0) return {Block::Stone};
    if(s23>0 && density+s12*s23*pressure(x,y,z,near[1]->fluid,near[2]->fluid,noise)>0) return {Block::Stone};
    bool schedule=different(0,1)||(s23>=flow && different(1,2))||(s13>=flow && different(0,2))||
        (s13>=flow && similar(0,3)>=flow && different(0,3));
    return {block,schedule};
}
} // namespace mcworld::detail

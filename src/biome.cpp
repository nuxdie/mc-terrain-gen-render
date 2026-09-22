#include "mcworld/biome.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mcworld {
namespace {
using enum Biome;
using B = Biome;
struct Range { float lo, hi; };
constexpr Range full{-1, 1};
constexpr Range temperatures[]{{-1,-.45F},{-.45F,-.15F},{-.15F,.2F},{.2F,.55F},{.55F,1}};
constexpr Range humidities[]{{-1,-.35F},{-.35F,-.1F},{-.1F,.1F},{.1F,.3F},{.3F,1}};
constexpr Range erosions[]{{-1,-.78F},{-.78F,-.375F},{-.375F,-.2225F},{-.2225F,.05F},{.05F,.45F},{.45F,.55F},{.55F,1}};
constexpr Range continents[]{{-.19F,-.11F},{-.11F,.03F},{.03F,.3F},{.3F,1}};
constexpr B middle[5][5]{
    {SnowyPlains,SnowyPlains,SnowyPlains,SnowyTaiga,Taiga},
    {Plains,Plains,Forest,Taiga,OldGrowthSpruceTaiga},
    {FlowerForest,Plains,Forest,BirchForest,DarkForest},
    {Savanna,Savanna,Forest,Jungle,Jungle},
    {Desert,Desert,Desert,Desert,Desert}};
constexpr B middleVariant[5][5]{
    {IceSpikes,SnowyPlains,SnowyTaiga,SnowyTaiga,Taiga},
    {DappledForest,Plains,Forest,Taiga,OldGrowthPineTaiga},
    {SunflowerPlains,Plains,Forest,OldGrowthBirchForest,DarkForest},
    {Savanna,Savanna,Plains,SparseJungle,BambooJungle},
    {Desert,Desert,Desert,Desert,Desert}};
constexpr B plateau[5][5]{
    {SnowyPlains,SnowyPlains,SnowyPlains,SnowyTaiga,SnowyTaiga},
    {Meadow,Meadow,Forest,Taiga,OldGrowthSpruceTaiga},
    {Meadow,Meadow,Meadow,Meadow,PaleGarden},
    {SavannaPlateau,SavannaPlateau,Forest,Forest,Jungle},
    {Badlands,Badlands,Badlands,WoodedBadlands,WoodedBadlands}};
constexpr B plateauVariant[5][5]{
    {IceSpikes,SnowyPlains,SnowyPlains,SnowyTaiga,SnowyTaiga},
    {CherryGrove,Meadow,Meadow,Meadow,OldGrowthPineTaiga},
    {CherryGrove,CherryGrove,Forest,BirchForest,PaleGarden},
    {SavannaPlateau,SavannaPlateau,Forest,Forest,Jungle},
    {ErodedBadlands,ErodedBadlands,Badlands,WoodedBadlands,WoodedBadlands}};

std::int64_t quantize(float v) { return static_cast<std::int64_t>(v * 10000.0F); }
struct Point { std::array<std::array<std::int64_t,2>,6> ranges; B biome; };
std::vector<Point> makePoints() {
    std::vector<Point> points;
    auto add = [&](Range t, Range h, Range c, Range e, Range d, Range w, B b) {
        Point p{}; p.biome = b;
        std::array<Range,6> ranges{t,h,c,e,d,w};
        for (int i=0;i<6;++i) p.ranges[i]={quantize(ranges[i].lo),quantize(ranges[i].hi)};
        points.push_back(p);
    };
    auto surface = [&](Range t, Range h, Range c, Range e, Range w, B b) {
        add(t,h,c,e,{0,0},w,b); add(t,h,c,e,{1,1},w,b);
    };
    surface(full,full,{-1.2F,-1.05F},full,full,MushroomFields);
    constexpr B oceans[2][5]{{DeepFrozenOcean,DeepColdOcean,DeepOcean,DeepLukewarmOcean,WarmOcean},
        {FrozenOcean,ColdOcean,Ocean,LukewarmOcean,WarmOcean}};
    for(int t=0;t<5;++t) {
        surface(temperatures[t],full,{-1.05F,-.455F},full,full,oceans[0][t]);
        surface(temperatures[t],full,{-.455F,-.19F},full,full,oceans[1][t]);
    }
    constexpr float edges[]{-1,-.93333334F,-.7666667F,-.56666666F,-.4F,-.26666668F,-.05F,
        .05F,.26666668F,.4F,.56666666F,.7666667F,.93333334F,1};
    // 0 mid, 1 high, 2 peaks, 3 low, 4 valley; emitted in vanilla registration order.
    constexpr int slices[]{0,1,2,1,0,3,4,3,0,1,2,1,0};
    for(int s=0;s<13;++s) {
        Range w{edges[s],edges[s+1]}; bool positive=w.hi>=0; int kind=slices[s];
        auto common = [&](Range t, int c0,int c1,int e0,int e1,B b) {
            surface(t,full,{continents[c0].lo,continents[c1].hi},{erosions[e0].lo,erosions[e1].hi},w,b);
        };
        if(kind==0 || kind==3) common(full,0,0,0,2,StonyShore);
        if(kind==4) {
            // Keep Java's registration order even where climate ranges overlap.
            for(int region=0;region<3;++region) for(int frozen=0;frozen<2;++frozen) {
                Range t=frozen==0 ? temperatures[0] : Range{temperatures[1].lo,1};
                B river=frozen==0 ? FrozenRiver : River;
                if(region==0) common(t,0,0,0,1,positive?river:StonyShore);
                else if(region==1) common(t,1,1,0,1,river);
                else common(t,0,3,2,5,river);
            }
            common(temperatures[0],0,0,6,6,FrozenRiver);
            common({temperatures[1].lo,1},0,0,6,6,River);
        }
        if(kind==0 || kind==3 || kind==4) {
            common({temperatures[1].lo,temperatures[2].hi},1,3,6,6,Swamp);
            common({temperatures[3].lo,temperatures[4].hi},1,3,6,6,MangroveSwamp);
            if(kind==4) common(temperatures[0],1,3,6,6,FrozenRiver);
        }
        for(int t=0;t<5;++t) for(int h=0;h<5;++h) {
            B m=positive?middleVariant[t][h]:middle[t][h];
            B p=positive?plateauVariant[t][h]:plateau[t][h];
            B bad=h<2 ? (positive?ErodedBadlands:Badlands) : (h<3?Badlands:WoodedBadlands);
            B hot=t==4?bad:m;
            B slope=t>=3?p:(h<=1?SnowySlopes:Grove);
            B cold=t==0?slope:hot;
            B peak=t<=2?(positive?FrozenPeaks:JaggedPeaks):(t==3?StonyPeaks:bad);
            B shattered=t>=3?m:(h>=3?WindsweptForest:(h<2 && t<2?WindsweptGravellyHills:WindsweptHills));
            B beach=t==0?SnowyBeach:(t==4?Desert:Beach);
            auto windswept=[&](B b){return t>1 && h<4 && positive?WindsweptSavanna:b;};
            auto put=[&](int c0,int c1,int e0,int e1,B b){
                surface(temperatures[t],humidities[h],{continents[c0].lo,continents[c1].hi},
                    {erosions[e0].lo,erosions[e1].hi},w,b);
            };
            if(kind==2) {
                put(0,3,0,0,peak); put(0,1,1,1,cold); put(2,3,1,1,peak);
                put(0,1,2,3,m); put(2,3,2,2,p); put(2,2,3,3,hot); put(3,3,3,3,p);
                put(0,3,4,4,m); put(0,1,5,5,windswept(shattered)); put(2,3,5,5,shattered); put(0,3,6,6,m);
            } else if(kind==1) {
                put(0,0,0,1,m); put(1,1,0,0,slope); put(2,3,0,0,peak);
                put(1,1,1,1,cold); put(2,3,1,1,slope); put(0,1,2,3,m);
                put(2,3,2,2,p); put(2,2,3,3,hot); put(3,3,3,3,p); put(0,3,4,4,m);
                put(0,1,5,5,windswept(m)); put(2,3,5,5,shattered); put(0,3,6,6,m);
            } else if(kind==0) {
                put(1,3,0,0,slope); put(1,2,1,1,cold); put(3,3,1,1,t==0?slope:p);
                put(1,1,2,2,m); put(2,2,2,2,hot); put(3,3,2,2,p);
                put(0,1,3,3,m); put(2,3,3,3,hot);
                if(!positive) {put(0,0,4,4,beach); put(1,3,4,4,m);} else put(0,3,4,4,m);
                put(0,0,5,5,windswept(positive?m:beach)); put(1,1,5,5,windswept(m)); put(2,3,5,5,shattered);
                put(0,0,6,6,positive?m:beach); if(t==0) put(1,3,6,6,m);
            } else if(kind==3) {
                put(1,1,0,1,hot); put(2,3,0,1,cold); put(1,1,2,3,m); put(2,3,2,3,hot);
                put(0,0,3,4,beach); put(1,3,4,4,m); put(0,0,5,5,windswept(positive?m:beach));
                put(1,1,5,5,windswept(m)); put(2,3,5,5,m); put(0,0,6,6,beach); if(t==0) put(1,3,6,6,m);
            } else put(2,3,0,1,hot);
        }
    }
    add(full,full,{.8F,1},full,{.2F,.9F},full,DripstoneCaves);
    add(full,{.7F,1},full,full,{.2F,.9F},full,LushCaves);
    add(full,full,{-.19F,.55F},{.45F,1},{.2F,.9F},{-1.1F,-.85F},SulfurCaves);
    add(full,full,full,{-1,-.375F},{1.1F,1.1F},full,DeepDark);
    return points;
}
} // namespace

Biome resolveOverworldBiome(const RouterSample& sample) {
    static const auto points=makePoints();
    std::array<float,6> values{sample.temperature,sample.vegetation,sample.continentalness,
        sample.erosion,sample.depth,sample.ridges};
    std::array<std::int64_t,6> target{};
    for(int i=0;i<6;++i) {
        if(!std::isfinite(values[i]) || std::abs(values[i])>10000) throw std::invalid_argument("Invalid biome climate");
        target[i]=quantize(values[i]);
    }
    std::int64_t best=std::numeric_limits<std::int64_t>::max(); B result=Plains;
    for(const auto& p:points) {
        std::int64_t distance=0;
        for(int i=0;i<6;++i) {
            auto delta=std::max({p.ranges[i][0]-target[i],target[i]-p.ranges[i][1],std::int64_t{0}});
            distance+=delta*delta;
            if(distance>=best) break;
        }
        if(distance<best) {best=distance; result=p.biome;}
    }
    return result;
}

Biome sampleOverworldBiome(const OverworldNoiseRouter& router,int x,int y,int z) {
    auto quart=[](int v){return std::floor(static_cast<double>(v)/4)*4;};
    return resolveOverworldBiome(router.sample(quart(x),quart(y),quart(z)));
}
} // namespace mcworld

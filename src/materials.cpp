#include "terrain_internal.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace mcworld::detail {
namespace {
using enum Block;
using Opt = std::optional<Block>;
float remapClamped(float value,float fromMin,float fromMax,float toMin,float toMax) {
    // DensityFunctions.clampedMap builds a clamp -> multiply -> add graph.
    float factor=(toMax-toMin)/(fromMax-fromMin),offset=toMin-fromMin*factor;
    return std::clamp(value,fromMin,fromMax)*factor+offset;
}
float interpolateNoise(const NormalNoise& noise,int x,int y,int z,double scale,float outside) {
    int x0=floorDiv(x,4)*4,y0=floorDiv(y,8)*8,z0=floorDiv(z,4)*4;
    auto sample=[&](int dx,int dy,int dz){int sy=y0+dy*8;return sy>=-64 && sy<=56?
        noise.sample((x0+dx*4)*scale,sy*scale,(z0+dz*4)*scale):outside;};
    auto lerp=[](float t,float a,float b){return a+t*(b-a);};
    float tx=(x-x0)/4.0F,ty=(y-y0)/8.0F,tz=(z-z0)/4.0F;
    // Match the existing density lattice's X, then Y, then Z evaluation order.
    float c000=sample(0,0,0),c100=sample(1,0,0),c010=sample(0,1,0),c110=sample(1,1,0);
    float c001=sample(0,0,1),c101=sample(1,0,1),c011=sample(0,1,1),c111=sample(1,1,1);
    float a=lerp(tx,c000,c100),b=lerp(tx,c010,c110);
    float c=lerp(tx,c001,c101),d=lerp(tx,c011,c111);
    return lerp(tz,lerp(ty,a,b),lerp(ty,c,d));
}
class Materials {
    const OverworldNoiseRouter& router;
    BlockBiomeGetter biomes;
    NormalNoise surface,secondary,toggle,veinA,veinB,gap,sulfur,calcite,gravel,powder,packed,ice,swamp,patch,
        bandOffset,badSurface,badPillar,badRoof,iceSurface,icePillar,iceRoof;
    std::array<Block,192> bands;
public:
    explicit Materials(const OverworldNoiseRouter& r,BlockBiomeGetter getter):router(r),biomes(std::move(getter)),
        surface(r.seed(),"minecraft:surface",-6,{1,1,1}),secondary(r.seed(),"minecraft:surface_secondary",-6,{1,1,0,1}),
        toggle(r.seed(),"minecraft:ore_veininess",-8,{1}),veinA(r.seed(),"minecraft:ore_vein_a",-7,{1}),veinB(r.seed(),"minecraft:ore_vein_b",-7,{1}),
        gap(r.seed(),"minecraft:ore_gap",-5,{1}),sulfur(r.seed(),"minecraft:sulfur_cave_gradient",-5,{1,0,1}),
        calcite(r.seed(),"minecraft:calcite",-9,{1,1,1,1}),gravel(r.seed(),"minecraft:gravel",-8,{1,1,1,1}),
        powder(r.seed(),"minecraft:powder_snow",-6,{1,1,1,1}),packed(r.seed(),"minecraft:packed_ice",-7,{1,1,1,1}),
        ice(r.seed(),"minecraft:ice",-4,{1,1,1,1}),swamp(r.seed(),"minecraft:surface_swamp",-2,{1}),patch(r.seed(),"minecraft:small_patch",-3,{3}),
        bandOffset(r.seed(),"minecraft:clay_bands_offset",-8,{1}),badSurface(r.seed(),"minecraft:badlands_surface",-6,{1,1,1}),
        badPillar(r.seed(),"minecraft:badlands_pillar",-2,{1,1,1,1}),badRoof(r.seed(),"minecraft:badlands_pillar_roof",-8,{1}),
        iceSurface(r.seed(),"minecraft:iceberg_surface",-6,{1,1,1}),icePillar(r.seed(),"minecraft:iceberg_pillar",-6,{1,1,1,1}),
        iceRoof(r.seed(),"minecraft:iceberg_pillar_roof",-3,{1}) {
        bands.fill(Terracotta);LegacyRandom random(deriveSeed(r.seed(),"minecraft:clay_bands"));
        for(int i=0;i<192;++i) {i+=random.nextInt(5)+1;if(i<192) bands[i]=OrangeTerracotta;}
        auto colored=[&](int base,Block b) {
            int count=6+random.nextInt(10);
            for(int i=0;i<count;++i) {
                int width=base+random.nextInt(3),start=random.nextInt(192);
                for(int p=0;p<width && start+p<192;++p) bands[start+p]=b;
            }
        };
        colored(1,YellowTerracotta);colored(2,BrownTerracotta);colored(1,RedTerracotta);
        int count=9+random.nextInt(7),start=0;
        for(int i=0;i<count && start<192;++i) {
            bands[start]=WhiteTerracotta;
            if(start-1>0 && random.bits(1)) bands[start-1]=LightGrayTerracotta;
            if(start+1<192 && random.bits(1)) bands[start+1]=LightGrayTerracotta;
            start+=random.nextInt(16)+4;
        }
    }
    Biome biomeAt(const TerrainChunk& chunk,int x,int y,int z) const {
        return biomes?biomes(chunk.chunkX*16+x,y,chunk.chunkZ*16+z):chunk.biomeAt(x,std::clamp(y,-64,319),z);
    }
    Opt ore(int x,int y,int z) {
        if(!((y>=0 && y<50)||(y>=-60 && y<-8))) return {};
        float t=interpolateNoise(toggle,x,y,z,1.5,0);bool copper=t>0;
        int lo=copper?0:-60,hi=copper?50:-8;
        if(y<lo || y>=hi) return {};
        float edge=remapClamped(static_cast<float>(std::min(y-lo,hi-y)),0,20,-.2F,0);
        if(std::abs(t)-.4F+edge<0) return {};
        if(std::max(std::abs(interpolateNoise(veinA,x,y,z,4,1)),std::abs(interpolateNoise(veinB,x,y,z,4,1)))>.08F) return {};
        LegacyRandom random(positionalSeed(router.seed(),"minecraft:ore",x,y,z));
        if(random.nextFloat()>.7F) return {};
        float richness=remapClamped(std::abs(t),.4F,.6F,.1F,.3F);
        if(random.nextFloat()<richness && gap.sample(x,y,z)>-.3F) {
            if(random.nextFloat()<.02F) return copper?RawCopper:RawIron;
            return copper?CopperOre:DeepslateIronOre;
        }
        return copper?Granite:Tuff;
    }
    Opt sulfurBand(int x,int y,int z) {
        float n=sulfur.sample(x,y,z);
        if(n>=-.4F && n<=-.1F) return Cinnabar;
        if(n>=0 && n<=.4F) return Sulfur;
        if(n>=.4F) return Cinnabar;
        return {};
    }
    bool gradient(const char* key,int x,int y,int z,int low,int high) {
        if(y<=low) return true;
        if(y>=high) return false;
        LegacyRandom random(positionalSeed(router.seed(),key,x,y,z));
        return random.nextFloat()<1.0-(y-low)/static_cast<double>(high-low);
    }
    Opt biomeMaterial(Biome b,int x,int y,int z,bool top,bool ceiling,bool dry,bool steep,float n) {
        auto in=[&](const NormalNoise& noise,double lo,double hi){float v=noise.sample(x,0,z);return v>=lo && v<=hi;};
        auto surfaceAbove=[&](double threshold){return n>=threshold/8.25;};
        Block sand=ceiling?Sandstone:Sand,grit=ceiling?Stone:Gravel;
        if(b==Biome::FrozenPeaks) {
            if(steep || in(packed,top?0:-.5,.2)) return PackedIce;
            if(in(ice,top?0:-.0625,.025)) return Ice;
            if(dry) return Snow;
        }
        if(b==Biome::SnowySlopes || b==Biome::Grove) {
            if(b==Biome::SnowySlopes && steep) return Stone;
            if(dry && in(powder,top?.35:.45,top?.6:.58)) return PowderSnow;
            if(b==Biome::Grove && !top) return Dirt;
            if(dry) return Snow;
        }
        if(b==Biome::JaggedPeaks) {
            if(!top || steep) return Stone;
            if(dry) return Snow;
        }
        if(b==Biome::StonyPeaks) return in(calcite,-.0125,.0125)?Calcite:Stone;
        if(b==Biome::StonyShore) return in(gravel,-.05,.05)?grit:Stone;
        if(b==Biome::WindsweptHills && surfaceAbove(1)) return Stone;
        if(b==Biome::WarmOcean || b==Biome::Beach || b==Biome::SnowyBeach || b==Biome::Desert) return sand;
        if(b==Biome::DripstoneCaves) return Stone;
        if(b==Biome::SulfurCaves) return sulfurBand(x,y,z).value_or(Stone);
        if(b==Biome::MangroveSwamp) return Mud;
        if(b==Biome::WindsweptSavanna) {
            if(surfaceAbove(1.75)) return Stone;
            if(top && surfaceAbove(-.5)) return CoarseDirt;
        }
        if(b==Biome::WindsweptGravellyHills) {
            if(surfaceAbove(2)) return grit;
            if(surfaceAbove(1)) return Stone;
            if(!surfaceAbove(-1)) return grit;
        }
        if(top) {
            if(b==Biome::OldGrowthPineTaiga || b==Biome::OldGrowthSpruceTaiga) {
                if(surfaceAbove(1.75)) return CoarseDirt;
                if(surfaceAbove(-.95)) return Podzol;
            }
            if(b==Biome::IceSpikes && dry) return Snow;
            if(b==Biome::MushroomFields) return Mycelium;
            if(b==Biome::DappledForest && patch.sample(x,0,z)>=1.2F) return CoarseDirt;
            return dry?Grass:Dirt;
        }
        return Dirt;
    }
    Opt top(const TerrainChunk& chunk,int x,int y,int z,bool underFluid,bool veins) {
        int wx=chunk.chunkX*16+x,wz=chunk.chunkZ*16+z;
        if(gradient("minecraft:bedrock_floor",wx,y,wz,-64,-59)) return Bedrock;
        if(veins) if(auto value=ore(wx,y,wz)) return value;
        Biome biome=biomeAt(chunk,x,y,z);
        float sn=surface.sample(wx,0,wz);
        LegacyRandom random(positionalSeed(router.seed(),"minecraft:surface",wx,0,wz));
        int depth=static_cast<int>(sn*2.75+3+random.nextDouble()*.25);
        int minSurface=static_cast<int>(std::floor(router.sample(wx,0,wz).chunkSurfaceLevel))+depth-8;
        // topMaterial evaluates a single-block column: both stone depths are one.
        // With fluid, waterHeight=y+1; NOT_UNDERWATER's -1 offset still matches.
        (void)underFluid;
        if(y>=minSurface) {
            bool band=(sn>=-.909 && sn<=-.5454)||(sn>=-.1818 && sn<=.1818)||(sn>=.5454 && sn<=.909);
            if(biome==Biome::WoodedBadlands && y>=97+2*depth) return band?CoarseDirt:Grass;
            if(y<63 && ((biome==Biome::Swamp && y>=62)||(biome==Biome::MangroveSwamp && y>=60)) && swamp.sample(wx,0,wz)>=0) return Water;
            if(biome==Biome::Badlands || biome==Biome::ErodedBadlands || biome==Biome::WoodedBadlands) {
                if(y>=256) return OrangeTerracotta;
                if(y+1>=74+depth) {
                    int shift=static_cast<int>(std::floor(bandOffset.sample(wx,0,wz)*4.0F+.5F));
                    return band?Terracotta:bands[(y+shift+384)%192];
                }
                return RedSandstone;
            }
            if((biome==Biome::FrozenOcean || biome==Biome::DeepFrozenOcean) && depth<=0) return Air;
            int gx=chunk.worldSurface[z*16+std::min(x+1,15)]-chunk.worldSurface[z*16+std::max(x-1,0)];
            int gz=chunk.worldSurface[std::min(z+1,15)*16+x]-chunk.worldSurface[std::max(z-1,0)*16+x];
            return biomeMaterial(biome,wx,y,wz,true,true,true,gx<=-4 || gz>=4,sn);
        }
        if(biome==Biome::SulfurCaves) if(auto value=sulfurBand(wx,y,wz)) return value;
        if(gradient("minecraft:deepslate",wx,y,wz,0,8)) return Deepslate;
        return {};
    }
    void build(TerrainChunk& chunk,bool veins) {
        for(int x=0;x<16;++x) for(int z=0;z<16;++z) {
            int wx=chunk.chunkX*16+x,wz=chunk.chunkZ*16+z;
            int start=chunk.worldSurface[z*16+x];
            Biome surfaceBiome=biomeAt(chunk,x,start,z);
            if(surfaceBiome==Biome::ErodedBadlands) {
                double buffer=std::min(std::abs(badSurface.sample(wx,0,wz)*8.25),badPillar.sample(wx*.2,0,wz*.2)*15.0);
                if(buffer>0) {
                    double roof=std::abs(badRoof.sample(wx*.75,0,wz*.75)*1.5);
                    int top=std::min(319,static_cast<int>(std::floor(64+std::min(buffer*buffer*2.5,std::ceil(roof*50)+24))));
                    bool water=false;
                    if(start<=top) {
                        for(int y=top;y>=-64;--y) {auto b=chunk.at(x,y,z);if(b==Stone) break;if(b==Water){water=true;break;}}
                        if(!water) for(int y=top;y>=-64 && chunk.at(x,y,z)==Air;--y) setWorldgenBlock(chunk,x,y,z,Stone);
                    }
                }
            }
            float sn=surface.sample(wx,0,wz);
            LegacyRandom columnRandom(positionalSeed(router.seed(),"minecraft:surface",wx,0,wz));
            int depth=static_cast<int>(sn*2.75+3+columnRandom.nextDouble()*.25);
            float second=secondary.sample(wx,0,wz);
            int minSurface=static_cast<int>(std::floor(router.sample(wx,0,wz).chunkSurfaceLevel))+depth-8;
            int gx=chunk.worldSurface[z*16+std::min(x+1,15)]-chunk.worldSurface[z*16+std::max(x-1,0)];
            int gz=chunk.worldSurface[std::min(z+1,15)*16+x]-chunk.worldSurface[std::max(z-1,0)*16+x];
            bool steep=gx<=-4 || gz>=4; // SteepCondition is deliberately directional.
            int above=0,water=-1000000,bottom=320;
            int bandShift=static_cast<int>(std::floor(bandOffset.sample(wx,0,wz)*4.0F+.5F));
            for(int y=319;y>=-64;--y) {
                auto old=chunk.at(x,y,z);
                if(old==Air) {above=0;water=-1000000;continue;}
                if(isFluid(old)) {if(water==-1000000) water=y+1;continue;}
                if(bottom>=y) {
                    bottom=y;
                    while(bottom>-64 && isSolid(chunk.at(x,bottom-1,z))) --bottom;
                }
                ++above;int below=y-bottom+1;Biome biome=biomeAt(chunk,x,y,z);
                Opt result;
                if(gradient("minecraft:bedrock_floor",wx,y,wz,-64,-59)) result=Bedrock;
                if(!result && veins) result=ore(wx,y,wz);
                if(!result && y>=minSurface) {
                    bool top=above<=1,ceiling=below<=1,dry=water==-1000000 || y>=water-1;
                    bool shallow=water==-1000000 || y+above>=water-6-depth;
                    bool under=above<=1+depth,hole=depth<=0;
                    Block grit=ceiling?Stone:Gravel;
                    bool band=(sn>=-.909 && sn<=-.5454)||(sn>=-.1818 && sn<=.1818)||(sn>=.5454 && sn<=.909);
                    Block clay=bands[static_cast<std::size_t>((y+bandShift+384)%192)];
                    bool bad=biome==Biome::Badlands || biome==Biome::WoodedBadlands || biome==Biome::ErodedBadlands;
                    bool frozen=biome==Biome::FrozenOcean || biome==Biome::DeepFrozenOcean;
                    if(top && biome==Biome::WoodedBadlands && y>=97+2*depth) result=band?CoarseDirt:(dry?Grass:Dirt);
                    if(!result && top && y<63 && ((biome==Biome::Swamp && y>=62)||(biome==Biome::MangroveSwamp && y>=60)) && swamp.sample(wx,0,wz)>=0) result=Water;
                    bool mid=y+above>=74+depth;
                    if(!result && bad) {
                        if(top) {
                            if(y>=256) result=OrangeTerracotta;
                            else if(mid) result=band?Terracotta:clay;
                            else if(dry) result=ceiling?RedSandstone:RedSand;
                            else if(!hole) result=OrangeTerracotta;
                            else if(shallow) result=WhiteTerracotta;
                            else result=grit;
                        } else if(y+above>=63-depth) result=y>=63 && !mid?OrangeTerracotta:clay;
                        else if(under && shallow) result=WhiteTerracotta;
                    }
                    if(!result && top && dry) result=frozen && hole?Opt{Air}:biomeMaterial(biome,wx,y,wz,true,ceiling,dry,steep,sn);
                    if(!result && shallow) {
                        if(top && frozen && hole) result=Water;
                        else if(under) result=biomeMaterial(biome,wx,y,wz,false,ceiling,dry,steep,sn);
                        else {
                            int range=biome==Biome::Desert?30:6;
                            int extra=static_cast<int>((static_cast<double>(second)+1)/2*range);
                            if((biome==Biome::Desert || biome==Biome::WarmOcean || biome==Biome::Beach || biome==Biome::SnowyBeach) && above<=1+depth+extra) result=Sandstone;
                        }
                    }
                    if(!result && top) {
                        if(biome==Biome::FrozenPeaks || biome==Biome::JaggedPeaks) result=Stone;
                        else if(biome==Biome::WarmOcean || biome==Biome::LukewarmOcean || biome==Biome::DeepLukewarmOcean) result=ceiling?Sandstone:Sand;
                        else result=grit;
                    }
                }
                if(!result && biome==Biome::SulfurCaves) result=sulfurBand(wx,y,wz);
                if(!result && gradient("minecraft:deepslate",wx,y,wz,0,8)) result=Deepslate;
                if(result) {
                    setWorldgenBlock(chunk,x,y,z,*result);
                    if(isFluid(*result)) chunk.fluidPostProcessing.push_back({x,y,z});
                }
            }
            if(surfaceBiome==Biome::FrozenOcean || surfaceBiome==Biome::DeepFrozenOcean) {
                double iceberg=std::min(std::abs(iceSurface.sample(wx,0,wz)*8.25),icePillar.sample(wx*1.28,0,wz*1.28)*15.0);
                if(iceberg>1.8) {
                    double roof=std::abs(iceRoof.sample(wx*1.17,0,wz*1.17)*1.5);
                    double size=std::min(iceberg*iceberg*1.2,std::ceil(roof*40)+14);
                    if(meltsFrozenOceanIceberg(surfaceBiome,wx,wz)) size-=2;
                    if(size<=2) continue;
                    double top=63+size,low=63-size-7;
                    LegacyRandom random(positionalSeed(router.seed(),"minecraft:surface",wx,0,wz));
                    int maxSnow=2+random.nextInt(4),snowY=81+random.nextInt(10),snowDepth=0;
                    for(int y=std::min(319,std::max(start,static_cast<int>(top)+1));y>=std::max(-64,minSurface);--y) {
                        Block b=chunk.at(x,y,z);
                        if((b==Air && y<static_cast<int>(top) && random.nextDouble()>.01)||
                            (b==Water && y>static_cast<int>(low) && y<63 && random.nextDouble()>.15)) {
                            bool snow=snowDepth<=maxSnow && y>snowY;
                            setWorldgenBlock(chunk,x,y,z,snow?Snow:PackedIce);if(snow) ++snowDepth;
                        }
                    }
                }
            }
        }
    }
};
} // namespace
void buildMaterials(TerrainChunk& chunk,const OverworldNoiseRouter& router,bool veins,BlockBiomeGetter biomes) {Materials(router,std::move(biomes)).build(chunk,veins);}
TopMaterialRule makeTopMaterialRule(const TerrainChunk& chunk,const OverworldNoiseRouter& router,bool veins,BlockBiomeGetter biomes) {
    return [materials=std::make_shared<Materials>(router,std::move(biomes)),&chunk,veins](int x,int y,int z,bool underFluid) {
        return materials->top(chunk,x,y,z,underFluid,veins);
    };
}
} // namespace mcworld::detail

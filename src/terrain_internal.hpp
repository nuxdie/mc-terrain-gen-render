#pragma once
#include "mcworld/terrain.hpp"
#include "noise.hpp"
#include <map>
#include <tuple>
#include <functional>
#include <optional>

namespace mcworld::detail {
inline int floorDiv(int x,int d) { int q=x/d; return q-(x%d<0); }
// Java's 48-bit LCG, including bounded rejection and signed nextLong halves.
class LegacyRandom {
    std::uint64_t state_;
public:
    explicit LegacyRandom(std::uint64_t seed):state_((seed^0x5deece66dULL)&((1ULL<<48)-1)){}
    std::uint32_t bits(int n) {state_=(state_*0x5deece66dULL+11)&((1ULL<<48)-1);return state_>>(48-n);}
    int nextInt(int bound) {
        if((bound & -bound)==bound) return (static_cast<std::uint64_t>(bound)*bits(31))>>31;
        std::uint32_t value,mod;
        do {value=bits(31);mod=value%bound;} while(value-mod+static_cast<std::uint32_t>(bound-1)>=0x80000000U);
        return static_cast<int>(mod);
    }
    float nextFloat() {return bits(24)*0x1p-24F;}
    double nextDouble() {auto a=bits(26);auto b=bits(27);return ((static_cast<std::uint64_t>(a)<<27)+b)*0x1p-53;}
    std::uint64_t nextLong() {
        auto high=bits(32); auto low=bits(32);
        return (static_cast<std::uint64_t>(high)<<32)+static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(low)));
    }
};
std::uint64_t positionalSeed(std::int64_t seed,const char* key,int x,int y,int z);
struct Substance {Block block; bool schedule=false;};
class Aquifer {
public:
    Aquifer(const OverworldNoiseRouter& router,bool enabled);
    Substance sample(int x,int y,int z,float density);
private:
    struct Fluid {
        int level; Block type;
        Block at(int y) const {return y<level?type:Block::Air;}
        bool operator==(const Fluid&) const = default;
    };
    struct Center {int x,y,z; Fluid fluid;};
    const OverworldNoiseRouter& router_;
    bool enabled_;
    NormalNoise barrier_,floodedness_,spread_,lava_;
    std::map<std::tuple<int,int,int>,Center> centers_;
    std::map<std::pair<int,int>,int> surfaces_;
    int surface(int x,int z);
    Fluid fluid(int x,int y,int z);
    const Center& center(int x,int y,int z);
    double pressure(int x,int y,int z,Fluid a,Fluid b,double& noise);
};
using BlockBiomeGetter = std::function<Biome(int,int,int)>; // world block coordinates
std::uint64_t biomeZoomSeed(std::int64_t seed);
BlockPosition zoomedBiomeQuart(std::uint64_t seed,int x,int y,int z);
BlockBiomeGetter makeBlockBiomeGetter(const TerrainChunk& chunk,const OverworldNoiseRouter& router,const BiomeSource& source,bool clampY);
bool meltsFrozenOceanIceberg(Biome biome,int x,int z);
void buildMaterials(TerrainChunk& chunk,const OverworldNoiseRouter& router,bool veins,BlockBiomeGetter biomes={});
void setWorldgenBlock(TerrainChunk& chunk,int x,int y,int z,Block block);
using CarvingMask = std::vector<bool>; // z, x, y; same storage order as TerrainChunk
CarvingMask buildCarvingMask(int chunkX,int chunkZ,std::int64_t seed);
using TopMaterialRule = std::function<std::optional<Block>(int,int,int,bool)>;
void applyCarvingMask(TerrainChunk& chunk,const CarvingMask& mask,Aquifer& aquifer,const TopMaterialRule& topMaterial);
TopMaterialRule makeTopMaterialRule(const TerrainChunk& chunk,const OverworldNoiseRouter& router,bool veins,BlockBiomeGetter biomes={});
void carve(TerrainChunk& chunk,const OverworldNoiseRouter& router,const BiomeSource& biomes,Aquifer& aquifer,bool veins);
} // namespace mcworld::detail

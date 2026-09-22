#include "terrain_internal.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mcworld {
namespace {
std::size_t index(int x,int y,int z) {
    if(x<0 || x>=16 || z<0 || z>=16 || y<TerrainChunk::minY || y>=TerrainChunk::maxY)
        throw std::out_of_range("Terrain block coordinate outside chunk");
    return static_cast<std::size_t>((z*16+x)*TerrainChunk::height+y-TerrainChunk::minY);
}
}
Block TerrainChunk::at(int x,int y,int z) const {return blocks.at(index(x,y,z));}
void TerrainChunk::set(int x,int y,int z,Block block) {blocks.at(index(x,y,z))=block;}
Biome TerrainChunk::biomeAt(int x,int y,int z) const {
    (void)index(x,y,z);
    return biomes[(z/4*4+x/4)*(height/4)+(y-minY)/4];
}
void TerrainChunk::primeHeightmaps() {
    worldSurface.fill(minY);oceanFloor.fill(minY);motionBlocking.fill(minY);
    for(int z=0;z<16;++z) for(int x=0;x<16;++x) for(int y=maxY-1;y>=minY;--y) {
        Block b=at(x,y,z);int column=z*16+x;
        if(b!=Block::Air && worldSurface[column]==minY) worldSurface[column]=y+1;
        if(blocksMotion(b) && oceanFloor[column]==minY) oceanFloor[column]=y+1;
        if((blocksMotion(b) || isFluid(b)) && motionBlocking[column]==minY) motionBlocking[column]=y+1;
    }
    motionBlockingNoLeaves=motionBlocking; // No leaf states in the terrain palette.
}
Biome BiomeSource::sample(const OverworldNoiseRouter& router,int x,int y,int z) const {
    return sampleOverworldBiome(router,x,y,z);
}
class OverworldTerrainGenerator::Impl {
public:
    Impl(const OverworldNoiseRouter& router,TerrainOptions options):router_(router),options_(std::move(options)) {
        if(!options_.biomes) options_.biomes=std::make_shared<BiomeSource>();
    }
    TerrainChunk generate(int cx,int cz) {
        // Reserve space for the carver neighborhood, aquifer probes, and interpolation halos.
        constexpr std::int64_t margin=512;
        std::int64_t ox=static_cast<std::int64_t>(cx)*16,oz=static_cast<std::int64_t>(cz)*16;
        if(ox<std::numeric_limits<int>::min()+margin || ox>std::numeric_limits<int>::max()-margin ||
            oz<std::numeric_limits<int>::min()+margin || oz>std::numeric_limits<int>::max()-margin)
            throw std::invalid_argument("Terrain chunk is outside supported coordinate range");
        TerrainChunk chunk;chunk.chunkX=cx;chunk.chunkZ=cz;
        int originX=static_cast<int>(ox),originZ=static_cast<int>(oz);
        for(int z=0;z<4;++z) for(int x=0;x<4;++x) for(int y=0;y<96;++y)
            chunk.biomes[(z*4+x)*96+y]=options_.biomes->sample(router_,originX+x*4,-64+y*4,originZ+z*4);
        detail::Aquifer aquifer(router_,options_.aquifers);
        for(int z=0;z<16;++z) for(int x=0;x<16;++x) for(int y=319;y>=-64;--y) {
            auto density=router_.sampleFinalDensity(originX+x,y,originZ+z);
            auto substance=aquifer.sample(originX+x,y,originZ+z,density);
            chunk.set(x,y,z,substance.block);
            if(substance.schedule && isFluid(substance.block)) chunk.fluidPostProcessing.push_back({x,y,z});
        }
        chunk.primeHeightmaps();
        detail::buildMaterials(chunk,router_,options_.oreVeins,detail::makeBlockBiomeGetter(chunk,router_,*options_.biomes,true));
        chunk.primeHeightmaps();
        if(options_.carvers) detail::carve(chunk,router_,*options_.biomes,aquifer,options_.oreVeins);
        chunk.primeHeightmaps();
        auto& updates=chunk.fluidPostProcessing;
        std::erase_if(updates,[&](auto p){return !isFluid(chunk.at(p.x,p.y,p.z));});
        std::sort(updates.begin(),updates.end(),[](auto a,auto b){return std::tie(a.z,a.x,a.y)<std::tie(b.z,b.x,b.y);});
        updates.erase(std::unique(updates.begin(),updates.end()),updates.end());
        return chunk;
    }
private:
    const OverworldNoiseRouter& router_;
    TerrainOptions options_;
};
OverworldTerrainGenerator::OverworldTerrainGenerator(const OverworldNoiseRouter& router,TerrainOptions options)
    :impl_(std::make_unique<Impl>(router,std::move(options))){}
OverworldTerrainGenerator::~OverworldTerrainGenerator()=default;
OverworldTerrainGenerator::OverworldTerrainGenerator(OverworldTerrainGenerator&&) noexcept=default;
OverworldTerrainGenerator& OverworldTerrainGenerator::operator=(OverworldTerrainGenerator&&) noexcept=default;
TerrainChunk OverworldTerrainGenerator::generate(int x,int z) {return impl_->generate(x,z);}
} // namespace mcworld

namespace mcworld::detail {
void setWorldgenBlock(TerrainChunk& chunk,int x,int y,int z,Block block) {
    chunk.set(x,y,z,block);
    // Material gradients and carver topMaterial observe the live WORLD_SURFACE_WG
    // heightmap. The remaining heightmaps are primed at the end of each pass.
    int& height=chunk.worldSurface[z*16+x];
    if(block!=Block::Air) height=std::max(height,y+1);
    else if(height==y+1) {
        height=y;
        while(height>TerrainChunk::minY && chunk.at(x,height-1,z)==Block::Air) --height;
    }
}
}

#include "terrain_internal.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>

namespace mcworld::detail {
std::uint64_t biomeZoomSeed(std::int64_t seed) {
    // BiomeManager.obfuscateSeed: SHA-256 of eight little-endian seed bytes,
    // reading the first eight digest bytes as a little-endian long. A single
    // fixed-size block keeps this operation dependency-free.
    constexpr std::uint32_t k[]{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<std::uint32_t,64> w{};
    for(int i=0;i<8;++i) w[i/4]|=static_cast<std::uint32_t>((static_cast<std::uint64_t>(seed)>>(8*i))&255)<<(24-8*(i%4));
    w[2]=0x80000000;w[15]=64;
    for(int i=16;i<64;++i) {
        auto a=w[i-15],b=w[i-2];
        w[i]=w[i-16]+(std::rotr(a,7)^std::rotr(a,18)^(a>>3))+w[i-7]+(std::rotr(b,17)^std::rotr(b,19)^(b>>10));
    }
    std::uint32_t a=0x6a09e667,b=0xbb67ae85,c=0x3c6ef372,d=0xa54ff53a,
        e=0x510e527f,f=0x9b05688c,g=0x1f83d9ab,h=0x5be0cd19;
    for(int i=0;i<64;++i) {
        auto t1=h+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        auto t2=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    a+=0x6a09e667;b+=0xbb67ae85;
    std::uint64_t result=0;
    for(int i=0;i<8;++i) result|=static_cast<std::uint64_t>(((i<4?a:b)>>(24-8*(i%4)))&255)<<(i*8);
    return result;
}

BlockPosition zoomedBiomeQuart(std::uint64_t seed,int x,int y,int z) {
    // Generator coordinate validation reserves enough room for this two-block shift.
    int px=floorDiv(x-2,4),py=floorDiv(y-2,4),pz=floorDiv(z-2,4);
    double fx=(x-2-px*4)/4.0,fy=(y-2-py*4)/4.0,fz=(z-2-pz*4)/4.0;
    auto next=[](std::uint64_t v,std::uint64_t salt){return v*(v*6364136223846793005ULL+1442695040888963407ULL)+salt;};
    auto fiddle=[](std::uint64_t v){return (((v>>24)&1023)/1024.0-.5)*.9;};
    double best=std::numeric_limits<double>::infinity();BlockPosition result;
    for(int i=0;i<8;++i) {
        int dx=(i>>2)&1,dy=(i>>1)&1,dz=i&1;
        int qx=px+dx,qy=py+dy,qz=pz+dz;
        auto v=seed;
        for(int coordinate:{qx,qy,qz,qx,qy,qz}) v=next(v,static_cast<std::uint64_t>(coordinate));
        double xx=fx-dx+fiddle(v);v=next(v,seed);
        double yy=fy-dy+fiddle(v);v=next(v,seed);
        double zz=fz-dz+fiddle(v),distance=zz*zz+yy*yy+xx*xx;
        if(distance<best) {best=distance;result={qx,qy,qz};}
    }
    return result;
}

BlockBiomeGetter makeBlockBiomeGetter(const TerrainChunk& chunk,const OverworldNoiseRouter& router,const BiomeSource& source,bool clampY) {
    return [&chunk,&router,&source,clampY,seed=biomeZoomSeed(router.seed()),
            cache=std::map<std::tuple<int,int,int>,Biome>{}](int x,int y,int z) mutable {
        auto q=zoomedBiomeQuart(seed,x,y,z);
        if(clampY) q.y=std::clamp(q.y,-16,79); // ChunkAccess.getNoiseBiome
        int bx=q.x*4,bz=q.z*4,lx=bx-chunk.chunkX*16,lz=bz-chunk.chunkZ*16;
        if(lx>=0 && lx<16 && lz>=0 && lz<16 && q.y>=-16 && q.y<80) return chunk.biomeAt(lx,q.y*4,lz);
        auto [it,inserted]=cache.try_emplace({q.x,q.y,q.z});
        if(inserted) it->second=source.sample(router,bx,q.y*4,bz);
        return it->second;
    };
}

namespace {
// The frozen-ocean temperature modifier uses fixed-seed, offset-free 2-D
// simplex noises, independent of the world's density-noise seed convention.
class TemperatureSimplex {
    std::array<int,256> permutation;
public:
    explicit TemperatureSimplex(LegacyRandom& random) {
        for(int i=0;i<3;++i) (void)random.nextDouble(); // discarded offsets still consume RNG
        std::iota(permutation.begin(),permutation.end(),0);
        for(int i=0;i<256;++i) {int j=i+random.nextInt(256-i);std::swap(permutation[i],permutation[j]);}
    }
    float sample(double x,double z) const {
        static const double skew=(std::sqrt(3.0)-1)*.5,unskew=(3-std::sqrt(3.0))/6;
        double s=(x+z)*skew;
        int i=static_cast<int>(std::floor(x+s)),j=static_cast<int>(std::floor(z+s));
        double t=(i+j)*unskew,dx=x-(i-t),dz=z-(j-t);
        int di=dx>dz?1:0,dj=1-di;
        auto p=[&](int v){return permutation[v&255];};
        auto corner=[](int gradient,double a,double b) {
            constexpr int gradients[][2]{{1,1},{-1,1},{1,-1},{-1,-1},{1,0},{-1,0},{1,0},{-1,0},{0,1},{0,-1},{0,1},{0,-1}};
            double attenuation=.5-a*a-b*b;
            if(attenuation<0) return 0.0;
            attenuation*=attenuation;
            return attenuation*attenuation*(gradients[gradient][0]*a+gradients[gradient][1]*b);
        };
        double a=corner(p((i&255)+p(j&255))%12,dx,dz);
        double b=corner(p((i&255)+di+p((j&255)+dj))%12,dx-di+unskew,dz-dj+unskew);
        double c=corner(p((i&255)+1+p((j&255)+1))%12,dx-1+2*unskew,dz-1+2*unskew);
        return static_cast<float>(70*(a+b+c));
    }
};
}
bool meltsFrozenOceanIceberg(Biome biome,int x,int z) {
    if(biome==Biome::DeepFrozenOcean) return true; // base 0.5 or modified 0.2, both > 0.1
    static const auto layers=[] {
        LegacyRandom random(3456);
        return std::array{TemperatureSimplex(random),TemperatureSimplex(random),TemperatureSimplex(random)};
    }();
    static const auto info=[] {LegacyRandom random(2345);return TemperatureSimplex(random);}();
    float large=0;
    constexpr double frequencies[]{1,.5,.25};
    constexpr float amplitudes[]{.14285715F,.2857143F,.5714286F};
    for(int i=0;i<3;++i) large+=amplitudes[i]*layers[i].sample(x*.05*frequencies[i],z*.05*frequencies[i]);
    return static_cast<double>(large*7.0F)+info.sample(x*.2,z*.2)<.3 && info.sample(x*.09,z*.09)<.8;
}
} // namespace mcworld::detail

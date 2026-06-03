#include "dsp/ir_wav.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <algorithm>

namespace ithaca::dsp {
namespace {
bool readWavMono(const std::string& path, std::vector<float>& out, int& sr_out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    char riff[4]; f.read(riff,4); if (std::memcmp(riff,"RIFF",4)!=0) return false;
    uint32_t fsz; f.read(reinterpret_cast<char*>(&fsz),4);
    char wave[4]; f.read(wave,4); if (std::memcmp(wave,"WAVE",4)!=0) return false;
    uint16_t fmt=0, ch=0, bps=0; uint32_t sr=0, dsz=0;
    while (f.good()) {
        char id[4]; f.read(id,4); uint32_t csz; f.read(reinterpret_cast<char*>(&csz),4);
        if (std::memcmp(id,"fmt ",4)==0) {
            f.read(reinterpret_cast<char*>(&fmt),2); f.read(reinterpret_cast<char*>(&ch),2);
            f.read(reinterpret_cast<char*>(&sr),4); f.seekg(6,std::ios::cur);
            f.read(reinterpret_cast<char*>(&bps),2);
            if (csz>16) f.seekg(csz-16,std::ios::cur);
        } else if (std::memcmp(id,"data",4)==0) { dsz=csz; break; }
        else f.seekg(csz,std::ios::cur);
    }
    if (ch==0||sr==0||dsz==0) return false;
    sr_out=(int)sr;
    if (fmt==3 && bps==32) {
        int n=(int)(dsz/(4*ch)); out.resize((size_t)n);
        if (ch==1) f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)n*4);
        else { std::vector<float> b((size_t)n*ch); f.read(reinterpret_cast<char*>(b.data()),dsz);
               for(int i=0;i<n;++i){float s=0;for(int c=0;c<ch;++c)s+=b[(size_t)(i*ch+c)];out[(size_t)i]=s/ch;} }
    } else if (fmt==1 && bps==16) {
        int n=(int)(dsz/(2*ch)); out.resize((size_t)n);
        std::vector<int16_t> b((size_t)n*ch); f.read(reinterpret_cast<char*>(b.data()),dsz);
        for(int i=0;i<n;++i){float s=0;for(int c=0;c<ch;++c)s+=b[(size_t)(i*ch+c)]/32768.f;out[(size_t)i]=s/ch;}
    } else return false;
    return true;
}
}

bool loadIrWavMono(const std::string& path, float engine_sr, int max_len, std::vector<float>& out) {
    std::vector<float> raw; int sr=0;
    if (!readWavMono(path, raw, sr) || raw.empty()) return false;
    if (engine_sr>0.f && sr>0 && std::fabs((float)sr-engine_sr)>1.f) {
        float ratio=engine_sr/(float)sr; int nl=(int)((float)raw.size()*ratio); if(nl<2)nl=2;
        std::vector<float> rs((size_t)nl);
        for(int i=0;i<nl;++i){ float sp=(float)i/ratio; int idx=(int)sp; float fr=sp-(float)idx;
            if(idx+1<(int)raw.size()) rs[(size_t)i]=raw[(size_t)idx]*(1.f-fr)+raw[(size_t)(idx+1)]*fr;
            else if(idx<(int)raw.size()) rs[(size_t)i]=raw[(size_t)idx]*(1.f-fr); else rs[(size_t)i]=0.f; }
        raw.swap(rs);
    }
    if ((int)raw.size()>max_len) raw.resize((size_t)max_len);
    out.swap(raw);
    return true;
}
}

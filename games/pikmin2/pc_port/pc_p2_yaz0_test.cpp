// Authored, asset-free fixtures exercise the actual production Yaz0 decoders.
#include "pc_p2_yaz0_test.h"
#include "JSystem/JKernel/JKRDecomp.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

extern int pc_p2_yaz0_dvd_decoder_test(u8*, u32, u8*, u32, u32, u8*, u32*);

int pc_p2_yaz0_self_test()
{
    constexpr u32 size = 65536;
    std::vector<u8> expected(size), compressed(16,0);
    std::memcpy(compressed.data(),"Yaz0",4);
    for (unsigned i=0;i<4;++i) compressed[4+i]=u8(size>>(24-8*i));
    for (u32 i=0;i<size;++i) expected[i]=u8(1+(i*37u+i/13u)%251u);
    // Literal groups intentionally put meaningful output beyond byte 0x100,
    // where the old native-endian length stopped this exact 65536-byte case.
    for (u32 i=0;i<size;i+=8) {
        compressed.push_back(0xff);
        compressed.insert(compressed.end(),expected.begin()+i,expected.begin()+i+8);
    }
    u32 legacyLength=0;
    std::memcpy(&legacyLength,compressed.data()+4,4);
    if (legacyLength==size || legacyLength!=256 || expected[legacyLength]==0) return 1;
    unsigned cases=0;
    auto check=[&](u32 offset,u32 maximum) {
        const u32 count=offset<size ? std::min(maximum,size-offset) : 0;
        std::vector<u8> memory(maximum+32,0xcd),stream(maximum+32,0xcd),history(0x1120,0);
        JKRDecomp::decodeSZS(compressed.data(),memory.data(),maximum,offset);
        u32 written=0;
        const int result=pc_p2_yaz0_dvd_decoder_test(compressed.data(),u32(compressed.size()),
            stream.data(),maximum,offset,history.data(),&written);
        const bool valid=offset<=size;
        bool pass=(valid ? result==0 : result==-1) && written==count;
        pass=pass && std::equal(memory.begin(),memory.begin()+count,expected.begin()+std::min(offset,size));
        pass=pass && std::equal(stream.begin(),stream.begin()+count,memory.begin());
        pass=pass && std::all_of(memory.begin()+count,memory.end(),[](u8 v){return v==0xcd;});
        pass=pass && std::all_of(stream.begin()+count,stream.end(),[](u8 v){return v==0xcd;});
        std::printf("P2_YAZ0 offset=%u maximum=%u decoded=%u %s\n",offset,maximum,written,pass?"PASS":"FAIL");
        ++cases;return pass;
    };
    for (const auto& c:std::vector<std::pair<u32,u32>>{{0,size},{0,257},{0,size+17},{0,0},
        {255,8193},{size-1,12},{size,12},{size+1,12}}) {
        if (!check(c.first,c.second)) return 2;
    }
    // A real back-reference token exercises production copying, separately
    // from the literal-only offset fixtures (which need no skipped history).
    std::vector<u8> repeated(16,0);std::memcpy(repeated.data(),"Yaz0",4);
    // One literal followed by a distance-zero, 31-byte overlapping copy.
    repeated[7]=32;repeated.insert(repeated.end(),{0x80,0x6b,0,0,13});
    u8 a[40],b[40],history[0x1120]{};std::memset(a,0xcd,sizeof(a));std::memset(b,0xcd,sizeof(b));
    JKRDecomp::decodeSZS(repeated.data(),a,32,0);
    u32 written=0;
    if(pc_p2_yaz0_dvd_decoder_test(repeated.data(),u32(repeated.size()),b,32,0,history,&written)
       || written!=32 || !std::all_of(a,a+32,[](u8 v){return v==0x6b;})
       || std::memcmp(a,b,40)) return 3;
    ++cases;
    std::printf("{\"status\":\"passed\",\"cases\":%u,\"authored_expanded_bytes\":65536,"
        "\"old_native_endian_length_negative_control\":%u,\"boundary\":\"actual in-memory and DVD streaming decoder loops; authored literals/overlapping copy and bounded literal offsets/maxima; no DVD reader, ARAM DMA, game assets or scene claim\"}\n",cases,legacyLength);
    return 0;
}

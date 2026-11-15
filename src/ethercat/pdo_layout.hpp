// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "model/types.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace niceecatm::pdo_layout {
constexpr size_t MaxEntries=2048;
struct SyncManager { unsigned type=0, bytes=0; };

// SOEM can return zero both for a valid empty image and for rejected segment
// configuration. A completed map, even without PDOs, has at least one segment.
// Leave one transport frame available for other master requests.
inline bool mappingComplete(int mapperExtent,unsigned segments,unsigned frameCapacity) {
    return mapperExtent>=0 && segments>0 && segments<frameCapacity;
}

// Decode category payloads, excluding the SII category/length header. These
// parsers deliberately have no EtherCAT side effects and never trust EEPROM
// counts without checking the category boundary first.
inline bool strings(const std::vector<uint8_t>& bytes,std::vector<std::string>& result) {
    std::vector<std::string> parsed(1);
    if(bytes.empty())return false;
    size_t at=1;
    for(unsigned n=0;n<bytes[0];++n) {
        if(at>=bytes.size())return false;
        const size_t length=bytes[at++];
        if(length>bytes.size()-at)return false;
        std::string text(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+length));
        if(text.find('\0')!=std::string::npos)return false;
        parsed.push_back(std::move(text));at+=length;
    }
    if(at!=bytes.size()&&!(at+1==bytes.size()&&bytes[at]==0))return false;
    result=std::move(parsed);return true;
}
inline bool sii(const std::vector<uint8_t>& bytes,bool input,const std::vector<std::string>& names,
                std::vector<PdoEntry>& result,std::string& issue) {
    std::vector<PdoEntry> parsed;
    size_t at=0;
    while(at<bytes.size()) {
        if(bytes.size()-at<8){issue="Truncated SII PDO header.";return false;}
        const unsigned pdo=unsigned(bytes[at])|(unsigned(bytes[at+1])<<8);
        const unsigned count=bytes[at+2],sm=bytes[at+3];at+=8;
        if(pdo==0||size_t(count)*8>bytes.size()-at){issue="Invalid SII PDO entries.";return false;}
        for(unsigned n=0;n<count;++n,at+=8) {
            // 0xff denotes an optional, unassigned PDO. It is not in IOmap.
            if(sm==0xff)continue;
            if(sm>=8||parsed.size()>=MaxEntries){issue="SII PDO limits exceeded.";return false;}
            PdoEntry entry;entry.input=input;entry.syncManager=int(sm);entry.pdoIndex=uint16_t(pdo);
            entry.index=uint16_t(unsigned(bytes[at])|(unsigned(bytes[at+1])<<8));entry.subindex=bytes[at+2];
            entry.bitLength=bytes[at+5];entry.padding=entry.index==0&&entry.subindex==0;entry.source="SII";
            if(!entry.bitLength){issue="Zero-width SII PDO entry.";return false;}
            entry.type=pdoType(bytes[at+4],entry.bitLength);
            if(bytes[at+3]<names.size()&&!names[bytes[at+3]].empty())entry.names.push_back({0,names[bytes[at+3]]});
            parsed.push_back(std::move(entry));
        }
    }
    result=std::move(parsed);return true;
}

// SOEM maps SMs in increasing SM number. Each SM occupies rounded bytes in
// IOmap. Refuse an interior fractional-byte SM rather than accidentally
// treating its hardware padding as the next signal's bits.
inline bool validate(std::vector<PdoEntry>& entries,bool input,unsigned expectedBits,
                     const std::array<SyncManager,8>& managers,std::string& issue) {
    if(entries.size()>MaxEntries){issue="PDO entry limit exceeded.";return false;}
    std::array<unsigned,8> widths{};
    for(const auto& entry:entries) {
        if(entry.input!=input||entry.syncManager<0||entry.syncManager>=8||entry.bitLength==0||
           entry.bitLength>65535||widths[size_t(entry.syncManager)]>65535-entry.bitLength) {
            issue="Invalid PDO entry layout.";return false;
        }
        widths[size_t(entry.syncManager)]+=entry.bitLength;
    }
    unsigned total=0;int last=-1;
    for(size_t sm=0;sm<managers.size();++sm)if(managers[sm].type==(input?4u:3u)&&widths[sm])last=int(sm);
    for(size_t sm=0;sm<managers.size();++sm) {
        const auto& manager=managers[sm];
        if(widths[sm]&&manager.type!=(input?4u:3u)){issue="PDO direction differs from the mapped SM.";return false;}
        if(manager.type!=(input?4u:3u))continue;
        if((widths[sm]+7)/8!=manager.bytes){issue="PDO entries differ from the mapped SM length.";return false;}
        if(widths[sm]%8&&int(sm)!=last){issue="Multiple SMs contain unsupported bit padding.";return false;}
        total+=widths[sm];
    }
    if(total!=expectedBits){issue="PDO entries differ from the actual process-image length.";return false;}
    std::stable_sort(entries.begin(),entries.end(),[](const PdoEntry& a,const PdoEntry& b){return a.syncManager<b.syncManager;});
    unsigned bit=0;for(auto& entry:entries){entry.bitOffset=bit;bit+=entry.bitLength;}
    return true;
}
} // namespace niceecatm::pdo_layout

// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "model/types.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace niceecatm {
std::string pdoEntryName(const PdoEntry& entry,bool chinese) {
    const uint32_t exact=chinese?0x0804:0x0409;
    const uint32_t language=chinese?4:9;
    for(const auto& name:entry.names)if(name.lcid==exact&&!name.text.empty())return name.text;
    for(const auto& name:entry.names)if((name.lcid&0x3ff)==language&&!name.text.empty())return name.text;
    for(const auto& name:entry.names)if(name.lcid==0&&!name.text.empty())return name.text;
    // An English-only ESI keeps its original name in the Chinese interface.
    for(const auto& name:entry.names)if((name.lcid&0x3ff)==9&&!name.text.empty())return name.text;
    for(const auto& name:entry.names)if(!name.text.empty())return name.text;
    return {};
}
namespace {
std::string integerType(bool sign,unsigned width,unsigned maximum) {
    return width>0&&width<=maximum&&width<=64?std::string(sign?"INT":"UINT")+std::to_string(width):std::string{};
}
}
std::string pdoType(const std::string& name,unsigned bits) {
    auto value=name;
    value.erase(0,value.find_first_not_of(" \t\r\n"));
    const auto end=value.find_last_not_of(" \t\r\n");
    if(end!=std::string::npos)value.resize(end+1);
    for(auto& c:value)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if(value=="BOOL")return bits==1?"BOOL":"";
    if(value=="REAL"||value=="REAL32")return bits==32?"REAL32":"";
    if(value=="LREAL"||value=="REAL64")return bits==64?"REAL64":"";
    if(value=="SINT")return integerType(true,bits,8);
    if(value=="USINT"||value=="BYTE")return integerType(false,bits,8);
    if(value=="INT")return integerType(true,bits,16);
    if(value=="UINT"||value=="WORD")return integerType(false,bits,16);
    if(value=="DINT")return integerType(true,bits,32);
    if(value=="UDINT"||value=="DWORD")return integerType(false,bits,32);
    if(value=="LINT")return integerType(true,bits,64);
    if(value=="ULINT"||value=="LWORD")return integerType(false,bits,64);
    if(value=="BIT")return bits==1?"BOOL":"";
    if(value=="BITARR8")return bits==8?"UINT8":"";
    if(value=="BITARR16")return bits==16?"UINT16":"";
    if(value=="BITARR32")return bits==32?"UINT32":"";
    for(const auto* prefix:{"UINT","INT","BIT"}) {
        const std::string tag=prefix;
        if(value.compare(0,tag.size(),tag)!=0)continue;
        const auto suffix=value.substr(tag.size());
        if(suffix.empty()||suffix.size()>2||!std::all_of(suffix.begin(),suffix.end(),[](char c){return c>='0'&&c<='9';}))return {};
        const auto declared=static_cast<unsigned>(std::strtoul(suffix.c_str(),nullptr,10));
        if(declared!=bits)return {};
        if(tag=="BIT"&&bits==1)return "BOOL";
        return integerType(tag=="INT",bits,64);
    }
    return {};
}
std::string pdoType(uint16_t type,unsigned bits) {
    switch(type) {
    case 0x0001:return bits==1?"BOOL":"";
    case 0x0002:return integerType(true,bits,8);
    case 0x0003:return integerType(true,bits,16);
    case 0x0004:return integerType(true,bits,32);
    case 0x0005:return integerType(false,bits,8);
    case 0x0006:return integerType(false,bits,16);
    case 0x0007:return integerType(false,bits,32);
    case 0x0008:return bits==32?"REAL32":"";
    case 0x0010:return integerType(true,bits,24);
    case 0x0011:return bits==64?"REAL64":"";
    case 0x0012:return integerType(true,bits,40);
    case 0x0013:return integerType(true,bits,48);
    case 0x0014:return integerType(true,bits,56);
    case 0x0015:return integerType(true,bits,64);
    case 0x0016:return integerType(false,bits,24);
    case 0x0018:return integerType(false,bits,40);
    case 0x0019:return integerType(false,bits,48);
    case 0x001a:return integerType(false,bits,56);
    case 0x001b:return integerType(false,bits,64);
    case 0x002d:return bits==8?"UINT8":"";
    case 0x002e:return bits==16?"UINT16":"";
    case 0x002f:return bits==32?"UINT32":"";
    default:
        if(type>=0x0030&&type<=0x0037&&bits==unsigned(type-0x0030+1))return bits==1?"BOOL":integerType(false,bits,8);
        return {};
    }
}
}

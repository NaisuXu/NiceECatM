// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "model/types.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace niceecatm {
namespace {
std::string trim(const std::string& value) {
    auto first=value.find_first_not_of(" \t\r\n");
    if(first==std::string::npos) return {};
    return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
int nibble(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
struct Type { unsigned bits=0; bool sign=false, floating=false, string=false; };
Type describe(const std::string& name) {
    if(name=="BOOL") return {1};
    if(name=="STRING") return {0,false,false,true};
    if(name=="REAL32") return {32,false,true};
    if(name=="REAL64") return {64,false,true};
    for(unsigned width=1;width<=64;++width) {
        if(name=="UINT"+std::to_string(width)) return {width};
        if(name=="INT"+std::to_string(width)) return {width,true};
    }
    return {};
}
uint64_t mask(unsigned bits) { return bits==64?UINT64_MAX:((uint64_t(1)<<bits)-1); }
std::vector<uint8_t> littleBytes(uint64_t raw, unsigned bits) {
    std::vector<uint8_t> bytes((bits+7)/8);
    for(size_t i=0;i<bytes.size();++i) bytes[i]=static_cast<uint8_t>(raw>>(i*8));
    return bytes;
}
bool digitsOnly(const std::string& s, int base) {
    if(s.empty()) return false;
    return std::all_of(s.begin(),s.end(),[base](char c){return base==16?nibble(c)>=0:(c>='0'&&c<='9');});
}
}

const char* stateName(uint16_t state) {
    switch(state&0x0f) {
    case 1:return "INIT"; case 2:return "PRE-OP"; case 3:return "BOOT";
    case 4:return "SAFE-OP"; case 8:return "OP"; default:return "UNKNOWN";
    }
}

bool parseHex(const std::string& text, std::vector<uint8_t>& bytes, std::string& error) {
    std::vector<uint8_t> candidate;
    size_t position=0;
    while(position<text.size()) {
        while(position<text.size()&&(std::isspace(static_cast<unsigned char>(text[position]))||text[position]==',')) ++position;
        if(position==text.size()) break;
        size_t end=position;
        while(end<text.size()&&!std::isspace(static_cast<unsigned char>(text[end]))&&text[end]!=',') ++end;
        auto token=text.substr(position,end-position);
        if(token.size()>=2&&token[0]=='0'&&(token[1]=='x'||token[1]=='X')) token.erase(0,2);
        if(token.empty()||token.size()%2) {error="HEX bytes require complete pairs, for example 01 FF 0A.";return false;}
        for(size_t i=0;i<token.size();i+=2) {
            int high=nibble(token[i]),low=nibble(token[i+1]);
            if(high<0||low<0) {error="HEX bytes contain an invalid character.";return false;}
            if(candidate.size()>=1024*1024) {error="HEX data exceeds the 1 MiB editor limit.";return false;}
            candidate.push_back(static_cast<uint8_t>(high*16+low));
        }
        position=end;
    }
    if(candidate.empty()) {error="Enter at least one byte.";return false;}
    bytes=std::move(candidate);error.clear();return true;
}

std::string hexBytes(const std::vector<uint8_t>& bytes) {
    std::ostringstream stream;stream<<std::hex<<std::uppercase<<std::setfill('0');
    for(size_t i=0;i<bytes.size();++i) {if(i)stream<<' ';stream<<std::setw(2)<<unsigned(bytes[i]);}
    return stream.str();
}

bool writeBits(std::vector<uint8_t>& bytes, unsigned bit, unsigned width, uint64_t value, std::string& error) {
    if(width==0||width>64||uint64_t(bit)+width>uint64_t(bytes.size())*8) {error="Bit field is outside the process image.";return false;}
    if(width<64&&value>mask(width)) {error="Value is outside the unsigned bit-field range.";return false;}
    for(unsigned i=0;i<width;++i) {
        const auto index=(bit+i)/8;const auto flag=static_cast<uint8_t>(1u<<((bit+i)%8));
        bytes[index]=static_cast<uint8_t>((bytes[index]&~flag)|((value>>i&1)?flag:0));
    }
    error.clear();return true;
}

uint64_t readBits(const std::vector<uint8_t>& bytes, unsigned bit, unsigned width) {
    if(width==0||width>64||uint64_t(bit)+width>uint64_t(bytes.size())*8) return 0;
    uint64_t raw=0;
    for(unsigned i=0;i<width;++i) raw|=uint64_t((bytes[(bit+i)/8]>>((bit+i)%8))&1)<<i;
    return raw;
}

bool encodeValue(const std::string& text, const std::string& type, bool hex, std::vector<uint8_t>& bytes, std::string& error) {
    const auto descriptor=describe(type);
    if(descriptor.string) {bytes.assign(text.begin(),text.end());error.clear();return true;}
    if(!descriptor.bits) {error="Unsupported data type: "+type;return false;}
    auto value=trim(text);
    if(value.empty()) {error="Enter a value.";return false;}
    if(descriptor.floating) {
        if(hex) {error="REAL values require decimal input; use raw bytes for bit patterns.";return false;}
        std::istringstream stream(value);stream.imbue(std::locale::classic());
        double number=0;stream>>std::noskipws>>number;
        if(!stream||stream.peek()!=std::char_traits<char>::eof()||!std::isfinite(number)) {error="Enter a finite decimal REAL value.";return false;}
        if(descriptor.bits==32) {
            if(std::fabs(number)>std::numeric_limits<float>::max()) {error="Value exceeds the REAL32 range.";return false;}
            float rounded=static_cast<float>(number);uint32_t raw=0;std::memcpy(&raw,&rounded,sizeof raw);
            bytes=littleBytes(raw,32);
        } else {uint64_t raw=0;std::memcpy(&raw,&number,sizeof raw);bytes=littleBytes(raw,64);}
        error.clear();return true;
    }
    if(type=="BOOL") {
        std::string lower=value;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(lower=="true")value="1";else if(lower=="false")value="0";
    }
    try {
        uint64_t raw=0;
        if(hex) {
            if(value.size()>=2&&value[0]=='0'&&(value[1]=='x'||value[1]=='X'))value.erase(0,2);
            if(!digitsOnly(value,16)) {error="HEX integer input must contain unsigned hexadecimal digits.";return false;}
            raw=std::stoull(value,nullptr,16);
            if(raw>mask(descriptor.bits)) {error="HEX integer exceeds the data type's bit width.";return false;}
        } else if(descriptor.sign) {
            auto digits=value;if(digits[0]=='-'||digits[0]=='+')digits.erase(0,1);
            if(!digitsOnly(digits,10)) {error="Enter a whole decimal integer.";return false;}
            const int64_t signedValue=std::stoll(value,nullptr,10);
            if(descriptor.bits<64) {
                int64_t limit=int64_t(1)<<(descriptor.bits-1);
                if(signedValue < -limit || signedValue>=limit) {error="Integer is outside the signed data type range.";return false;}
            }
            raw=static_cast<uint64_t>(signedValue)&mask(descriptor.bits);
        } else {
            auto digits=value;if(digits[0]=='+')digits.erase(0,1);
            if(!digitsOnly(digits,10)) {error="Enter a nonnegative whole decimal integer.";return false;}
            raw=std::stoull(digits,nullptr,10);
            if(raw>mask(descriptor.bits)) {error="Integer is outside the unsigned data type range.";return false;}
        }
        bytes=littleBytes(raw,descriptor.bits);error.clear();return true;
    } catch(const std::exception&) {error="Integer is outside the data type range.";return false;}
}

std::string decodeValue(const std::vector<uint8_t>& bytes, const std::string& type, bool hex) {
    const auto descriptor=describe(type);
    if(descriptor.string)return std::string(bytes.begin(),bytes.end());
    if(!descriptor.bits||bytes.size()<(descriptor.bits+7)/8)return "—";
    auto raw=readBits(bytes,0,descriptor.bits);
    if(hex) {std::ostringstream stream;stream<<"0x"<<std::uppercase<<std::hex<<std::setw((descriptor.bits+3)/4)<<std::setfill('0')<<raw;return stream.str();}
    if(descriptor.floating) {
        double value=0;
        if(descriptor.bits==32) {uint32_t bits=static_cast<uint32_t>(raw);float result=0;std::memcpy(&result,&bits,sizeof result);value=result;}
        else std::memcpy(&value,&raw,sizeof value);
        if(!std::isfinite(value))return "Invalid REAL (NaN / Infinity)";
        std::ostringstream stream;stream.imbue(std::locale::classic());stream<<std::setprecision(descriptor.bits==32?std::numeric_limits<float>::max_digits10:std::numeric_limits<double>::max_digits10)<<value;return stream.str();
    }
    if(type=="BOOL")return raw?"TRUE":"FALSE";
    if(descriptor.sign&&(raw&(uint64_t(1)<<(descriptor.bits-1)))) {
        const auto magnitude=((~raw)&mask(descriptor.bits))+1;
        if(magnitude==(uint64_t(1)<<63))return "-9223372036854775808";
        return "-"+std::to_string(magnitude);
    }
    return std::to_string(raw);
}
} // namespace niceecatm

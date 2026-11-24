// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "model/types.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace niceecatm {

// Captures only explicitly monitored fields on every valid I/O cycle. The
// fixed rings keep sampling allocation-free, while history() creates the
// chronological snapshot copy outside the real-time cycle path.
class TrendAccumulator {
    static constexpr size_t Capacity=2000;
    struct Key {
        int slave=0;
        bool input=false;
        unsigned bit=0,width=0;
        bool operator<(const Key& other) const {
            if(slave!=other.slave)return slave<other.slave;
            if(input!=other.input)return input<other.input;
            if(bit!=other.bit)return bit<other.bit;
            return width<other.width;
        }
        bool operator==(const Key& other) const {
            return slave==other.slave&&input==other.input&&bit==other.bit&&width==other.width;
        }
    };
    struct Field { Key key; std::string type; };
    struct Capture {
        Field field;
        std::vector<float> samples;
        size_t head=0,count=0;
    };
    std::vector<Capture> captures_;
    int cycleUs_=10000;
    bool cycleKnown_=false;

    static std::string publicKey(const Key& key) {
        return std::to_string(key.slave)+(key.input?"/in/":"/out/")+std::to_string(key.bit)+"/"+std::to_string(key.width);
    }
    static float value(const Slave& slave,bool input,unsigned bit,unsigned width,const std::string& type) {
        const auto& image=input?slave.inputs:slave.outputs;
        if(width==0||width>64||uint64_t(bit)+width>uint64_t(image.size())*8)return 0;
        const uint64_t raw=readBits(image,bit,width);
        if(type=="REAL32"&&width==32) {
            const uint32_t bits=static_cast<uint32_t>(raw);float result=0;
            std::memcpy(&result,&bits,sizeof result);return std::isfinite(result)?result:0;
        }
        if(type=="REAL64"&&width==64) {
            double result=0;std::memcpy(&result,&raw,sizeof result);
            return std::isfinite(result)?static_cast<float>(result):0;
        }
        if(type.size()>3&&type.rfind("INT",0)==0) {
            if(width==64)return static_cast<float>(static_cast<int64_t>(raw));
            const uint64_t sign=uint64_t(1)<<(width-1);
            const uint64_t mask=(uint64_t(1)<<width)-1;
            return static_cast<float>((raw&sign)?static_cast<int64_t>(raw|~mask):static_cast<int64_t>(raw));
        }
        return static_cast<float>(raw);
    }
    static bool supportedType(const PdoEntry& entry) {
        if(entry.padding||entry.bitLength==0||entry.bitLength>64||entry.type.empty())return false;
        if(entry.type=="BOOL")return entry.bitLength==1;
        if(entry.type=="REAL32")return entry.bitLength==32;
        if(entry.type=="REAL64")return entry.bitLength==64;
        const bool integer=entry.type.compare(0,4,"UINT")==0||entry.type.compare(0,3,"INT")==0;
        if(!integer)return false;
        const auto at=entry.type.find_first_of("0123456789");
        return at!=std::string::npos&&std::strtoul(entry.type.c_str()+at,nullptr,10)==entry.bitLength;
    }
    static void appendRaw(std::vector<Field>& fields,int slave,bool input,unsigned bit,unsigned count) {
        const unsigned end=bit+count;
        while(bit<end) {
            const unsigned width=std::min(8u-bit%8,end-bit);
            fields.push_back({{slave,input,bit,width},width==1?"BOOL":"UINT"+std::to_string(width)});
            bit+=width;
        }
    }
    static std::vector<Field> buildDirection(const Slave& slave,bool input) {
        const auto& image=input?slave.inputs:slave.outputs;
        const unsigned bits=std::min<unsigned>(input?slave.inputBits:slave.outputBits,unsigned(image.size()*8));
        if(!bits)return {};
        std::vector<const PdoEntry*> entries;
        for(const auto& entry:slave.pdoEntries)if(entry.input==input)entries.push_back(&entry);
        std::sort(entries.begin(),entries.end(),[](const auto* a,const auto* b){return a->bitOffset<b->bitOffset;});
        if(entries.empty()) {std::vector<Field> fields;appendRaw(fields,slave.position,input,0,bits);return fields;}
        const bool layoutKnown=input?slave.inputLayoutKnown:slave.outputLayoutKnown;
        unsigned end=0;bool valid=layoutKnown;
        for(const auto* entry:entries) {
            if(!entry->bitLength||entry->bitOffset<end||uint64_t(entry->bitOffset)+entry->bitLength>bits){valid=false;break;}
            end=entry->bitOffset+entry->bitLength;
        }
        std::vector<Field> fields;
        if(!valid) {appendRaw(fields,slave.position,input,0,bits);return fields;}
        unsigned bit=0;
        for(const auto* entry:entries) {
            if(bit<entry->bitOffset)appendRaw(fields,slave.position,input,bit,entry->bitOffset-bit);
            if(supportedType(*entry))fields.push_back({{slave.position,input,entry->bitOffset,entry->bitLength},entry->type});
            else appendRaw(fields,slave.position,input,entry->bitOffset,entry->bitLength);
            bit=entry->bitOffset+entry->bitLength;
        }
        if(bit<bits)appendRaw(fields,slave.position,input,bit,bits-bit);
        return fields;
    }
    static std::vector<Field> buildFields(const std::vector<Slave>& slaves) {
        std::vector<Field> fields;
        for(const auto& slave:slaves) {
            auto input=buildDirection(slave,true);
            fields.insert(fields.end(),std::make_move_iterator(input.begin()),std::make_move_iterator(input.end()));
            auto output=buildDirection(slave,false);
            fields.insert(fields.end(),std::make_move_iterator(output.begin()),std::make_move_iterator(output.end()));
        }
        return fields;
    }
    void resetSamples() {for(auto& capture:captures_) {capture.head=0;capture.count=0;}}
public:
    void clear() {captures_.clear();cycleKnown_=false;cycleUs_=10000;}
    // Stopping I/O keeps the last captured history visible. A new cycle or a
    // mapping change is responsible for clearing samples.
    void pause() {}
    void setCycle(int cycleUs) {
        if(cycleUs<=0)return;
        if(!cycleKnown_||cycleUs_!=cycleUs) {cycleUs_=cycleUs;cycleKnown_=true;resetSamples();}
    }
    void setSelection(const std::vector<std::string>& keys,const std::vector<Slave>& slaves) {
        const auto available=buildFields(slaves);
        std::map<std::string,Field> byKey;
        for(const auto& field:available)byKey.emplace(publicKey(field.key),field);
        std::vector<Capture> next;std::set<std::string> seen;
        for(const auto& key:keys) {
            if(!seen.insert(key).second)continue;
            const auto found=byKey.find(key);if(found==byKey.end())continue;
            Capture capture;capture.field=found->second;capture.samples.resize(Capacity);
            const auto old=std::find_if(captures_.begin(),captures_.end(),[&](const Capture& item){return item.field.key==capture.field.key;});
            if(old!=captures_.end()) {capture.samples=std::move(old->samples);capture.head=old->head;capture.count=old->count;}
            next.push_back(std::move(capture));
        }
        captures_=std::move(next);
    }
    void observe(const std::vector<Slave>& slaves,int cycleUs,bool valid=true) {
        if(!valid||captures_.empty())return;
        setCycle(cycleUs);
        for(auto& capture:captures_) {
            const auto found=std::find_if(slaves.begin(),slaves.end(),[&](const Slave& slave){return slave.position==capture.field.key.slave;});
            if(found==slaves.end())continue;
            const auto sample=value(*found,capture.field.key.input,capture.field.key.bit,capture.field.key.width,capture.field.type);
            capture.samples[capture.head]=sample;capture.head=(capture.head+1)%Capacity;capture.count=std::min(Capacity,capture.count+1);
        }
    }
    TrendHistory history() const {
        TrendHistory result;
        for(const auto& capture:captures_)if(capture.count) {
            auto& output=result[publicKey(capture.field.key)];output.resize(capture.count);
            const size_t start=(capture.head+Capacity-capture.count)%Capacity;
            for(size_t n=0;n<capture.count;++n)output[n]=capture.samples[(start+n)%Capacity];
        }
        return result;
    }
};
} // namespace niceecatm

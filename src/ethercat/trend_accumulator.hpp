// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "model/types.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace niceecatm {

// Keeps one fixed-size display history while the I/O owner observes every
// valid process-data exchange. The raw process image is never retained.
class TrendAccumulator {
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
    };
    struct Active {
        double minimum=0,maximum=0,latest=0;
        bool initialized=false;
    };
    struct Field {
        Key key;
        std::string type;
    };
    std::map<Key,Active> active_;
    std::map<Key,std::vector<Field>> layouts_;
    TrendHistory history_;
    int cycleUs_=10000;
    int64_t elapsedUs_=0;

    static std::string publicKey(const Key& key) {
        return std::to_string(key.slave)+(key.input?"/in/":"/out/")+std::to_string(key.bit)+"/"+std::to_string(key.width);
    }
    static double value(const Slave& slave,bool input,unsigned bit,unsigned width,const std::string& type) {
        const auto& image=input?slave.inputs:slave.outputs;
        if(width==0||width>64||uint64_t(bit)+width>uint64_t(image.size())*8)return 0;
        const uint64_t raw=readBits(image,bit,width);
        if(type=="REAL32"&&width==32) {
            const uint32_t bits=static_cast<uint32_t>(raw);float result=0;
            std::memcpy(&result,&bits,sizeof result);return std::isfinite(result)?double(result):0;
        }
        if(type=="REAL64"&&width==64) {
            double result=0;std::memcpy(&result,&raw,sizeof result);return std::isfinite(result)?result:0;
        }
        if(type.size()>3&&type.rfind("INT",0)==0) {
            if(width==64)return double(static_cast<int64_t>(raw));
            const uint64_t sign=uint64_t(1)<<(width-1);
            const uint64_t mask=(uint64_t(1)<<width)-1;
            return double((raw&sign)?static_cast<int64_t>(raw|~mask):static_cast<int64_t>(raw));
        }
        return double(raw);
    }
    static bool supportedType(const PdoEntry& entry) {
        if(entry.padding||entry.bitLength==0||entry.bitLength>64||entry.type.empty())return false;
        if(entry.type=="BOOL")return entry.bitLength==1;
        if(entry.type=="REAL32")return entry.bitLength==32;
        if(entry.type=="REAL64")return entry.bitLength==64;
        const auto prefix=entry.type.compare(0,4,"UINT")==0||entry.type.compare(0,3,"INT")==0;
        if(!prefix)return false;
        const auto at=entry.type.find_first_of("0123456789");
        return at!=std::string::npos&&std::strtoul(entry.type.c_str()+at,nullptr,10)==entry.bitLength;
    }
    void observeValue(const Slave& slave,bool input,unsigned bit,unsigned width,const std::string& type) {
        const Key key{slave.position,input,bit,width};
        const double sample=value(slave,input,bit,width,type);auto& active=active_[key];
        if(!active.initialized) {active.minimum=active.maximum=active.latest=sample;active.initialized=true;}
        else {active.minimum=std::min(active.minimum,sample);active.maximum=std::max(active.maximum,sample);active.latest=sample;}
    }
    static void appendRaw(std::vector<Field>& fields,int slave,bool input,unsigned bit,unsigned count) {
        const unsigned end=bit+count;
        while(bit<end) {
            const unsigned width=std::min(8u-bit%8,end-bit);
            fields.push_back({{slave,input,bit,width},width==1?"BOOL":"UINT"+std::to_string(width)});
            bit+=width;
        }
    }
    static std::vector<Field> buildLayout(const Slave& slave,bool input) {
        const auto& image=input?slave.inputs:slave.outputs;
        const unsigned bits=std::min<unsigned>(input?slave.inputBits:slave.outputBits,unsigned(image.size()*8));
        if(!bits)return {};
        std::vector<const PdoEntry*> entries;
        for(const auto& entry:slave.pdoEntries)if(entry.input==input)entries.push_back(&entry);
        std::sort(entries.begin(),entries.end(),[](const auto* a,const auto* b){return a->bitOffset<b->bitOffset;});
        const bool layoutKnown=input?slave.inputLayoutKnown:slave.outputLayoutKnown;
        if(entries.empty()) {
            std::vector<Field> fields;
            appendRaw(fields,slave.position,input,0,bits);
            return fields;
        }
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
    void rebuildLayouts(const std::vector<Slave>& slaves) {
        layouts_.clear();
        for(const auto& slave:slaves) {
            const Key inputKey{slave.position,true,0,0};
            const Key outputKey{slave.position,false,0,0};
            layouts_[inputKey]=buildLayout(slave,true);
            layouts_[outputKey]=buildLayout(slave,false);
        }
    }
    void observeDirection(const Slave& slave,bool input) {
        const Key layoutKey{slave.position,input,0,0};
        const auto found=layouts_.find(layoutKey);
        if(found==layouts_.end())return;
        for(const auto& field:found->second)
            observeValue(slave,input,field.key.bit,field.key.width,field.type);
    }
    void commit() {
        for(const auto& [key,active]:active_)if(active.initialized) {
            auto& points=history_[publicKey(key)];
            points.push_back({active.minimum,active.maximum,active.latest});
            if(points.size()>200)points.erase(points.begin(),points.begin()+static_cast<std::ptrdiff_t>(points.size()-200));
        }
        for(auto& [key,active]:active_)active=Active{};
    }
public:
    void clear() {active_.clear();history_.clear();layouts_.clear();elapsedUs_=0;}
    void pause() {
        for(auto& [key,active]:active_)active=Active{};
        elapsedUs_=0;
    }
    void setCycle(int cycleUs) {
        if(cycleUs<=0||cycleUs==cycleUs_)return;
        cycleUs_=cycleUs;elapsedUs_=0;
        for(auto& [key,active]:active_)active=Active{};
    }
    void observe(const std::vector<Slave>& slaves,int cycleUs,bool valid=true) {
        if(!valid)return;
        setCycle(cycleUs);
        if(layouts_.empty())rebuildLayouts(slaves);
        for(const auto& slave:slaves) {
            observeDirection(slave,true);
            observeDirection(slave,false);
        }
        elapsedUs_+=cycleUs_;
        if(elapsedUs_>=100000) {commit();elapsedUs_%=100000;}
    }
    const TrendHistory& history() const {return history_;}
};
} // namespace niceecatm

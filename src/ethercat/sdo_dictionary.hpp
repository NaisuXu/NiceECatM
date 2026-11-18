// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "model/types.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
extern "C" {
#include "soem/soem.h"
}
#ifdef snprintf
#undef snprintf
#endif
#ifdef vsnprintf
#undef vsnprintf
#endif

namespace niceecatm::sdo_discovery {
struct Limits {
    std::chrono::milliseconds budget{6000};
    unsigned maxRequests=4096, maxObjects=1024;
};
// The caller owns the SOEM context exclusively and verifies PRE-OP before
// entering. These requests fetch descriptions only, never SDO current values.
inline std::shared_ptr<const SdoDictionary> read(ecx_contextt* context,uint16_t slave,Limits limits={}) {
    using Clock=std::chrono::steady_clock;
    auto result=std::make_shared<SdoDictionary>();result->complete=true;
    // Optional discovery must not consume older transport errors or leave
    // expected sparse-subindex Abort responses behind for the next operation.
    struct ErrorScope {
        ecx_contextt* context;
        std::vector<ec_errort> saved;
        explicit ErrorScope(ecx_contextt* value):context(value) {
            ec_errort error{};while(ecx_poperror(context,&error))saved.push_back(error);
        }
        ~ErrorScope() {
            ec_errort error{};while(ecx_poperror(context,&error)){}
            for(const auto& old:saved)ecx_pusherror(context,&old);
        }
    } scope(context);
    const auto deadline=Clock::now()+limits.budget;
    unsigned requests=0,entries=0,consecutiveFailures=0;
    std::string issue;
    const auto incomplete=[&](const std::string& reason) {
        result->complete=false;if(issue.empty())issue=reason;
    };
    const auto available=[&] {
        // Upstream single-entry calls have fixed mailbox timeouts. Reserve a
        // full operation before starting one instead of checking only after.
        const auto reserve=std::chrono::microseconds(EC_TIMEOUTTXM+EC_TIMEOUTRXM+10000);
        if(requests>=limits.maxRequests||deadline-Clock::now()<reserve) {
            incomplete("Discovery budget reached; refresh to retry.");return false;
        }
        ++requests;return true;
    };
    const auto failure=[&](bool& missing) {
        ec_errort error{};bool found=false;missing=true;std::string text;
        while(ecx_poperror(context,&error)) {
            found=true;
            if(error.Etype!=EC_ERR_TYPE_SDOINFO_ERROR||uint32_t(error.AbortCode)!=0x06090011u)missing=false;
            if(text.empty()) {
                std::ostringstream out;
                if(error.Etype==EC_ERR_TYPE_SDOINFO_ERROR||error.Etype==EC_ERR_TYPE_SDO_ERROR)
                    out<<"SDO Information Abort 0x"<<std::hex<<std::uppercase<<std::setw(8)<<std::setfill('0')<<uint32_t(error.AbortCode);
                else out<<"SDO Information response/transport error (type "<<error.Etype<<", code "<<error.ErrorCode<<").";
                text=out.str();
            }
        }
        missing=found&&missing;
        return text.empty()?std::string("SDO Information request failed or timed out."):text;
    };
    auto objects=std::make_unique<ec_ODlistt>();auto descriptions=std::make_unique<ec_OElistt>();
    if(available()) {
        const int received=ecx_readODlist(context,slave,objects.get());
        if(received<=0) {bool missing=false;incomplete(failure(missing));}
        if(objects->Entries>EC_MAXODLIST) {objects->Entries=EC_MAXODLIST;incomplete("Object list exceeds the master capacity.");}
        const unsigned count=std::min<unsigned>(objects->Entries,limits.maxObjects);
        if(count<objects->Entries)incomplete("Object count limit reached.");
        std::set<uint16_t> seen;
        for(unsigned item=0;item<count;++item) {
            const auto index=objects->Index[item];
            if(!index||!seen.insert(index).second) {incomplete("Invalid or duplicate object index.");continue;}
            SdoObject object;object.index=index;object.online=true;
            if(!available())break;
            if(ecx_readODdescription(context,uint16_t(item),objects.get())<=0) {
                bool missing=false;incomplete(failure(missing));result->objects.push_back(std::move(object));
                if(++consecutiveFailures>=3)break;
                continue;
            }
            consecutiveFailures=0;object.objectCode=objects->ObjectCode[item];object.maxSubindex=objects->MaxSub[item];
            if(objects->Name[item][0])object.names.push_back({0,objects->Name[item]});
            if(object.objectCode!=7&&object.objectCode!=8&&object.objectCode!=9) {
                incomplete("Unsupported object description code.");result->objects.push_back(std::move(object));continue;
            }
            const unsigned maxSub=object.objectCode==7?0:object.maxSubindex;
            for(unsigned sub=0;sub<=maxSub;++sub) {
                if(!available())break;
                *descriptions={};
                if(ecx_readOEsingle(context,uint16_t(item),uint8_t(sub),objects.get(),descriptions.get())<=0||!descriptions->Entries) {
                    bool missing=false;const auto reason=failure(missing);
                    // A record can legitimately have gaps below its maximum
                    // subindex; only explicit "subindex does not exist" is a
                    // gap. A timeout or malformed response stays incomplete.
                    if(missing) {consecutiveFailures=0;continue;}
                    incomplete(reason);if(++consecutiveFailures>=3)break;
                    continue;
                }
                consecutiveFailures=0;
                SdoEntry entry;entry.subindex=uint8_t(sub);entry.online=true;
                entry.dataType=descriptions->DataType[sub];entry.bitLength=descriptions->BitLength[sub];
                entry.type=entry.dataType==0x0009?"STRING":pdoType(entry.dataType,entry.bitLength);entry.access=descriptions->ObjAccess[sub];
                entry.accessKnown=true;
                if(descriptions->Name[sub][0])entry.names.push_back({0,descriptions->Name[sub]});
                object.entries.push_back(std::move(entry));++entries;
            }
            if(object.entries.empty())incomplete("Object has no available entry descriptions.");
            result->objects.push_back(std::move(object));
            if(consecutiveFailures>=3||requests>=limits.maxRequests||Clock::now()>=deadline) {
                if(item+1<count)incomplete("Discovery stopped before all objects were described.");
                break;
            }
        }
        if(result->objects.size()<seen.size())incomplete("Discovery budget reached; refresh to retry.");
    }
    std::sort(result->objects.begin(),result->objects.end(),[](const auto& a,const auto& b){return a.index<b.index;});
    result->status=std::string("Online SDO dictionary ")+(result->complete?"complete: ":"incomplete: ")+std::to_string(result->objects.size())+" objects, "+std::to_string(entries)+" entries.";
    if(!issue.empty())result->status+=" "+issue;
    return result;
}
}

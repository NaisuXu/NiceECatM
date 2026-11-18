// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/sdo_dictionary.hpp"
#include <deque>
#include <iostream>
#include <stdexcept>

namespace {
int checks=0,scenario=0,requests=0;
std::deque<ec_errort> errors;
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
void missing() {ec_errort error{};error.Etype=EC_ERR_TYPE_SDOINFO_ERROR;error.AbortCode=0x06090011;errors.push_back(error);}
}
extern "C" {
boolean ecx_poperror(ecx_contextt*,ec_errort* error) {if(errors.empty())return FALSE;*error=errors.front();errors.pop_front();return TRUE;}
void ecx_pusherror(ecx_contextt*,const ec_errort* error) {errors.push_back(*error);}
int ecx_readODlist(ecx_contextt*,uint16 slave,ec_ODlistt* objects) {
    ++requests;objects->Slave=slave;
    if(scenario==1)return 0;
    if(scenario==2){objects->Entries=0;return 1;}
    objects->Entries=2;objects->Index[0]=0x1008;objects->Index[1]=scenario==3?0x1008:0x8000;return 1;
}
int ecx_readODdescription(ecx_contextt*,uint16 item,ec_ODlistt* objects) {
    ++requests;if(scenario==4&&item==1)return 0;
    objects->ObjectCode[item]=item==0?7:9;objects->MaxSub[item]=item==0?0:3;
    std::snprintf(objects->Name[item],sizeof objects->Name[item],"Object %u",unsigned(item));return 1;
}
int ecx_readOEsingle(ecx_contextt*,uint16 item,uint8 sub,ec_ODlistt*,ec_OElistt* entries) {
    ++requests;
    if(item==1&&sub==2){missing();return 0;}
    if(scenario==5&&item==1&&sub==3)return 0;
    if((scenario==6||scenario==7)&&item==1&&sub==3) {
        ec_errort error{};error.Etype=scenario==6?EC_ERR_TYPE_PACKET_ERROR:EC_ERR_TYPE_SDOINFO_ERROR;
        if(scenario==6)error.ErrorCode=1;else error.AbortCode=0x06010001;
        errors.push_back(error);return 0;
    }
    entries->Entries=1;entries->DataType[sub]=item==0?0x9:6;
    entries->BitLength[sub]=item==0?104:16;entries->ObjAccess[sub]=item==0?7:63;
    std::snprintf(entries->Name[sub],sizeof entries->Name[sub],"Entry %u",unsigned(sub));return 1;
}
}

int main() {
    using namespace niceecatm;
    try {
        auto context=std::make_unique<ecx_contextt>();
        ec_errort original{};original.Etype=EC_ERR_TYPE_PACKET_ERROR;original.AbortCode=123;errors.push_back(original);
        auto dictionary=sdo_discovery::read(context.get(),1);
        check(dictionary->complete&&dictionary->objects.size()==2,"Complete dictionary metadata");
        check(dictionary->objects[0].entries[0].type=="STRING","VISIBLE_STRING uses text decoding rather than raw PDO fallback");
        check(dictionary->objects[1].entries.size()==3&&dictionary->objects[1].entries.back().subindex==3,"Sparse record skips only explicit missing subindices");
        check(dictionary->objects[1].entries.back().accessKnown&&dictionary->objects[1].entries.back().access==63,"Online access rights retained");
        check(errors.size()==1&&errors.front().AbortCode==123,"Previous errors preserved and sparse Abort drained");errors.clear();
        const auto first=dictionary;
        scenario=1;dictionary=sdo_discovery::read(context.get(),1);
        check(!dictionary->complete&&dictionary->objects.empty(),"Failed list is incomplete and empty");
        check(first->complete&&first->objects.size()==2,"Published descriptions remain immutable across refresh");
        scenario=2;dictionary=sdo_discovery::read(context.get(),1);
        check(dictionary->complete&&dictionary->objects.empty(),"A valid empty object list is complete");
        scenario=3;dictionary=sdo_discovery::read(context.get(),1);
        check(!dictionary->complete&&dictionary->objects.size()==1,"Duplicate object indices are not silently accepted");
        scenario=4;dictionary=sdo_discovery::read(context.get(),1);
        check(!dictionary->complete&&dictionary->objects.size()==2&&!dictionary->objects[0].entries.empty(),"Partial object failure retains valid earlier metadata");
        scenario=5;dictionary=sdo_discovery::read(context.get(),1);
        check(!dictionary->complete&&dictionary->objects[1].entries.size()==2,"A timeout is not treated as a sparse hole");
        scenario=6;dictionary=sdo_discovery::read(context.get(),1);
        check(!dictionary->complete&&dictionary->status.find("response/transport error")!=std::string::npos&&dictionary->status.find("Abort")==std::string::npos,
              "A malformed response is reported as a response error, never a device Abort");
        scenario=7;dictionary=sdo_discovery::read(context.get(),1);
        check(!dictionary->complete&&dictionary->status.find("Abort 0x06010001")!=std::string::npos,"A genuine device Abort retains its original hexadecimal code");
        scenario=0;requests=0;
        sdo_discovery::Limits limits;limits.maxRequests=3;
        dictionary=sdo_discovery::read(context.get(),1,limits);
        check(!dictionary->complete&&dictionary->objects.size()==1&&requests==3,"Exact request exhaustion before remaining objects is incomplete");
        limits.maxRequests=2;dictionary=sdo_discovery::read(context.get(),1,limits);
        check(!dictionary->complete&&dictionary->objects[0].entries.empty(),"Exhaustion within one object is incomplete");
        limits={};limits.maxObjects=0;requests=0;dictionary=sdo_discovery::read(context.get(),1,limits);
        check(!dictionary->complete&&dictionary->objects.empty()&&requests==1,"Zero object capacity never reports an empty complete dictionary");
        limits={};limits.budget=std::chrono::milliseconds(0);requests=0;
        dictionary=sdo_discovery::read(context.get(),1,limits);
        check(!dictionary->complete&&requests==0,"Zero time budget performs no mailbox operation");
        limits={};limits.maxRequests=0;dictionary=sdo_discovery::read(context.get(),1,limits);
        check(!dictionary->complete&&requests==0,"Zero request budget performs no mailbox operation");
        std::cout<<checks<<" dictionary enumeration checks passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

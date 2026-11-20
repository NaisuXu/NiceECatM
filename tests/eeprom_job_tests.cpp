// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/eeprom_job.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using niceecatm::eeprom::Session;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Esc {
    std::vector<uint8_t> memory=std::vector<uint8_t>(256,0);
    uint8_t owner=1,initialOwner=1;
    uint16_t state=0,staged=0;
    size_t address=0,lastRead=0;
    int configReads=1,statusReads=1,dataReads=1,controlWrites=1;
    bool nack=false,lostWriteReply=false,restoreFails=false;
    unsigned restoreFailuresRemaining=0,restoreAttempts=0;
    unsigned commands=0,writes=0;
    niceecatm::eeprom::Wire wire(){return {
        [this](uint16_t reg,size_t size,void* data){
            auto* bytes=static_cast<uint8_t*>(data);
            if(reg==0x500){*bytes=owner;return configReads;}
            if(reg==0x502){bytes[0]=uint8_t(state);bytes[1]=uint8_t(state>>8);return statusReads;}
            if(reg==0x508){require(address+size<=memory.size(),"read escaped memory capacity");lastRead=address;std::copy_n(memory.begin()+address,size,bytes);return dataReads;}
            throw std::runtime_error("unexpected read");
        },
        [this](uint16_t reg,size_t size,const void* data){
            const auto* bytes=static_cast<const uint8_t*>(data);
            if(reg==0x500){
                if(bytes[0]==initialOwner){++restoreAttempts;if(restoreFails)return 0;if(restoreFailuresRemaining){--restoreFailuresRemaining;return 0;}}
                owner=bytes[0];return 1;
            }
            if(reg==0x508){staged=uint16_t(bytes[0])|uint16_t(bytes[1])<<8;return 1;}
            if(reg==0x502){
                if(controlWrites!=1)return controlWrites;
                if(size==2){state&=0x0840;return 1;}
                require(size==6,"invalid EEPROM control size");
                ++commands;address=2*(size_t(bytes[2])|(size_t(bytes[3])<<8));
                state=(state&0x0840)|(nack?0x2000:0);
                if(bytes[0]==1&&bytes[1]==2&&!nack){
                    require(address+2<=memory.size(),"write escaped memory capacity");
                    memory[address]=uint8_t(staged);memory[address+1]=uint8_t(staged>>8);++writes;
                    if(lostWriteReply)return 0;
                }
                return 1;
            }
            throw std::runtime_error("unexpected write");
        }
    };}
};
void testSuccessfulTransfer(){
    Esc esc;esc.owner=esc.initialOwner=3;std::string error;
    {
        Session session(esc.wire());require(session.begin(error),error.c_str());
        require(esc.owner==0,"master ownership was not acquired");
        require(session.writeWord(254,0x1234,error),error.c_str());
        require(esc.memory[254]==0x34&&esc.memory[255]==0x12,"word wire byte order");
        std::vector<uint8_t> bytes;
        require(session.read(254,2,256,bytes,error),error.c_str());
        require(bytes==std::vector<uint8_t>({0x34,0x12}),"bounded last-word read");
        require(esc.lastRead==252,"32-bit read did not stay within selected capacity");
        require(session.finish(error),error.c_str());require(esc.owner==3,"exact previous owner was not restored");
    }
    require(esc.writes==1,"a word was automatically written more than once");
}
void testRead64BoundaryAndZeroData(){
    Esc esc;esc.state=0x40;std::string error;Session session(esc.wire());require(session.begin(error),error.c_str());
    std::vector<uint8_t> bytes{0xFF};require(session.read(254,2,256,bytes,error),error.c_str());
    require(bytes==std::vector<uint8_t>({0,0}),"valid zero EEPROM bytes were not returned");
    require(esc.lastRead==248,"64-bit read overran the final capacity boundary");
    require(session.finish(error),error.c_str());
}
void testMissingDataWkc(){
    Esc esc;esc.dataReads=0;std::string error;Session session(esc.wire());require(session.begin(error),error.c_str());
    std::vector<uint8_t> bytes{7};require(!session.read(0,4,256,bytes,error),"zero-WKC data read succeeded");
    require(bytes==std::vector<uint8_t>({7}),"failed read was returned as zero-filled data");
    require(session.finish(error)&&esc.owner==1,"read error did not restore ownership");
}
void testMissingStatusWkc(){
    Esc esc;esc.statusReads=0;std::string error;{Session session(esc.wire());require(!session.begin(error),"missing status WKC looked ready");}
    require(esc.owner==1,"begin failure did not restore ownership");require(esc.commands==0,"command issued without a valid ready status");
}
void testNackAndLostWrite(){
    for(bool lostReply:{false,true}){
        Esc esc;esc.nack=!lostReply;esc.lostWriteReply=lostReply;std::string error;
        Session session(esc.wire());require(session.begin(error),error.c_str());
        require(!session.writeWord(0,0xA5F0,error),"failed write reported success");
        require(session.writeAttempted(),"uncertain write did not mark persistent changes possible");
        require(lostReply?esc.commands==1:esc.commands==3,"NACK/reply handling issued an unexpected number of commands");
        require(session.finish(error)&&esc.owner==1,"failed write did not restore ownership");
        require(lostReply?esc.memory[0]==0xF0:esc.writes==0,"lost reply/NACK simulation is invalid");
    }
}
void testArbitrationAndBounds(){
    Esc esc;std::string error;Session session(esc.wire());std::vector<uint8_t> bytes;
    require(!session.read(0,4,256,bytes,error),"unowned EEPROM was read");
    require(session.begin(error),error.c_str());
    require(!session.read(255,2,256,bytes,error),"out-of-capacity read accepted");
    require(!session.read(0,4,Session::MaxCapacity+2,bytes,error),"16-bit word address overflow accepted");
    require(!session.writeWord(1,0,error),"odd word write accepted");
    require(!session.writeWord(Session::MaxCapacity,0,error),"word address overflow accepted");
    require(esc.commands==0,"invalid range issued an EEPROM command");
    esc.restoreFails=true;require(!session.finish(error),"failed ownership restoration reported success");
}
void testBusyTimeoutAndBlankChecksum(){
    Esc busy;busy.state=0x8000;std::string error;
    {Session session(busy.wire());require(!session.begin(error)&&error.find("timeout")!=std::string::npos,"busy timeout was not bounded");}
    require(busy.owner==1,"busy timeout did not restore ownership");
    Esc blank;blank.state=0x0800;Session session(blank.wire());require(session.begin(error),error.c_str());
    require(session.writeWord(0,0x0102,error),"blank SII checksum flag prevented programming access");
    require(session.finish(error),error.c_str());
}
void testRestoreRetryRemainsArmed(){
    Esc esc;esc.restoreFails=true;std::string error;Session session(esc.wire());
    require(session.begin(error),error.c_str());
    require(!session.finish(error),"one lost owner-restore reply was reported as success");
    require(esc.owner==0,"failed owner restore unexpectedly changed the simulated owner");
    esc.restoreFails=false;
    require(session.finish(error),"session did not permit a restore retry after a failed reply");
    require(esc.owner==esc.initialOwner&&esc.restoreAttempts==4,"owner restore retry did not recover the original owner");
}
void testCancelBetweenReadCommands(){
    Esc esc;std::string error;Session session(esc.wire(),[&]{return esc.commands>=1;});
    require(session.begin(error),error.c_str());std::vector<uint8_t> bytes{7};
    require(!session.read(0,128,256,bytes,error),"cancelled range read completed");
    require(esc.commands==1&&bytes==std::vector<uint8_t>({7}),"cancellation issued another command or returned a partial read");
    require(session.finish(error)&&esc.owner==1,"cancelled read did not restore ownership");
}
}
int main(){
    try{testSuccessfulTransfer();testRead64BoundaryAndZeroData();testMissingDataWkc();testMissingStatusWkc();testNackAndLostWrite();testArbitrationAndBounds();testBusyTimeoutAndBlankChecksum();testRestoreRetryRemainsArmed();testCancelBetweenReadCommands();std::cout<<"EEPROM register transport checks passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

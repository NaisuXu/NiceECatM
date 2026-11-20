// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/eeprom_job.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <utility>

namespace niceecatm::eeprom {
namespace {
constexpr uint16_t Config=0x0500,Control=0x0502,Data=0x0508;
constexpr uint16_t Busy=0x8000,Errors=0x7800,Nack=0x2000,Read64=0x0040;
// The ESC updates EEPROM status asynchronously after accepting EEPCTL.  The
// SOEM reference waits EC_LOCALDELAY (200 us) for reads and twice that for
// writes before polling status.  Without this settle time a fast host can
// observe the previous idle status and consume stale EEPDAT bytes.
constexpr auto CommandSettle=std::chrono::microseconds(200);
uint16_t word(const uint8_t* data){return uint16_t(data[0])|uint16_t(data[1])<<8;}
}
Session::~Session(){std::string ignored;finish(ignored);}
bool Session::readRegister(uint16_t address,size_t size,void* data,std::string& error){
    // AP datagrams can be lost independently of the ESC transaction. Reads
    // are idempotent, so follow SOEM's bounded retry behavior and never
    // expose a partially filled buffer to the caller.
    std::array<uint8_t,8> candidate{};
    if(size>candidate.size()){error="EEPROM register read is too large.";return false;}
    for(unsigned attempt=0;attempt<3;++attempt){
        if(wire_.read(address,size,candidate.data())==1){
            std::copy_n(candidate.data(),size,static_cast<uint8_t*>(data));
            return true;
        }
        if(attempt<2)std::this_thread::sleep_for(CommandSettle);
    }
    error="EEPROM register read failed (WKC must be 1).";return false;
}
bool Session::writeRegister(uint16_t address,size_t size,const void* data,std::string& error,unsigned retries){
    for(unsigned attempt=0;attempt<retries;++attempt){
        if(wire_.write(address,size,data)==1)return true;
        if(attempt+1<retries)std::this_thread::sleep_for(CommandSettle);
    }
    error="EEPROM register write failed (WKC must be 1).";return false;
}
bool Session::status(uint16_t& value,std::string& error){
    uint8_t bytes[2]{};
    if(!readRegister(Control,2,bytes,error)){error="EEPROM status read failed (WKC must be 1).";return false;}
    value=word(bytes);return true;
}
bool Session::ready(uint16_t& value,std::string& error){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
    do {
        if(!status(value,error))return false;
        if(!(value&Busy))return true;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }while(std::chrono::steady_clock::now()<deadline);
    error="EEPROM busy timeout.";return false;
}
bool Session::prepare(std::string& error){
    uint16_t state=0;if(!ready(state,error))return false;
    if(state&Errors){
        const uint8_t nop[2]{};
        if(!writeRegister(Control,2,nop,error)){error="EEPROM error acknowledgement failed (WKC must be 1).";return false;}
        if(!ready(state,error))return false;
        // A blank SII may retain the configuration checksum error (bit 11).
        // Command/NACK/write failures (bits 12..14) must actually clear.
        if(state&0x7000){error="EEPROM error status did not clear.";return false;}
    }
    return true;
}
bool Session::begin(std::string& error){
    if(saved_){error="EEPROM ownership is already acquired.";return false;}
    if(!readRegister(Config,1,&owner_,error)){error="EEPROM ownership read failed (WKC must be 1).";return false;}
    saved_=true;
    const uint8_t force=2,master=0;
    if(!writeRegister(Config,1,&force,error)||!writeRegister(Config,1,&master,error)){error="Cannot acquire EEPROM ownership (WKC must be 1).";return false;}
    uint8_t actual=0;
    if(!readRegister(Config,1,&actual,error)||actual!=master){error="EEPROM master ownership was not confirmed.";return false;}
    uint16_t state=0;if(!ready(state,error))return false;
    readWidth_=state&Read64?8:4;
    return true;
}
bool Session::finish(std::string& error){
    if(!saved_)return true;
    // Even a lost acquisition response may have changed the hardware owner.
    // Restore the exact byte observed before the first arbitration write.
    std::string ignored;
    const bool wrote=writeRegister(Config,1,&owner_,ignored);
    uint8_t actual=0;const bool restored=wrote&&readRegister(Config,1,&actual,ignored)&&actual==owner_;
    if(!restored){error="EEPROM ownership restoration could not be confirmed.";return false;}
    // Keep the guard armed when restoration failed.  The destructor (or an
    // explicit retry) can then make one more best-effort restore while the
    // SOEM context is still alive; clearing it here could strand the ESC in
    // master-owned mode after a lost AP datagram.
    saved_=false;
    return true;
}
bool Session::command(uint16_t operation,size_t byteOffset,std::string& error){
    if(byteOffset%2||byteOffset/2>0xFFFF){error="EEPROM word address is outside the supported range.";return false;}
    const size_t address=byteOffset/2;
    const uint8_t bytes[6]{uint8_t(operation),uint8_t(operation>>8),uint8_t(address),uint8_t(address>>8),0,0};
    if(operation==0x0201)written_=true; // A lost reply can still mean a write.
    // A lost WKC reply remains single-shot: the ESC may already have accepted
    // the write. writeWord() only retries after an explicit NACK, which means
    // the ESC rejected the command and the same word can safely be staged
    // again after its internal write cycle settles.
    if(wire_.write(Control,6,bytes)!=1){error="EEPROM command failed (WKC must be 1).";return false;}
    std::this_thread::sleep_for(operation==0x0201?CommandSettle*2:CommandSettle);
    uint16_t state=0;if(!ready(state,error))return false;
    if(state&Nack){error="EEPROM command rejected (NACK, status "+std::to_string(state)+").";return false;}
    if(state&0x5000){error="EEPROM command/write error (status "+std::to_string(state)+").";return false;}
    return true;
}
bool Session::read(size_t offset,size_t length,size_t capacity,std::vector<uint8_t>& data,std::string& error){
    if(!saved_){error="EEPROM ownership has not been acquired.";return false;}
    if(capacity<readWidth_||capacity>MaxCapacity||capacity%2||offset>capacity||length>capacity-offset){error="EEPROM read exceeds the bounded capacity.";return false;}
    std::vector<uint8_t> candidate;candidate.reserve(length);
    while(candidate.size()<length){
        if(cancelled_&&cancelled_()){error="EEPROM operation cancelled.";return false;}
        const size_t current=offset+candidate.size();
        const size_t address=std::min(current&~size_t(1),capacity-readWidth_);
        if(!prepare(error))return false;
        bool accepted=false;
        for(unsigned attempt=0;attempt<3;++attempt){
            if(command(0x0100,address,error)){accepted=true;break;}
            if(error.find("NACK")==std::string::npos||attempt==2)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if(!prepare(error))break;
        }
        if(!accepted)return false;
        std::array<uint8_t,8> bytes{};
        if(!readRegister(Data,readWidth_,bytes.data(),error)){error="EEPROM data read failed (WKC must be 1).";return false;}
        const size_t skip=current-address;
        const size_t take=std::min(readWidth_-skip,length-candidate.size());
        candidate.insert(candidate.end(),bytes.begin()+skip,bytes.begin()+skip+take);
    }
    data=std::move(candidate);return true;
}
bool Session::writeWord(size_t offset,uint16_t value,std::string& error){
    if(!saved_){error="EEPROM ownership has not been acquired.";return false;}
    if(offset%2||offset>=MaxCapacity){error="EEPROM word address is outside the supported range.";return false;}
    const uint8_t bytes[2]{uint8_t(value),uint8_t(value>>8)};
    for(unsigned attempt=0;attempt<3;++attempt){
        if(!prepare(error))return false;
        if(!writeRegister(Data,2,bytes,error)){error="EEPROM write data staging failed (WKC must be 1).";return false;}
        if(command(0x0201,offset,error))return true;
        if(error.find("NACK")==std::string::npos)return false;
        if(attempt<2)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
} // namespace niceecatm::eeprom

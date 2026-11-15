// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/pdo_layout.hpp"
#include <iostream>
#include <stdexcept>

using namespace niceecatm;
namespace {
int checks=0;
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
void append(std::vector<uint8_t>& bytes,uint16_t pdo,uint8_t sm,uint16_t index,uint8_t sub,uint8_t width,uint8_t name=1,uint8_t type=5) {
    const uint8_t data[]={uint8_t(pdo),uint8_t(pdo>>8),1,sm,0,0,0,0,
                          uint8_t(index),uint8_t(index>>8),sub,name,type,width,0,0};
    bytes.insert(bytes.end(),std::begin(data),std::end(data));
}
}
int main() {
    try {
        // SOEM 2.0.1 clears segment metadata and returns zero when a group
        // exceeds its frame capacity. Do not accept that as a no-PDO bus.
        check(!pdo_layout::mappingComplete(0,0,16),"Rejected zero-segment mapping cannot enable a heartbeat or cyclic I/O");
        check(pdo_layout::mappingComplete(0,1,16),"A valid mailbox-free zero-byte map retains its AL heartbeat");
        check(pdo_layout::mappingComplete(64,1,16),"A nonempty process image can exchange mapped data");
        check(!pdo_layout::mappingComplete(-1,1,16),"Negative mapper failure is rejected even with segment metadata");
        check(pdo_layout::mappingComplete(64,15,16)&&!pdo_layout::mappingComplete(64,16,16),"Mapped segments must fit the reserved transport frame capacity");
        std::vector<std::string> names;std::string issue;
        check(pdo_layout::strings({2,5,'I','n','p','u','t',6,'O','u','t','p','u','t',0},names),"SII strings with word padding decode");
        check(names.size()==3&&names[1]=="Input"&&names[2]=="Output","SII string indexes are one based");
        check(!pdo_layout::strings({1,5,'b'},names),"Truncated strings rejected");
        check(!pdo_layout::strings({1,3,'a',0,'b'},names),"Embedded null in names rejected");
        names={"","Input","Output"};
        std::array<pdo_layout::SyncManager,8> managers{};
        managers[0]={3,1};managers[1]={3,1};managers[2]={4,2};
        std::vector<uint8_t> bytes;
        // Actual ASIX fixture: RxPDO indexes use the common TxPDO range;
        // direction comes from SII category/SM, never the object index range.
        append(bytes,0x1a01,1,0x3101,2,8,2);append(bytes,0x1a00,0,0x3101,1,8,2);
        std::vector<PdoEntry> entries;
        check(pdo_layout::sii(bytes,false,names,entries,issue),"Fixed output entries decode");
        check(pdo_layout::validate(entries,false,16,managers,issue),"ASIX two-output-SM layout matches mapper");
        check(entries.size()==2&&entries[0].syncManager==0&&entries[0].bitOffset==0&&entries[1].bitOffset==8,"SM order controls offsets");
        check(entries[0].pdoIndex==0x1a00&&!entries[0].input&&entries[0].names[0].text=="Output","Nonconventional PDO indexes retain output direction and names");
        bytes.clear();append(bytes,0x1600,2,0x3001,1,8);append(bytes,0x1601,2,0x3001,2,8);
        check(pdo_layout::sii(bytes,true,names,entries,issue)&&pdo_layout::validate(entries,true,16,managers,issue),"Two PDOs within input SM preserve order");
        check(entries[1].index==0x3001&&entries[1].subindex==2&&entries[1].bitOffset==8,"Second PDO offset follows first");
        check(!pdo_layout::validate(entries,true,15,managers,issue),"Same byte size with different bit size is rejected");
        managers[2].bytes=3;
        check(!pdo_layout::validate(entries,true,16,managers,issue),"Mapped SM byte length must match");managers[2].bytes=2;
        managers[2].type=3;
        check(!pdo_layout::validate(entries,true,16,managers,issue),"Mapped SM direction must match");managers[2].type=4;
        bytes.pop_back();
        check(!pdo_layout::sii(bytes,true,names,entries,issue),"Truncated SII entries rejected atomically");
        bytes.clear();append(bytes,0x1600,0xff,0x6000,1,8);
        check(pdo_layout::sii(bytes,true,names,entries,issue)&&entries.empty(),"Unassigned PDO is excluded");
        bytes.clear();append(bytes,0x1600,9,0x6000,1,8);
        check(!pdo_layout::sii(bytes,true,names,entries,issue),"Out of range active SM rejected");
        bytes.clear();append(bytes,0x1600,2,0x6000,1,0);
        check(!pdo_layout::sii(bytes,true,names,entries,issue),"Zero-width fields rejected");
        bytes.clear();append(bytes,0x1600,2,0,0,3);append(bytes,0x1600,2,0x6000,1,12);append(bytes,0x1600,2,0x6000,2,1,1,1);
        check(pdo_layout::sii(bytes,true,names,entries,issue)&&pdo_layout::validate(entries,true,16,managers,issue),"Padding and nonbyte aligned fields supported");
        check(entries[0].padding&&entries[1].bitOffset==3&&entries[1].bitLength==12&&entries[2].bitOffset==15,"Padding contributes bits without becoming a signal");
        managers={};managers[0]={3,1};managers[1]={3,1};
        bytes.clear();append(bytes,0x1a00,0,0x3101,1,3);append(bytes,0x1a01,1,0x3101,2,5);
        check(pdo_layout::sii(bytes,false,names,entries,issue)&&!pdo_layout::validate(entries,false,8,managers,issue),"Interior fractional-byte SM stays raw");
        managers={};managers[0]={3,1};bytes.clear();append(bytes,0x1a00,0,0x3101,1,3);
        check(pdo_layout::sii(bytes,false,names,entries,issue)&&pdo_layout::validate(entries,false,3,managers,issue),"Single bit-oriented SM supported");
        std::cout<<checks<<" PDO layout checks passed.\n";return 0;
    }catch(const std::exception& error){std::cerr<<"PDO layout check failed: "<<error.what()<<'\n';return 1;}
}

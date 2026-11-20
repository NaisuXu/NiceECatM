// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/foe_job.hpp"
#include "ethercat/bootstrap_session.hpp"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace niceecatm;
using foe::Opcode;
using foe::Packet;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Files {
    std::filesystem::path directory;
    Files(){directory=std::filesystem::temp_directory_path()/std::filesystem::u8path("niceecatm-foe-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directory(directory);}
    ~Files(){std::error_code ignored;std::filesystem::remove_all(directory,ignored);}
    std::string path(const char* name) const {return (directory/name).u8string();}
    void write(const char* name,const std::vector<uint8_t>& data){std::ofstream file(directory/name,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));require(bool(file),"test file write failed");}
    std::vector<uint8_t> read(const char* name){std::ifstream file(directory/name,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
    bool clean() const {for(const auto& file:std::filesystem::directory_iterator(directory))if(file.path().filename().u8string().find(".foe-part-")!=std::string::npos)return false;return true;}
};
struct Mailbox {
    uint64_t time=0;
    std::deque<Packet> incoming;
    std::vector<Packet> sent;
    std::function<void(const Packet&)> onSend;
    bool sendOk=true;
    foe::Wire wire(size_t tx=8,size_t rx=5) {return {tx,rx,
        [this](const Packet& packet,int timeout){require(timeout<=20,"send wait is not bounded");++time;sent.push_back(packet);if(onSend)onSend(packet);return sendOk;},
        [this](Packet& packet,int timeout){require(timeout<=20,"receive wait is not bounded");time+=unsigned(timeout);if(incoming.empty())return 0;packet=std::move(incoming.front());incoming.pop_front();return 1;},
        [this]{return time;},[this](unsigned ms){time+=ms;}};}
    void acknowledge(){onSend=[this](const Packet& packet){if(packet.opcode==Opcode::Write)incoming.push_back({Opcode::Ack,0,{}});if(packet.opcode==Opcode::Data)incoming.push_back({Opcode::Ack,packet.number,{}});};}
};
FoeOptions options(const Files& files,const char* name){FoeOptions value;value.remoteName="f.bin";value.localPath=files.path(name);value.responseTimeoutMs=60;value.noProgressTimeoutMs=1000;value.totalTimeoutMs=3000;value.retries=1;return value;}
void downloadExactAndImmutable(){
    Files files;std::vector<uint8_t> original(16);for(size_t i=0;i<original.size();++i)original[i]=uint8_t(i);files.write("source.bin",original);
    Mailbox mailbox;mailbox.acknowledge();auto acknowledge=mailbox.onSend;
    mailbox.onSend=[&](const Packet& packet){if(packet.opcode==Opcode::Write)files.write("source.bin",std::vector<uint8_t>(16,99));acknowledge(packet);};
    std::vector<FoeProgress> progress;FoeControl control;control.progress=[&](const FoeProgress& value){progress.push_back(value);};
    const auto result=foe::run(mailbox.wire(8,3),2,FoeDirection::Download,options(files,"source.bin"),control);
    require(result.ok&&result.data.empty(),"exact-multiple download failed");std::vector<uint8_t> received;size_t dataBlocks=0;
    for(const auto& packet:mailbox.sent)if(packet.opcode==Opcode::Data){received.insert(received.end(),packet.data.begin(),packet.data.end());++dataBlocks;}
    require(received==original,"download streamed user mutations instead of immutable prepared bytes");require(dataBlocks==3&&mailbox.sent.back().data.empty(),"exact multiple needs trailing zero DATA");
    require(progress.back().stage==FoeStage::Complete&&progress.back().completed==16&&progress.back().slave==2,"completion progress is incorrect");
}
void uploadAsymmetricAndDuplicate(){
    Files files;files.write("received.bin",{99,99});Mailbox mailbox;unsigned ack1=0;
    mailbox.onSend=[&](const Packet& packet){
        if(packet.opcode==Opcode::Read)mailbox.incoming.push_back({Opcode::Data,1,{1,2,3,4,5}});
        if(packet.opcode==Opcode::Ack&&packet.number==1)mailbox.incoming.push_back(++ack1==1?Packet{Opcode::Data,1,{1,2,3,4,5}}:Packet{Opcode::Data,2,{6,7,8}});
    };
    const auto result=foe::run(mailbox.wire(8,5),2,FoeDirection::Upload,options(files,"received.bin"),{});
    require(result.ok,"upload failed with different read/write capacities");require(files.read("received.bin")==std::vector<uint8_t>({1,2,3,4,5,6,7,8}),"upload duplicate was appended or EOF used write capacity");require(files.clean(),"successful upload left partial file");
}
void uploadExactAndEmpty(){
    for(bool empty:{false,true}) {
        Files files;Mailbox mailbox;
        mailbox.onSend=[&](const Packet& packet){if(packet.opcode==Opcode::Read)mailbox.incoming.push_back({Opcode::Data,1,empty?std::vector<uint8_t>{}:std::vector<uint8_t>{1,2,3,4,5}});else if(packet.opcode==Opcode::Ack&&packet.number==1&&!empty)mailbox.incoming.push_back({Opcode::Data,2,{}});};
        require(foe::run(mailbox.wire(),2,FoeDirection::Upload,options(files,"received.bin"),{}).ok,"exact/empty upload failed");require(files.read("received.bin").size()==(empty?0u:5u),"exact/empty upload size wrong");
    }
}
void boundedBusyAndRetries(){
    Files files;files.write("source.bin",{1,2});Mailbox mailbox;unsigned requests=0;
    mailbox.onSend=[&](const Packet& packet){if(packet.opcode==Opcode::Write)mailbox.incoming.push_back({++requests==1?Opcode::Busy:Opcode::Ack,0,{}});else if(packet.opcode==Opcode::Data)mailbox.incoming.push_back({Opcode::Ack,packet.number,{}});};
    require(foe::run(mailbox.wire(),1,FoeDirection::Download,options(files,"source.bin"),{}).ok&&requests==2,"initial BUSY must wait and retry request");
    Mailbox busy;busy.onSend=[&](const Packet&){busy.incoming.push_back({Opcode::Busy,0,{}});};auto settings=options(files,"source.bin");settings.noProgressTimeoutMs=65;
    FoeProgress last;FoeControl control;control.progress=[&](const FoeProgress& value){last=value;};
    const auto failure=foe::run(busy.wire(),1,FoeDirection::Download,settings,control);
    require(!failure.ok&&last.stage==FoeStage::Failed&&busy.time<150&&failure.message.find("no-progress")!=std::string::npos,"BUSY reset no-progress deadline or reported success");
    Mailbox retry;requests=0;retry.onSend=[&](const Packet& packet){if(packet.opcode==Opcode::Write&&++requests>1)retry.incoming.push_back({Opcode::Ack,0,{}});else if(packet.opcode==Opcode::Data)retry.incoming.push_back({Opcode::Ack,packet.number,{}});};
    require(foe::run(retry.wire(),1,FoeDirection::Download,options(files,"source.bin"),{}).ok&&requests==2,"lost response was not retried");
}
void finalAckRequiredAndCancellation(){
    Files files;files.write("source.bin",{1,2});Mailbox mailbox;mailbox.onSend=[&](const Packet& packet){if(packet.opcode==Opcode::Write)mailbox.incoming.push_back({Opcode::Ack,0,{}});};FoeProgress last;FoeControl control;control.progress=[&](const FoeProgress& value){last=value;};
    auto result=foe::run(mailbox.wire(),1,FoeDirection::Download,options(files,"source.bin"),control);
    require(!result.ok&&last.stage==FoeStage::Failed&&last.completed==0,"unacknowledged final DATA was counted as success");
    files.write("received.bin",{9,9,9});Mailbox upload;bool cancelled=false;
    upload.onSend=[&](const Packet& packet){if(packet.opcode==Opcode::Read)upload.incoming.push_back({Opcode::Data,1,{1,2,3,4,5}});};
    control.cancelled=[&]{return cancelled;};control.progress=[&](const FoeProgress& value){last=value;if(value.stage==FoeStage::Receiving)cancelled=true;};
    result=foe::run(upload.wire(),1,FoeDirection::Upload,options(files,"received.bin"),control);
    require(!result.ok&&last.stage==FoeStage::Cancelled&&files.read("received.bin")==std::vector<uint8_t>({9,9,9})&&files.clean(),"cancelled upload changed destination or retained partial file");
}
void errorsAndLimits(){
    Files files;files.write("received.bin",{7,7});Mailbox mailbox;mailbox.onSend=[&](const Packet&){mailbox.incoming.push_back({Opcode::Error,0x8001,{'n','o',' ','f','i','l','e',0}});};FoeProgress last;FoeControl control;control.progress=[&](const FoeProgress& value){last=value;};
    auto result=foe::run(mailbox.wire(8,16),1,FoeDirection::Upload,options(files,"received.bin"),control);
    require(!result.ok&&last.errorCode==0x8001&&result.message.find("no file")!=std::string::npos&&files.read("received.bin")==std::vector<uint8_t>({7,7})&&files.clean(),"remote error detail or destination preservation failed");
    Mailbox limit;limit.onSend=[&](const Packet&){limit.incoming.push_back({Opcode::Data,1,{1,2,3,4,5}});};auto settings=options(files,"received.bin");settings.maxBytes=4;
    require(!foe::run(limit.wire(),1,FoeDirection::Upload,settings,{}).ok&&files.read("received.bin")==std::vector<uint8_t>({7,7})&&files.clean(),"upload exceeded maximum or destroyed destination");
    Mailbox malformed;malformed.onSend=[&](const Packet&){malformed.incoming.push_back({Opcode::Data,2,{1}});};
    require(!foe::run(malformed.wire(),1,FoeDirection::Upload,options(files,"received.bin"),{}).ok,"future DATA packet accepted");
    settings=options(files,"received.bin");settings.remoteName="filename-too-long";
    Mailbox invalid;require(!foe::run(invalid.wire(),1,FoeDirection::Upload,settings,{}).ok&&invalid.sent.empty(),"long filename was silently truncated or sent");
}
void remoteErrorReasons(){
    struct ErrorCase {uint32_t code;const char* reason;};
    const ErrorCase cases[]={
        {0x8001,"File not found; check the device filename."},
        {0x8002,"File access denied."},
        {0x8008,"FoE requires BOOT state."},
        {0x8009,"File is not permitted in BOOT; check the device filename."},
        {0x800a,"Invalid password or insufficient rights."},
        {0x800c,"File checksum is invalid."},
        {0x800f,"File not found; check the device filename."},
        {0x800e,"Unknown or vendor-specific error; consult the slave documentation."},
        {0xf1234567,"Unknown or vendor-specific error; consult the slave documentation."},
        {0,"Unknown or vendor-specific error; consult the slave documentation."},
    };
    Files files;files.write("received.bin",{7,7});files.write("source.bin",{1,2,3});
    for(const auto& item:cases)for(const auto direction:{FoeDirection::Upload,FoeDirection::Download}) {
        Mailbox mailbox;mailbox.onSend=[&](const Packet&){mailbox.incoming.push_back({Opcode::Error,item.code,{}});};
        FoeProgress last;FoeControl control;control.progress=[&](const FoeProgress& value){last=value;};
        const auto result=foe::run(mailbox.wire(),1,direction,options(files,direction==FoeDirection::Upload?"received.bin":"source.bin"),control);
        require(!result.ok&&result.message.find(item.reason)!=std::string::npos,"code-only remote error has no correct reason");
        require(last.errorCode==item.code&&last.stage==FoeStage::Failed&&last.detail==result.message,"remote error reason/code was lost from progress");
        require(mailbox.sent.size()==1&&result.data.empty()&&files.clean()&&files.read("received.bin")==std::vector<uint8_t>({7,7}),"remote error sent file data, retried, or damaged upload destination");
        if(item.code==0xf1234567)require(result.message.find("0xF1234567")!=std::string::npos,"vendor error code was truncated or normalized");
        if(item.code==0x8002)require(result.message.find("password")==std::string::npos,"access denied was incorrectly attributed to a password");
    }
    Mailbox text;const std::vector<uint8_t> payload={'b','a','d',' ','n','a','m','e','\n',0x1b,0x7f,0,'x'};
    text.onSend=[&](const Packet&){text.incoming.push_back({Opcode::Error,0x8009,payload});};
    auto result=foe::run(text.wire(8,32),1,FoeDirection::Upload,options(files,"received.bin"),{});
    require(result.message=="FoE error 0x00008009: File is not permitted in BOOT; check the device filename. | Slave message: bad name","slave error text was lost, conflated with reason, or included controls/trailing bytes");
    Mailbox vendor;vendor.onSend=[&](const Packet&){vendor.incoming.push_back({Opcode::Error,0x12345678,{'d','e','t','a','i','l'}});};
    result=foe::run(vendor.wire(8,16),1,FoeDirection::Upload,options(files,"received.bin"),{});
    require(result.message.find("0x12345678: Unknown or vendor-specific error;")!=std::string::npos&&result.message.find(" | Slave message: detail")!=std::string::npos,"unknown remote code lost its slave-provided explanation");
}
void progressThrottlingAndBootstrapValidation(){
    Files files;files.write("source.bin",std::vector<uint8_t>(800,1));Mailbox mailbox;mailbox.acknowledge();unsigned callbacks=0;FoeControl control;control.progress=[&](const FoeProgress&){++callbacks;};
    require(foe::run(mailbox.wire(),1,FoeDirection::Download,options(files,"source.bin"),control).ok,"large segmented download failed");require(callbacks<40,"per-block snapshots were not throttled");
    require(foe::validBootstrapMailbox(0x1000,128,0x1080,64,256),"valid asymmetric bootstrap rejected");
    require(!foe::validBootstrapMailbox(0,128,0,128,256),"blank bootstrap accepted");
    require(!foe::validBootstrapMailbox(0x1000,128,0x1040,128,256),"overlapping bootstrap accepted");
    require(!foe::validBootstrapMailbox(0xfff0,128,0x1000,128,256),"wrapped bootstrap accepted");
    require(!foe::validBootstrapMailbox(0x1000,12,0x1080,128,256),"header-only bootstrap accepted");
    require(!foe::validBootstrapMailbox(0x1000,512,0x1400,128,256),"oversized bootstrap accepted");
}
struct BootstrapEsc {
    foe::MailboxImage memory{};
    uint16_t state=2,failState=0,code=0;
    uint64_t time=0;
    int write0=1,write1=1,readWkc=1;
    int stateWriteWkc=1,stateReadWkc=1;
    bool corrupt=false,mirrorAck=false,stickyError=false;
    std::vector<uint16_t> requests;
    unsigned writes=0;
    foe::MailboxRegisters wire(){return {
        [&](uint16_t address,size_t size,const void* bytes){
            require(size==8&&(address==0x800||address==0x808),"bootstrap wrote outside SM0/SM1");
            ++writes;const int wkc=address==0x800?write0:write1;
            if(wkc==1)std::memcpy(memory.data()+address-0x800,bytes,size);
            return wkc;
        },
        [&](uint16_t address,size_t size,void* bytes){
            require(size==8&&(address==0x800||address==0x808),"bootstrap read outside SM0/SM1");
            std::memcpy(bytes,memory.data()+address-0x800,size);
            // SM status/PDI state may change between write and readback.
            auto* result=static_cast<uint8_t*>(bytes);result[5]=8;result[7]=2;
            if(corrupt)result[0]^=1;
            return readWkc;
        }
    };}
    bool request(uint16_t desired){return foe::requestMailboxState({
        [&](uint16_t value){
            requests.push_back(value);if(stateWriteWkc!=1)return stateWriteWkc;
            if(value==failState){if(value==3){state=0x11;code=0x0015;}return 1;}
            if(value==0x11) {if(!stickyError){state=mirrorAck?0x11:1;code=0;}}
            else if(!(state&0x10)||mirrorAck)state=value;
            return 1;
        },
        [&](uint16_t& actual,uint16_t& error){actual=state;error=code;return stateReadWkc;},
        [&]{return time;},[&](unsigned ms){time+=ms;}
    },desired,25);}
};
foe::MailboxImage mailboxImage(uint8_t high){
    foe::MailboxImage result{};
    for(unsigned sm=0;sm<2;++sm){auto* bytes=result.data()+sm*8;bytes[0]=uint8_t(sm*128);bytes[1]=high;bytes[2]=128;bytes[4]=sm?0x22:0x26;bytes[6]=1;}
    return result;
}
void nativeBootstrapLifecycleFaults(){
    using Session=foe::BootstrapSession<foe::MailboxImage>;
    const auto normal=mailboxImage(0x10),boot=mailboxImage(0x14);
    BootstrapEsc esc;esc.memory=normal;Session session;
    const auto request=[&](uint16_t state){return esc.request(state);};
    const auto configure=[&](const foe::MailboxImage& image){require(esc.state==1,"mailbox changed before confirmed INIT");return foe::programMailbox(esc.wire(),image);};
    require(session.enter(normal,boot,request,configure)&&esc.memory==boot&&esc.state==3,"native BOOT setup failed");
    require(esc.requests==std::vector<uint16_t>({1,3}),"BOOT did not follow addressed INIT then BOOT");
    require(session.leave(request,configure)&&!session.active()&&esc.state==1&&esc.memory==normal,"BOOT exit did not restore normal SM0/SM1 through INIT");
    // SM1 loses WKC after SM0 was written. Preserve the ORIGINAL normal image
    // across a failed BOOT, failed restoration, and another BOOT attempt.
    esc={};esc.memory=normal;esc.write1=0;session={};
    require(!session.enter(normal,boot,request,configure)&&session.active()&&esc.requests==std::vector<uint16_t>({1}),"partial SM failure still requested BOOT or lost restoration record");
    require(!session.leave(request,configure)&&session.active(),"failed restore discarded its saved normal context");
    esc.write1=1;esc.failState=3;
    require(!session.enter(boot,boot,request,configure)&&session.active()&&esc.state==0x11,"failed BOOT request lost saved normal context");
    esc.failState=0;
    require(session.leave(request,configure)&&esc.memory==normal&&!session.active(),"retry restored bootstrap instead of original normal image");
    require(esc.requests.size()>=2&&esc.requests[esc.requests.size()-2]==0x11&&esc.requests.back()==1,"INIT+ERR recovery did not ACK then finish plain INIT");
    // INIT confirmation is required before any SM access on entry or exit.
    esc={};esc.memory=normal;esc.failState=1;session={};
    require(!session.enter(normal,boot,request,configure)&&esc.writes==0&&session.active(),"failed INIT still changed mailboxes");
    require(!session.leave(request,configure)&&esc.writes==0&&session.active(),"failed INIT exit forgot saved context");
    esc.failState=0;require(session.leave(request,configure),"INIT recovery could not restore context");
    for(int wkc:{0,2}) {
        esc={};esc.memory=normal;esc.write0=wkc;session={};
        require(!session.enter(normal,boot,request,configure)&&esc.state==1&&esc.writes==1,"zero/multiple-target SM WKC accepted");
        esc.write0=1;require(session.leave(request,configure)&&esc.memory==normal,"SM WKC failure could not be recovered");
    }
    for(bool corrupted:{false,true}) {
        esc={};esc.memory=normal;session={};esc.readWkc=corrupted?1:0;esc.corrupt=corrupted;
        require(!session.enter(normal,boot,request,configure)&&esc.state==1&&session.active(),"failed/mismatched SM readback still requested BOOT");
        esc.readWkc=1;esc.corrupt=false;require(session.leave(request,configure)&&esc.memory==normal,"readback failure prevented explicit recovery");
    }
    // The actual-state helper is also used by the native backend, so a
    // missing/multiple-target reply can never masquerade as BOOT (INIT|PREOP).
    for(int wkc:{0,2}) {
        esc={};esc.state=3;esc.stateReadWkc=wkc;
        require(!esc.request(3)&&esc.requests.empty(),"unknown/aggregate AL reply authorized BOOT");
        esc={};esc.stateWriteWkc=wkc;require(!esc.request(1),"state write with non-unit WKC accepted");
    }
    esc={};esc.state=0;require(!esc.request(1)&&esc.requests.empty(),"missing slave state authorized INIT");
    esc={};esc.state=0x11;esc.mirrorAck=true;
    require(esc.request(1)&&esc.state==1&&esc.requests==std::vector<uint16_t>({0x11,1}),"mirrored ACK was left latched or INIT+ERR was accepted as INIT");
    esc={};esc.state=1;require(esc.request(1)&&esc.requests==std::vector<uint16_t>({1}),"ACK sent without confirmed AL error");
    esc={};esc.state=0x11;esc.code=0x15;esc.stickyError=true;
    require(!esc.request(1)&&esc.state==0x11&&esc.time<=25,"persistent AL error falsely confirmed INIT or wait was unbounded");
}
} // namespace
int main(){try{downloadExactAndImmutable();uploadAsymmetricAndDuplicate();uploadExactAndEmpty();boundedBusyAndRetries();finalAckRequiredAndCancellation();errorsAndLimits();remoteErrorReasons();progressThrottlingAndBootstrapValidation();nativeBootstrapLifecycleFaults();std::cout<<"FoE job tests passed\n";return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

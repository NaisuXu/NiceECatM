// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include "ethercat/backend.hpp"
#include "ethercat/eeprom_job.hpp"
#include "ethercat/foe_job.hpp"
#include "ethercat/bootstrap_session.hpp"
#include "ethercat/pdo_layout.hpp"
#include "ethercat/pdo_description_queue.hpp"
#include "platform/pcap_runtime.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <cerrno>
#ifndef _WIN32
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/if_arp.h>
#include <filesystem>
#else
#include <winsock2.h>
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <ipifcons.h>
#include <objbase.h>
#endif
extern "C" {
#include "soem/soem.h"
#include "oshw.h"
}
// The bundled legacy pcap headers define these compatibility macros for MSVC.
#ifdef snprintf
#undef snprintf
#endif
#ifdef vsnprintf
#undef vsnprintf
#endif
#include "ethercat/cyclic_state_poll.hpp"
#include "ethercat/sdo_dictionary.hpp"

namespace niceecatm {
namespace {
using Clock=std::chrono::steady_clock;
constexpr int RegisterTimeoutUs=20000;
constexpr int ServiceTimeoutUs=250000;
constexpr auto StateTimeout=std::chrono::milliseconds(750);
// A single lost frame is common when Windows schedules the GUI/driver or a
// redundant port is changing state. Keep inputs marked stale while allowing a
// short recovery window; stop only when the WKC remains invalid continuously.
// Windows scheduling and Npcap can briefly miss a receive window. Two
// seconds of continuous failure is long enough to ride through that jitter,
// while still preventing an indefinitely stale OP image after a real link loss.
constexpr auto WkcFailureGrace=std::chrono::seconds(2);
bool validCycle(int cycleUs) {
    return cycleUs==500||cycleUs==1000||cycleUs==2000||cycleUs==5000||cycleUs==10000||cycleUs==20000||cycleUs==50000||cycleUs==100000;
}
// A slave ESC has a 16-bit physical address space. Reserve both directions for
// every configured slave plus room for mailbox-status and overlap receive tails.
// This capacity is allocated BEFORE the SOEM mapper can write into the IOmap.
constexpr size_t MapCapacity=size_t(EC_MAXSLAVE)*2*65536+1024*1024;

std::string lowerAscii(std::string value) {
    for(char& c:value) if(c>='A'&&c<='Z') c=char(c-'A'+'a');
    return value;
}

#ifdef _WIN32
bool physicalWiredAdapter(const std::string& captureName,const std::string& description) {
    // Resolve Npcap's \\Device\\NPF_{GUID} directly to the network interface.
    // GetAdaptersInfo omits interfaces without IP bindings, even though they
    // remain usable for raw EtherCAT frames through Npcap.
    const auto begin=captureName.find('{');
    const auto end=captureName.find('}',begin==std::string::npos?0:begin+1);
    if(begin==std::string::npos||end==std::string::npos||end-begin+1!=38)return false;
    const std::wstring guidText(captureName.begin()+begin,captureName.begin()+end+1);
    GUID guid{};
    if(FAILED(CLSIDFromString(guidText.c_str(),&guid)))return false;
    const std::string label=lowerAscii(description);
    for(const char* virtualName:{"loopback","virtual","vmware","virtualbox","hyper-v","tap-windows","bluetooth","wireless","wi-fi","wifi"})
        if(label.find(virtualName)!=std::string::npos)return false;
    MIB_IF_ROW2 row{};
    if(ConvertInterfaceGuidToLuid(&guid,&row.InterfaceLuid)!=NO_ERROR||
       GetIfEntry2(&row)!=NO_ERROR)return false;
    return row.Type==MIB_IF_TYPE_ETHERNET && row.PhysicalAddressLength>=6 &&
        row.InterfaceAndOperStatusFlags.HardwareInterface &&
        (row.PhysicalMediumType==NdisPhysicalMedium802_3 ||
         row.PhysicalMediumType==NdisPhysicalMediumUnspecified);
}
#else
bool physicalWiredAdapter(const std::string& name) {
    if(name.empty()||name=="lo"||if_nametoindex(name.c_str())==0)return false;
    // A real Linux NIC has a device symlink in sysfs. This removes loopback,
    // bridges, veth/tun and other virtual interfaces while retaining a
    // disconnected physical port for later EtherCAT use.
    if(!std::filesystem::exists(std::filesystem::path("/sys/class/net")/name/"device"))return false;
    const int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return false;
    struct ifreq request{};std::snprintf(request.ifr_name,sizeof request.ifr_name,"%s",name.c_str());
    const bool ethernet=ioctl(fd,SIOCGIFHWADDR,&request)==0 && request.ifr_hwaddr.sa_family==ARPHRD_ETHER;
    close(fd);return ethernet;
}
#endif

std::string numberHex(uint64_t value,unsigned width=4)
{
    std::ostringstream out;out<<"0x"<<std::uppercase<<std::hex<<std::setw(width)<<std::setfill('0')<<value;return out.str();
}
uint16_t littleWord(const uint8_t* bytes){return uint16_t(bytes[0])|uint16_t(bytes[1])<<8;}
uint32_t littleDword(const uint8_t* bytes){return uint32_t(littleWord(bytes))|uint32_t(littleWord(bytes+2))<<16;}
bool validSiiHeader(const std::vector<uint8_t>& bytes){
    if(bytes.size()<128)return false;
    uint8_t crc=0xFF;
    for(size_t i=0;i<14;++i){crc^=bytes[i];for(int bit=0;bit<8;++bit)crc=uint8_t((crc&0x80)?(crc<<1)^7:crc<<1);}
    return crc==bytes[14];
}
class SoemBackend final: public Backend {
    std::unique_ptr<ecx_contextt> context_;
    std::unique_ptr<ecx_redportt> redundant_;
    std::vector<uint8_t> io_;
    Snapshot view_;
    bool initialized_=false;
    unsigned invalidCycles_=0;
    Clock::time_point previousCycle_{}, statePoll_{}, invalidSince_{};
    std::vector<uint16_t> requested_;
    CyclicStatePoll stateMonitor_;
    std::vector<Result> diagnostics_;
    std::map<int,PdoDescriptionQueue> pdoDescriptions_;
    std::unique_ptr<ec_ODlistt> pdoObjects_;
    std::unique_ptr<ec_OElistt> pdoObjectEntries_;
    struct MailboxConfig {
        uint16_t txOffset=0,txLength=0,rxOffset=0,rxLength=0;
        ec_smt sm0{},sm1{};
    };
    std::set<int> bootstrapSlaves_;
    std::map<int,foe::BootstrapSession<MailboxConfig>> mailboxSessions_;

    bool readActualState(int slave) {
        auto& native=context_->slavelist[slave];ec_alstatust actual{};
        if(ecx_FPRD(&context_->port,native.configadr,ECT_REG_ALSTAT,sizeof(actual),&actual,RegisterTimeoutUs)!=1) {
            native.state=0;return false;
        }
        native.state=etohs(actual.alstatus);native.ALstatuscode=etohs(actual.alstatuscode);
        const auto state=native.state&15;
        return state==1||state==2||state==3||state==4||state==8;
    }
    bool applyMailboxState(int slave,uint16_t desired) {
        auto& native=context_->slavelist[slave];
        return foe::requestMailboxState({
            [&](uint16_t value){uint16_t request=htoes(value);return ecx_FPWR(&context_->port,native.configadr,ECT_REG_ALCTL,sizeof(request),&request,RegisterTimeoutUs);},
            [&](uint16_t& state,uint16_t& code){if(!readActualState(slave))return 0;state=native.state;code=native.ALstatuscode;return 1;},
            []{return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());},
            [](unsigned ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
        },desired,unsigned(StateTimeout.count()));
    }

    bool readBootstrap(int slave,MailboxConfig& config,std::string& issue) {
        const auto address=context_->slavelist[slave].configadr;
        eeprom::Session session({
            [this,address](uint16_t reg,size_t size,void* data){return ecx_FPRD(&context_->port,address,reg,uint16_t(size),data,RegisterTimeoutUs);},
            [this,address](uint16_t reg,size_t size,const void* data){return ecx_FPWR(&context_->port,address,reg,uint16_t(size),const_cast<void*>(data),RegisterTimeoutUs);}
        });
        std::vector<uint8_t> bytes;
        const bool read=session.begin(issue)&&session.read(ECT_SII_BOOTRXMBX*2,8,128,bytes,issue);
        std::string cleanup;const bool restored=session.finish(cleanup);
        if(!read||!restored) {if(!cleanup.empty())issue+=" "+cleanup;return false;}
        config.txOffset=littleWord(bytes.data());config.txLength=littleWord(bytes.data()+2);
        config.rxOffset=littleWord(bytes.data()+4);config.rxLength=littleWord(bytes.data()+6);
        if(!foe::validBootstrapMailbox(config.txOffset,config.txLength,config.rxOffset,config.rxLength,EC_MAXMBX)) {
            issue="SII does not provide valid, separate Bootstrap SM0/SM1 mailboxes.";return false;
        }
        config.sm0.StartAddr=htoes(config.txOffset);config.sm0.SMlength=htoes(config.txLength);config.sm0.SMflags=htoel(0x00010026);
        config.sm1.StartAddr=htoes(config.rxOffset);config.sm1.SMlength=htoes(config.rxLength);config.sm1.SMflags=htoel(0x00010022);
        return true;
    }
    MailboxConfig normalMailbox(int slave) const {
        const auto& native=context_->slavelist[slave];
        return {native.mbx_wo,native.mbx_l,native.mbx_ro,native.mbx_rl,native.SM[0],native.SM[1]};
    }
    bool configureMailbox(int slave,const MailboxConfig& config) {
        auto& native=context_->slavelist[slave];
        static_assert(sizeof(ec_smt)==8,"Unexpected SOEM SyncManager layout");
        foe::MailboxImage image{};std::memcpy(image.data(),&config.sm0,8);std::memcpy(image.data()+8,&config.sm1,8);
        const uint16_t address=native.configadr;
        if(!foe::programMailbox({
            [this,address](uint16_t reg,size_t size,const void* data){return ecx_FPWR(&context_->port,address,reg,uint16_t(size),const_cast<void*>(data),RegisterTimeoutUs);},
            [this,address](uint16_t reg,size_t size,void* data){return ecx_FPRD(&context_->port,address,reg,uint16_t(size),data,RegisterTimeoutUs);}
        },image))return false;
        native.SM[0]=config.sm0;native.SM[1]=config.sm1;
        native.mbx_wo=config.txOffset;native.mbx_l=config.txLength;native.mbx_ro=config.rxOffset;native.mbx_rl=config.rxLength;
        native.mbx_cnt=0;native.mbxhandlerstate=0;
        return true;
    }
    bool restoreMailbox(int slave) {
        const auto found=mailboxSessions_.find(slave);
        if(found==mailboxSessions_.end())return true;
        if(!found->second.leave([&](uint16_t state){return applyMailboxState(slave,state);},[&](const MailboxConfig& config){return configureMailbox(slave,config);}))return false;
        mailboxSessions_.erase(found);return true;
    }

    Result enterBoot(int target) {
        if(view_.running)return error("Stop cyclic I/O by switching all slaves to PRE-OP before BOOT.");
        for(int s=1;s<=context_->slavecount;++s) {
            if(!readActualState(s)){updateSlaves();return error("BOOT preflight could not confirm a live AL state for slave "+std::to_string(s)+".");}
            const auto actual=context_->slavelist[s].state;
            if(actual==EC_STATE_OPERATIONAL||(actual&EC_STATE_ERROR))
                return error("BOOT requires stopped cyclic I/O, no OP slaves, and no AL errors.");
        }
        std::map<int,MailboxConfig> checked;
        // Read and validate the WHOLE requested scope before changing any AL
        // state. No BOOT broadcast or bitwise aggregate state confirmation.
        for(int s=target?target:1;s<=(target?target:context_->slavecount);++s) {
            MailboxConfig config;std::string issue;
            if(!readBootstrap(s,config,issue)) {updateSlaves();return error("BOOT preflight failed for slave "+std::to_string(s)+": "+issue);}
            checked.emplace(s,config);
        }
        for(const auto& item:checked)bootstrapSlaves_.insert(item.first);
        invalidateMapping();view_.running=false;stateMonitor_.reset();
        for(const auto& [s,config]:checked) {
            auto& native=context_->slavelist[s];
            if(native.state==EC_STATE_BOOT&&mailboxSessions_.count(s))continue;
            requested_[s]=EC_STATE_BOOT;
            if(!mailboxSessions_[s].enter(normalMailbox(s),config,[&](uint16_t state){return applyMailboxState(s,state);},[&](const MailboxConfig& mailbox){return configureMailbox(s,mailbox);})) {
                ecx_readstate(context_.get());updateSlaves();
                return error(errors("BOOT configuration/state was not confirmed for slave "+std::to_string(s)+"; "+stateFailure(s)));
            }
        }
        ecx_readstate(context_.get());updateSlaves();
        return done("BOOT confirmed individually; Bootstrap Mailbox configured. Scan again before restarting cyclic I/O.");
    }

    eeprom::Wire eepromWire(int slave){
        const uint16_t position=uint16_t(1-slave);
        return {
            [this,position](uint16_t address,size_t size,void* bytes){return ecx_APRD(&context_->port,position,address,uint16_t(size),bytes,RegisterTimeoutUs);},
            [this,position](uint16_t address,size_t size,const void* bytes){return ecx_APWR(&context_->port,position,address,uint16_t(size),const_cast<void*>(bytes),RegisterTimeoutUs);}
        };
    }
    bool programmingTopology(int target,std::string& issue){
        uint16_t type=0;
        const int count=ecx_BRD(&context_->port,0,ECT_REG_TYPE,sizeof(type),&type,RegisterTimeoutUs);
        if(count!=context_->slavecount){
            issue="Programming topology BRD ECT_REG_TYPE WKC mismatch: expected "+std::to_string(context_->slavecount)+
                  ", received "+std::to_string(count)+"; the link or slave count changed. Scan again.";
            return false;
        }
        for(const auto& slave:view_.slaves){
            uint8_t identity[4]{};
            const uint16_t ap=uint16_t(1-slave.position);
            const int wkc=ecx_APRD(&context_->port,ap,ECT_REG_TYPE,sizeof(identity),identity,RegisterTimeoutUs);
            if(wkc!=1){
                issue="Programming topology AP ECT_REG_TYPE WKC mismatch at slave "+std::to_string(slave.position)+
                      " (AP "+std::to_string(ap)+"): expected 1, received "+std::to_string(wkc)+".";
                return false;
            }
            const uint32_t actualId=littleDword(identity);
            if(actualId!=slave.escId){
                issue="Programming topology ESC identity mismatch at slave "+std::to_string(slave.position)+
                      ": expected "+numberHex(slave.escId,8)+", received "+numberHex(actualId,8)+"; scan again.";
                return false;
            }
        }
        uint8_t state[6]{};
        const uint16_t ap=uint16_t(1-target);
        const int wkc=ecx_APRD(&context_->port,ap,ECT_REG_ALSTAT,sizeof(state),state,RegisterTimeoutUs);
        if(wkc!=1){
            issue="Programming target AP ECT_REG_ALSTAT WKC mismatch at slave "+std::to_string(target)+
                  " (AP "+std::to_string(ap)+"): expected 1, received "+std::to_string(wkc)+".";
            return false;
        }
        auto& slave=view_.slaves[size_t(target-1)];
        slave.state=littleWord(state);slave.alCode=littleWord(state+4);
        context_->slavelist[target].state=slave.state;context_->slavelist[target].ALstatuscode=slave.alCode;
        if((slave.state&15)!=EC_STATE_INIT){
            issue="Programming target actual AL state is "+numberHex(slave.state,4)+
                  " (AL code "+numberHex(slave.alCode,4)+"); expected INIT. Scan again.";
            return false;
        }
        return true;
    }
    Result eepromOperation(int target,size_t length,const std::vector<uint8_t>* image,const EepromControl& control){
        if(view_.connection.mode!=ConnectionMode::Programming||!validSlave(target)||view_.running||view_.mapped)
            return error("Select Programming, scan, and select an INIT slave before EEPROM access.");
        if(length<128||length>eeprom::Session::MaxCapacity||length%2||(image&&image->size()!=length))
            return error("EEPROM length must be an even size from 128 to 131072 bytes.");
        const Slave original=view_.slaves[size_t(target-1)];
        if(image&&!validSiiHeader(*image))return error("EEPROM image configuration CRC is invalid.");
        auto report=[&](EepromStage stage,size_t completed,bool written,const std::string& detail){
            view_.eeprom={stage,completed,image?length*2:length,written,detail};
            if(control.progress)control.progress(view_.eeprom);
        };
        const auto cancelled=[&]{return control.cancelled&&control.cancelled();};
        report(EepromStage::Preparing,0,false,"Checking programming target and EEPROM ownership");
        std::string issue;
        if(cancelled()){report(EepromStage::Cancelled,0,false,"Cancelled before EEPROM access");return error("EEPROM operation cancelled; no EEPROM data was written.");}
        if(!programmingTopology(target,issue)){report(EepromStage::Failed,0,false,issue);return error(issue);}
        eeprom::Session session(eepromWire(target),cancelled);
        size_t completed=0;
        auto finish=[&](bool success,EepromStage stage,std::string message,std::vector<uint8_t> data=std::vector<uint8_t>{}){
            std::string cleanup;
            if(!session.finish(cleanup)){success=false;stage=EepromStage::Failed;message+=" "+cleanup;data.clear();}
            if(session.writeAttempted()){
                auto& changed=view_.slaves[size_t(target-1)];
                changed.eepromIdentityKnown=false;changed.eepromSizeKnown=false;changed.eepromCapacity=0;
                changed.esiFile.clear();changed.esiStatus.clear();changed.sdoDictionary.reset();changed.esiSdoDictionary.reset();changed.pdoEntries.clear();
            }
            if(!success&&session.writeAttempted())message+=" EEPROM may be partially programmed; no rollback was performed.";
            report(stage,completed,session.writeAttempted(),message);
            if(!success)return error(message);
            view_.status=message;return Result{true,std::move(message),std::move(data)};
        };
        if(!session.begin(issue))return finish(false,EepromStage::Failed,issue);
        // Re-read every available identifying field immediately before the
        // operation. ESI descriptions never substitute for physical identity.
        if(original.eepromIdentityKnown){
            std::vector<uint8_t> header;
            if(!session.read(0,128,128,header,issue))return finish(false,cancelled()?EepromStage::Cancelled:EepromStage::Failed,issue);
            if(!validSiiHeader(header)||littleDword(header.data()+16)!=original.vendor||littleDword(header.data()+20)!=original.product||
               littleDword(header.data()+24)!=original.revision||littleDword(header.data()+28)!=original.serial||littleWord(header.data()+8)!=original.eepromAlias)
                return finish(false,EepromStage::Failed,"Programming target SII identity changed since scanning; scan again.");
        }
        auto nextProbe=Clock::now(),nextProgress=Clock::now();
        auto checkpoint=[&](){
            if(cancelled()){issue="EEPROM operation cancelled.";return false;}
            if(Clock::now()>=nextProbe){
                if(!programmingTopology(target,issue))return false;
                nextProbe=Clock::now()+std::chrono::milliseconds(100);
            }
            return true;
        };
        if(image){
            // Read the current image first.  Besides avoiding unnecessary
            // EEPROM wear, this is required by devices such as AX58100 whose
            // protected configuration/category words reject a redundant write.
            std::vector<uint8_t> existing;
            report(EepromStage::Writing,0,false,"Reading current EEPROM before programming");
            if(!session.read(0,length,length,existing,issue))
                return finish(false,cancelled()?EepromStage::Cancelled:EepromStage::Failed,"EEPROM pre-write read failed: "+issue);
            report(EepromStage::Writing,0,false,"Writing EEPROM");
            for(size_t offset=0;offset<length;offset+=2){
                if(!checkpoint())return finish(false,cancelled()?EepromStage::Cancelled:EepromStage::Failed,issue);
                if(existing.size()==length&&littleWord(existing.data()+offset)==littleWord(image->data()+offset)){
                    completed=offset+2;continue;
                }
                if(!session.writeWord(offset,littleWord(image->data()+offset),issue))
                    return finish(false,EepromStage::Failed,"EEPROM write failed at word "+numberHex(offset/2)+": "+issue);
                completed=offset+2;
                if(Clock::now()>=nextProgress){report(EepromStage::Writing,completed,true,"Writing EEPROM");nextProgress=Clock::now()+std::chrono::milliseconds(100);}
            }
        }
        const auto readStage=image?EepromStage::Verifying:EepromStage::Reading;
        report(readStage,completed,session.writeAttempted(),image?"Reading back and comparing EEPROM":"Reading EEPROM");
        std::vector<uint8_t> contents;contents.reserve(length);
        for(size_t offset=0;offset<length;offset+=4){
            if(!checkpoint())return finish(false,cancelled()?EepromStage::Cancelled:EepromStage::Failed,issue);
            std::vector<uint8_t> bytes;
            const size_t count=std::min(size_t(4),length-offset);
            if(!session.read(offset,count,length,bytes,issue))
                return finish(false,cancelled()?EepromStage::Cancelled:EepromStage::Failed,
                              (cancelled()?"EEPROM read cancelled at byte ":"EEPROM read failed at byte ")+numberHex(offset)+": "+issue);
            if(image){
                for(size_t i=0;i<bytes.size();++i)if(bytes[i]!=(*image)[offset+i])
                    return finish(false,EepromStage::Failed,"EEPROM readback mismatch at byte "+numberHex(offset+i)+": expected "+numberHex((*image)[offset+i],2)+", received "+numberHex(bytes[i],2)+".");
            }else contents.insert(contents.end(),bytes.begin(),bytes.end());
            completed=(image?length:0)+offset+count;
            if(Clock::now()>=nextProgress){report(readStage,completed,session.writeAttempted(),image?"Comparing full EEPROM readback":"Reading EEPROM");nextProgress=Clock::now()+std::chrono::milliseconds(100);}
        }
        if(!checkpoint())return finish(false,cancelled()?EepromStage::Cancelled:EepromStage::Failed,issue);
        return finish(true,EepromStage::Complete,image?"EEPROM written and fully verified; activation pending. Follow the device's reload or power-cycle procedure.":"EEPROM read completed; all bytes were received successfully.",std::move(contents));
    }

    Result error(const std::string& message) {
        view_.status=message;return {false,message,{}};
    }
    std::string errors(const std::string& prefix) {
        std::string message=prefix;
        if(context_) {ec_errort entry;unsigned count=0;while(ecx_poperror(context_.get(),&entry)) {
            if(count++<8) {const char* text=ecx_err2string(entry);if(text)message+=" | "+std::string(text);}
        }}
        return message;
    }
    Result done(const std::string& message) {view_.status=message;return {true,message,{}};}
    bool validSlave(int slave) const {return initialized_ && view_.connected && slave>0 && slave<=context_->slavecount;}
    bool hasOp() const {
        if(!context_)return false;
        for(int s=1;s<=context_->slavecount;++s)if((context_->slavelist[s].state&15)==EC_STATE_OPERATIONAL)return true;
        return false;
    }
    bool inMap(const uint8* pointer,size_t length) const {
        if(!pointer)return length==0;
        const auto begin=reinterpret_cast<uintptr_t>(io_.data()), address=reinterpret_cast<uintptr_t>(pointer);
        return address>=begin && address-begin<=io_.size() && length<=io_.size()-(address-begin);
    }
    void updateSlaves(bool processValid=false) {
        if(!context_)return;
        view_.slaves.resize(context_->slavecount);
        for(int s=1;s<=context_->slavecount;++s) {
            const auto& native=context_->slavelist[s];auto& slave=view_.slaves[s-1];
            slave.position=s;slave.name=native.name;slave.vendor=native.eep_man;slave.product=native.eep_id;
            slave.revision=native.eep_rev;slave.serial=native.eep_ser;slave.state=native.state;
            slave.requested=size_t(s)<requested_.size()?requested_[s]:native.state;
            slave.alCode=native.ALstatuscode;slave.address=native.configadr;slave.mailboxProtocols=native.mbx_proto;
            slave.bootstrapMailbox=bootstrapSlaves_.count(s)!=0;
            slave.sdoInfo=(native.CoEdetails&ECT_COEDET_SDOINFO)!=0;
            slave.dc=native.hasdc!=0;slave.inputBits=native.Ibits;slave.outputBits=native.Obits;
            slave.inputStartBit=native.Istartbit;slave.outputStartBit=native.Ostartbit;
            slave.inputOffset=native.inputs&&inMap(native.inputs,0)?int(native.inputs-io_.data()):-1;
            slave.outputOffset=native.outputs&&inMap(native.outputs,0)?int(native.outputs-io_.data()):-1;
            const size_t ni=(size_t(native.Ibits)+7)/8, no=(size_t(native.Obits)+7)/8;
            if(view_.mapped && ni && inMap(native.inputs,(size_t(native.Ibits)+native.Istartbit+7)/8)) {
                if(processValid || slave.inputs.size()!=ni) {
                    slave.inputs.assign(ni,0);
                    for(unsigned bit=0;bit<native.Ibits;++bit) {
                        const unsigned source=bit+native.Istartbit;
                        if(native.inputs[source/8]&(1u<<(source%8)))slave.inputs[bit/8]|=uint8_t(1u<<(bit%8));
                    }
                }
            } else slave.inputs.clear();
            if(view_.mapped && no && inMap(native.outputs,(size_t(native.Obits)+native.Ostartbit+7)/8)) {
                slave.outputs.assign(no,0);
                for(unsigned bit=0;bit<native.Obits;++bit) {
                    const unsigned source=bit+native.Ostartbit;
                    if(native.outputs[source/8]&(1u<<(source%8)))slave.outputs[bit/8]|=uint8_t(1u<<(bit%8));
                }
            } else slave.outputs.clear();
        }
    }
    void invalidateMapping() {
        view_.mapped=false;
        pdoDescriptions_.clear();
        for(auto& slave:view_.slaves) {
            slave.inputLayoutKnown=slave.outputLayoutKnown=false;slave.pdoEntries.clear();
            slave.pdoIssue="Process mapping invalid; scan again.";
        }
    }
    bool metadataScalar(int slave,uint16_t index,uint8_t sub,unsigned minimum,unsigned maximum,
                        uint32_t& value,Clock::time_point deadline) {
        if(Clock::now()>=deadline)return false;
        const auto remaining=std::chrono::duration_cast<std::chrono::microseconds>(deadline-Clock::now()).count();
        uint8_t bytes[4]{};int length=int(maximum);
        if(ecx_SDOread(context_.get(),uint16(slave),index,sub,FALSE,&length,bytes,
                       int(std::clamp<int64_t>(remaining,1,100000)))<=0||length<int(minimum)||length>int(maximum))return false;
        value=0;for(int n=0;n<length;++n)value|=uint32_t(bytes[n])<<(8*n);
        return true;
    }
    bool coeEntries(int slave,bool input,const std::array<pdo_layout::SyncManager,8>& managers,
                    std::vector<PdoEntry>& entries,std::string& issue,Clock::time_point deadline) {
        std::vector<PdoEntry> candidate;
        for(size_t sm=0;sm<managers.size();++sm) {
            if(managers[sm].type!=(input?4u:3u))continue;
            uint32_t count=0;
            if(!metadataScalar(slave,uint16_t(ECT_SDO_PDOASSIGN+sm),0,1,2,count,deadline)||count>255) {
                issue="Current CoE PDO assignment could not be read.";return false;
            }
            for(unsigned n=1;n<=count;++n) {
                uint32_t pdo=0,fields=0;
                if(!metadataScalar(slave,uint16_t(ECT_SDO_PDOASSIGN+sm),uint8_t(n),2,2,pdo,deadline)||!pdo||
                   !metadataScalar(slave,uint16_t(pdo),0,1,1,fields,deadline)) {
                    issue="Current CoE PDO assignment could not be read.";return false;
                }
                for(unsigned field=1;field<=fields;++field) {
                    uint32_t mapping=0;
                    if(candidate.size()>=pdo_layout::MaxEntries||
                       !metadataScalar(slave,uint16_t(pdo),uint8_t(field),4,4,mapping,deadline)||!(mapping&0xff)) {
                        issue="Current CoE PDO entries could not be read.";return false;
                    }
                    PdoEntry entry;entry.input=input;entry.syncManager=int(sm);entry.pdoIndex=uint16_t(pdo);
                    entry.index=uint16_t(mapping>>16);entry.subindex=uint8_t(mapping>>8);entry.bitLength=mapping&0xff;
                    entry.padding=entry.index==0&&entry.subindex==0;entry.source="CoE";
                    candidate.push_back(std::move(entry));
                }
            }
        }
        if(!pdo_layout::validate(candidate,input,input?context_->slavelist[slave].Ibits:context_->slavelist[slave].Obits,managers,issue))return false;
        entries=std::move(candidate);return true;
    }
    // Walk within the compiled SII cache and the shared description deadline,
    // validating the complete category payload before reading its contents.
    bool siiCategory(int slave,uint16_t category,std::vector<uint8_t>& bytes,Clock::time_point deadline) {
        auto byte=[&](unsigned at,uint8_t& value) {
            if(at>=EC_MAXEEPBUF-8||Clock::now()>=deadline)return false;
            value=ecx_siigetbyte(context_.get(),uint16(slave),uint16(at));return true;
        };
        auto word=[&](unsigned at,unsigned& value) {
            uint8_t lo=0,hi=0;if(!byte(at,lo)||!byte(at+1,hi))return false;
            value=unsigned(lo)|(unsigned(hi)<<8);return true;
        };
        for(unsigned at=ECT_SII_START*2,sections=0;at+4<EC_MAXEEPBUF-8&&sections<128;++sections) {
            unsigned type=0,words=0;if(!word(at,type))return false;
            if(type==0xffff){bytes.clear();return true;}
            if(!word(at+2,words)||words>(EC_MAXEEPBUF-8-at-4)/2)return false;
            const unsigned length=words*2;
            if((type&0x7fff)==category) {
                bytes.resize(length);
                for(unsigned n=0;n<length;++n)if(!byte(at+4+n,bytes[n]))return false;
                return true;
            }
            at+=4+length;
        }
        return false;
    }
    PdoDescription readPdoDescription(int slave,uint16_t index,uint8_t sub) {
        if(!pdoObjects_)pdoObjects_=std::make_unique<ec_ODlistt>();
        if(!pdoObjectEntries_)pdoObjectEntries_=std::make_unique<ec_OElistt>();
        pdoObjects_->Slave=uint16_t(slave);pdoObjects_->Index[0]=index;
        pdoObjectEntries_->Entries=0;
        // Keep optional mailbox failures local to this query. Other commands
        // still need to see any errors that were pending when it started.
        std::vector<ec_errort> saved;ec_errort error{};
        while(ecx_poperror(context_.get(),&error))saved.push_back(error);
        const int wkc=ecx_readOEsingle(context_.get(),0,sub,pdoObjects_.get(),pdoObjectEntries_.get());
        bool sawError=false,allMissing=true;
        while(ecx_poperror(context_.get(),&error)) {
            sawError=true;
            if(error.Etype!=EC_ERR_TYPE_SDOINFO_ERROR||
               (uint32_t(error.AbortCode)!=0x06020000&&uint32_t(error.AbortCode)!=0x06090011))allMissing=false;
        }
        for(const auto& old:saved)ecx_pusherror(context_.get(),&old);
        if(wkc<=0||!pdoObjectEntries_->Entries)
            return {sawError&&allMissing?PdoDescriptionStatus::Missing:PdoDescriptionStatus::Failed,0,0,{}};
        return {PdoDescriptionStatus::Available,pdoObjectEntries_->DataType[sub],
                pdoObjectEntries_->BitLength[sub],pdoObjectEntries_->Name[sub]};
    }
    bool advancePdoDescriptions(Slave& slave,PdoDescriptionQueue& queue) {
        const bool changed=queue.step(slave.pdoEntries,[&](uint16_t index,uint8_t sub) {
            return readPdoDescription(slave.position,index,sub);
        });
        if(!queue.pending()&&queue.unavailable()) {
            auto message="Slave "+std::to_string(slave.position)+": CoE descriptions unavailable for "+
                std::to_string(queue.unavailable())+" mapped object(s).";
            if(queue.stoppedAfterFailures())message+=" Stopped after three consecutive SDO Information request failures.";
            diagnostics_.push_back({true,std::move(message),{}});
        }
        return changed;
    }
    void describeCoeNames(Slave& slave,Clock::time_point deadline) {
        if(!slave.sdoInfo)return;
        PdoDescriptionQueue queue(slave.pdoEntries);
        const auto reserve=std::chrono::microseconds(EC_TIMEOUTRXM+EC_TIMEOUTTXM+10000);
        for(unsigned n=0;n<128&&queue.pending()&&slave.state==EC_STATE_PRE_OP&&deadline-Clock::now()>=reserve;++n)
            advancePdoDescriptions(slave,queue);
        if(queue.pending())pdoDescriptions_.emplace(slave.position,std::move(queue));
    }
    void discoverPdoEntries() {
        if(!context_||!view_.mapped)return;
        // Optional description requests must not consume or replace transport
        // failures already recorded by the scan/map operation.
        std::vector<ec_errort> saved;ec_errort error{};
        while(ecx_poperror(context_.get(),&error))saved.push_back(error);
        const auto overall=Clock::now()+std::chrono::seconds(6);
        for(auto& slave:view_.slaves) {
            slave.pdoEntries.clear();slave.pdoIssue.clear();
            slave.inputLayoutKnown=slave.inputBits==0;slave.outputLayoutKnown=slave.outputBits==0;
            if(Clock::now()>=overall){slave.pdoIssue="PDO description time budget exceeded.";continue;}
            const auto deadline=std::min(overall,Clock::now()+std::chrono::milliseconds(2000));
            const auto& native=context_->slavelist[slave.position];
            std::array<pdo_layout::SyncManager,8> managers{};
            for(size_t sm=0;sm<managers.size();++sm)managers[sm]={native.SMtype[sm],etohs(native.SM[sm].SMlength)};
            const bool coe=(native.mbx_proto&ECT_MBXPROT_COE)!=0;
            if(!coe&&(native.mbx_proto&ECT_MBXPROT_SOE)) {
                slave.pdoIssue="Online SoE signal descriptions are not available.";continue;
            }
            const bool restorePdi=native.eep_pdi!=0;
            std::vector<std::string> names;
            if(!coe) {
                std::vector<uint8_t> bytes;
                if(siiCategory(slave.position,ECT_SII_STRING,bytes,deadline))pdo_layout::strings(bytes,names);
            }
            for(const bool input:{false,true}) {
                const unsigned bits=input?slave.inputBits:slave.outputBits;if(!bits)continue;
                std::vector<PdoEntry> entries;std::string issue;bool valid=false;
                if(coe)valid=coeEntries(slave.position,input,managers,entries,issue,deadline);
                else {
                    std::vector<uint8_t> bytes;
                    valid=siiCategory(slave.position,uint16_t(ECT_SII_PDO+(input?0:1)),bytes,deadline)&&
                        pdo_layout::sii(bytes,input,names,entries,issue)&&pdo_layout::validate(entries,input,bits,managers,issue);
                    if(!valid&&issue.empty())issue="SII PDO descriptions could not be read.";
                }
                if(valid) {
                    (input?slave.inputLayoutKnown:slave.outputLayoutKnown)=true;
                    slave.pdoEntries.insert(slave.pdoEntries.end(),entries.begin(),entries.end());
                } else {
                    if(!slave.pdoIssue.empty())slave.pdoIssue+=" ";
                    slave.pdoIssue+=std::string(input?"IN: ":"OUT: ")+issue;
                }
            }
            if(coe)describeCoeNames(slave,deadline);
            else if(restorePdi)ecx_eeprom2pdi(context_.get(),uint16(slave.position));
            // Consume only optional description errors. Actual scan failures
            // are restored below and remain visible in the normal event log.
            while(ecx_poperror(context_.get(),&error)){}
        }
        for(const auto& entry:saved)ecx_pusherror(context_.get(),&entry);
    }
    bool validateMap(int mapperExtent) const {
        const auto& group=context_->grouplist[0];
        if(!pdo_layout::mappingComplete(mapperExtent,group.nsegments,EC_MAXBUF))return false;
        // LRW overlap receives a full frame after the outputs, not merely Ibytes.
        const size_t length=view_.connection.overlap?
            std::max(group.Obytes,group.Ibytes)+size_t(group.mbxstatuslength):
            size_t(group.Obytes)+group.Ibytes+group.mbxstatuslength;
        uint8* start=group.Obytes?group.outputs:group.inputs;
        const size_t tail=view_.connection.overlap && group.Obytes?group.Obytes:0;
        if(length && !inMap(start,length+tail))return false;
        for(int s=1;s<=context_->slavecount;++s) {
            const auto& slave=context_->slavelist[s];
            if(!inMap(slave.inputs,(size_t(slave.Ibits)+slave.Istartbit+7)/8) ||
               !inMap(slave.outputs,(size_t(slave.Obits)+slave.Ostartbit+7)/8))return false;
        }
        return true;
    }
    void exchange() {
#ifdef _WIN32
        // Npcap and the Windows scheduler can deliver a valid frame later than
        // a sub-millisecond target period. Do not mistake that latency for a
        // missing frame by shrinking the receive window to half the period.
        // Allow at least SOEM's normal receive timeout, with a bounded wait for
        // slower periods. A redundant port may be disconnected while the
        // other path remains usable; cap that missing-path wait to a quarter
        // cycle so redundancy does not silently double the target period. The
        // controller still records actual/missed cycles; this does not make
        // Windows a realtime EtherCAT master.
        const bool redundant=context_&&context_->port.redport!=nullptr;
        const int receiveTimeout=redundant?
            std::clamp(view_.connection.cycleUs/4,EC_TIMEOUTRET,2000):
            std::clamp(view_.connection.cycleUs,EC_TIMEOUTRET,10000);
#else
        const bool redundant=context_&&context_->port.redport!=nullptr;
        const int receiveTimeout=redundant?
            std::clamp(view_.connection.cycleUs/4,100,2000):
            std::clamp(view_.connection.cycleUs/2,100,2000);
#endif
        const auto& group=context_->grouplist[0];
        if(group.Obytes==0 && group.Ibytes==0 && group.mbxstatuslength==0) {
            // A legitimate mailbox-free device can have no PDOs at all. Keep
            // its link observable with a read-only AL-status heartbeat rather
            // than treating SOEM's "no process frame" return as a lost slave.
            uint16_t status=0;
            view_.wkc=ecx_BRD(&context_->port,0,ECT_REG_ALSTAT,sizeof(status),&status,
                             receiveTimeout);
            return;
        }
        ecx_send_processdata(context_.get());
        view_.wkc=ecx_receive_processdata(context_.get(),receiveTimeout);
        // Npcap can occasionally lose a process-data frame even when the
        // EtherCAT chain is healthy. A single bounded resend keeps that
        // isolated capture miss from being reported as a stale I/O sample.
        // The retry uses the same output image and never changes the target
        // state; it is only attempted after an incomplete response.
        const int expected=view_.expectedWkc;
        if(view_.wkc<expected) {
            ecx_send_processdata(context_.get());
            const int retryTimeout=std::clamp(receiveTimeout/2,EC_TIMEOUTRET,2000);
            const int retry=ecx_receive_processdata(context_.get(),retryTimeout);
            if(retry>view_.wkc)view_.wkc=retry;
        }
    }
    bool confirmState(int slave,uint16_t requested,bool exchangeFrames) {
        auto deadline=Clock::now()+StateTimeout;
        do {
            if(exchangeFrames && view_.mapped)exchange();
            ecx_readstate(context_.get());
            bool complete=true;
            for(int s=slave?slave:1;s<=(slave?slave:context_->slavecount);++s) {
                if(context_->slavelist[s].state!=requested)complete=false;
            }
            if(complete)return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while(Clock::now()<deadline);
        return false;
    }
    bool applyState(int slave,uint16_t requested,bool exchangeFrames) {
        if(exchangeFrames && view_.mapped)exchange();
        context_->slavelist[slave].state=requested;
        if(ecx_writestate(context_.get(),uint16(slave))<=0)return false;
        return confirmState(slave,requested,exchangeFrames);
    }
    std::string stateFailure(int target) const {
        std::ostringstream message;message<<"State request was not confirmed";
        for(int s=target?target:1;s<=(target?target:context_->slavecount);++s) {
            const auto& slave=context_->slavelist[s];
            message<<"; slave "<<s<<" actual "<<stateName(slave.state)<<" AL "<<numberHex(slave.ALstatuscode);
            if(slave.ALstatuscode)message<<" ("<<ec_ALstatuscode2string(slave.ALstatuscode)<<")";
        }
        return message.str();
    }
public:
    SoemBackend() {view_.simulation=false;view_.connection.mode=ConnectionMode::Hardware;view_.status="Hardware mode: choose an interface and scan";}
    ~SoemBackend() override {close();}
    Result adapters() override {
        view_.adapters.clear();
#ifdef _WIN32
        view_.driver=niceecatm_pcap_status();
        if(!niceecatm_pcap_available())return error(view_.driver);
#else
        view_.driver="Linux AF_PACKET raw Ethernet (network access permissions required)";
#endif
        ec_adaptert* list=ec_find_adapters();
        for(auto* item=list;item;item=item->next) {
            if(physicalWiredAdapter(item->name,item->desc))view_.adapters.push_back({item->name,item->desc});
        }
        ec_free_adapters(list);
        if(view_.adapters.empty()) {
#ifdef _WIN32
            return error(std::string("No physical wired Ethernet interfaces available: ")+niceecatm_pcap_error());
#else
            return error("No physical wired Ethernet interfaces available");
#endif
        }
        return done("Physical wired Ethernet interfaces enumerated: "+std::to_string(view_.adapters.size()));
    }
    Result scan(const Connection& config) override {
        if(config.mode==ConnectionMode::Programming)return error("Use the dedicated Programming scan without PDO mapping.");
        if(view_.running)return error("Stop cyclic exchange before scanning");
        if(config.primary.empty())return error("Select a primary network interface");
        if(config.redundant && (config.secondary.empty() || config.primary==config.secondary))
            return error("Same-bus redundancy requires two different network interfaces");
        if(config.primary.size()>=EC_MAXLEN_ADAPTERNAME || (config.redundant && config.secondary.size()>=EC_MAXLEN_ADAPTERNAME))
            return error("Network interface identifier exceeds the supported length");
#ifndef _WIN32
        // Upstream Linux setupnic uses strcpy into ifreq.ifr_name.
        if(config.primary.size()>=IFNAMSIZ || (config.redundant && config.secondary.size()>=IFNAMSIZ))
            return error("Linux network interface names must be shorter than IFNAMSIZ");
#endif
        if(!validCycle(config.cycleUs))
            return error("Target cycle must be 500, 1000, 2000, 5000, 10000, 20000, 50000 or 100000 us");
        close();view_.connection=config;view_.connection.mode=ConnectionMode::Hardware;view_.simulation=false;
        adapters();
#ifdef _WIN32
        if(!niceecatm_pcap_available())return error(view_.driver);
#endif
        context_=std::make_unique<ecx_contextt>();redundant_=std::make_unique<ecx_redportt>();
#ifndef _WIN32
        context_->port.sockhandle=-1;redundant_->sockhandle=-1;
#endif
        // Deliberately do not use ecx_init_redundant: SOEM returns only the
        // secondary setup result. Check each setup individually, initialize the
        // same dummy BRD frame, and close both sockets on either failure.
        initialized_=true;
        if(ecx_init(context_.get(),config.primary.c_str())<=0) {
            std::string message="Cannot open primary interface";
#ifdef _WIN32
            message+=std::string(": ")+niceecatm_pcap_error();
#else
            message+=std::string(": ")+std::strerror(errno)+" (requires raw socket and interface-management privileges)";
#endif
            close();return error(message);
        }
        if(config.redundant) {
            context_->port.redport=redundant_.get();
            if(ecx_setupnic(&context_->port,config.secondary.c_str(),TRUE)<=0) {
                std::string message="Cannot open secondary interface; both interfaces were closed";
#ifdef _WIN32
                message+=std::string(": ")+niceecatm_pcap_error();
#endif
                close();return error(message);
            }
            auto* header=reinterpret_cast<ec_etherheadert*>(&context_->port.txbuf2);
            header->sa1=oshw_htons(secMAC[0]);int zero=0;
            ecx_setupdatagram(&context_->port,&context_->port.txbuf2,EC_CMD_BRD,0,0,0,2,&zero);
            context_->port.txbuflength2=ETH_HEADERSIZE+EC_HEADERSIZE+EC_WKCSIZE+2;
        }
        const int found=ecx_config_init(context_.get());
        if(found<=0) {
            const char* reason="EC_ERROR: general configuration error";
            if(found==EC_NOFRAME)reason="EC_NOFRAME: no matching EtherCAT return frame was received";
            else if(found==EC_OTHERFRAME)reason="EC_OTHERFRAME: received traffic did not match the requested EtherCAT frame";
            else if(found==EC_SLAVECOUNTEXCEEDED)reason="EC_SLAVECOUNTEXCEEDED: slave count exceeds the compiled capacity";
            else if(found==EC_TIMEOUT)reason="EC_TIMEOUT: EtherCAT request timed out";
            else if(found==0)reason="WKC_ZERO: frame returned without any slave processing the datagram";
            std::string message=errors("SOEM scan result "+std::to_string(found)+" ("+reason+"). Interface opened successfully; check slave power, the slave IN port, cable and link negotiation");
#ifdef _WIN32
            message+="; pcap_last_error=";
            message+=niceecatm_pcap_last_error()[0]?niceecatm_pcap_last_error():"none";
#endif
            close();return error(message);
        }
        view_.connected=true;
        for(int s=1;s<=context_->slavecount;++s) {
            MailboxConfig config;std::string issue;
            if(readBootstrap(s,config,issue))bootstrapSlaves_.insert(s);
        }
        context_->manualstatechange=1; // Mapping must not silently request SAFE-OP.
        context_->overlappedMode=config.overlap?TRUE:FALSE;
        io_.assign(MapCapacity,0);
        const int mapperExtent=ecx_config_map_group(context_.get(),io_.data(),0);
        view_.mapped=validateMap(mapperExtent);
        const bool unsupportedOverlap=config.overlap && context_->grouplist[0].blockLRW;
        if(unsupportedOverlap)view_.mapped=false;
        requested_.assign(context_->slavecount+1,EC_STATE_PRE_OP);
        ecx_readstate(context_.get());updateSlaves();
        if(unsupportedOverlap)return error("Overlapped mapping with a slave that blocks LRW is unsupported in this native build; select normal mapping and scan again");
        if(!view_.mapped)return error(errors("Slaves discovered, but native mapping/transport extent could not be validated; cyclic exchange disabled"));
        view_.expectedWkc=context_->grouplist[0].outputsWKC*2+context_->grouplist[0].inputsWKC;
        if(!context_->grouplist[0].Obytes && !context_->grouplist[0].Ibytes && !context_->grouplist[0].mbxstatuslength)
            view_.expectedWkc=context_->slavecount; // read-only AL heartbeat WKC, no PDOs
        view_.wkc=0;view_.cycles=0;view_.errors=0;invalidCycles_=0;
        view_.actualCycleUs=0;view_.maxJitterUs=0;view_.missed=0;previousCycle_={};invalidSince_={};statePoll_=Clock::now();
        stateMonitor_.reset();
        // SOEM config_init requests PRE-OP with ACK set. Digital I/O ESCs
        // using device emulation mirror that ACK bit into AL Status. Finish
        // with plain PRE-OP and confirm the unmasked state, so real AL errors
        // remain visible and prevent a successful scan result.
        const bool preOpConfirmed=applyState(0,EC_STATE_PRE_OP,false);
        ecx_readstate(context_.get());updateSlaves();
        discoverPdoEntries();
        if(!preOpConfirmed)return error(errors("Scan did not finish in PRE-OP; "+stateFailure(0)));
        return done(errors("Scanned "+std::to_string(found)+" real EtherCAT slave(s); mapping prepared; cyclic exchange stopped"));
    }
    Result scanProgramming(const Connection& config,const EepromControl& control) override {
        if(view_.running)return error("Stop cyclic exchange before programming scan.");
        if(config.primary.empty()||config.primary.size()>=EC_MAXLEN_ADAPTERNAME)
            return error("Select a valid primary network interface for programming.");
        if(config.redundant||config.overlap)return error("Programming requires a single interface without PDO mapping.");
#ifndef _WIN32
        if(config.primary.size()>=IFNAMSIZ)return error("Linux network interface names must be shorter than IFNAMSIZ");
#endif
        close();view_.connection=config;view_.connection.mode=ConnectionMode::Programming;view_.simulation=false;
        view_.eeprom={EepromStage::Preparing,0,0,false,"Enumerating responding ESCs without PDO mapping"};
        if(control.progress)control.progress(view_.eeprom);
        const auto cancelled=[&]{return control.cancelled&&control.cancelled();};
        auto fail=[&](std::string message,bool wasCancelled=false){
            close();view_.eeprom={wasCancelled?EepromStage::Cancelled:EepromStage::Failed,0,0,false,message};
            if(control.progress)control.progress(view_.eeprom);
            return error(message);
        };
        if(cancelled())return fail("Programming scan cancelled.",true);
        adapters();
#ifdef _WIN32
        if(!niceecatm_pcap_available())return fail(view_.driver);
#endif
        context_=std::make_unique<ecx_contextt>();
#ifndef _WIN32
        context_->port.sockhandle=-1;
#endif
        initialized_=true;
        if(ecx_init(context_.get(),config.primary.c_str())<=0){
#ifdef _WIN32
            return fail(std::string("Cannot open programming interface: ")+niceecatm_pcap_error());
#else
            return fail(std::string("Cannot open programming interface: ")+std::strerror(errno));
#endif
        }
        uint16_t type=0;
        const int count=ecx_BRD(&context_->port,0,ECT_REG_TYPE,sizeof(type),&type,RegisterTimeoutUs);
        if(count<=0)return fail("Programming scan received no responding ESC; check the interface, slave power and cabling.");
        if(count>=EC_MAXSLAVE)return fail("Programming scan exceeds the compiled slave capacity.");
        // Deliberately no config_init, address assignment, DC, mailbox or PDO
        // configuration. AP positions work even when SII is completely blank.
        context_->slavecount=count;context_->manualstatechange=1;
        requested_.assign(size_t(count)+1,EC_STATE_INIT);view_.slaves.reserve(size_t(count));
        for(int position=1;position<=count;++position){
            if(cancelled())return fail("Programming scan cancelled.",true);
            const uint16_t ap=uint16_t(1-position);
            uint8_t identity[4]{};
            if(ecx_APRD(&context_->port,ap,ECT_REG_TYPE,sizeof(identity),identity,RegisterTimeoutUs)!=1)
                return fail("Programming ESC read failed at slave "+std::to_string(position)+".");
            // A slave may still carry an AL error from the normal master.
            // INIT|ACK is the standard SOEM transition used to clear it;
            // blank/invalid SII devices are still accepted below when they
            // report INIT with the error bit retained.
            const uint8_t request[2]{uint8_t(EC_STATE_INIT|EC_STATE_ACK),0};
            if(ecx_APWR(&context_->port,ap,ECT_REG_ALCTL,sizeof(request),const_cast<uint8_t*>(request),RegisterTimeoutUs)!=1)
                return fail("Programming INIT request failed at slave "+std::to_string(position)+".");
            // The ACK bit is a control command, not a stable AL state. A
            // follow-up plain INIT request clears ESCs that mirror that bit
            // in the status register while remaining in INIT.
            const uint8_t plainInit[2]{uint8_t(EC_STATE_INIT),0};
            if(ecx_APWR(&context_->port,ap,ECT_REG_ALCTL,sizeof(plainInit),const_cast<uint8_t*>(plainInit),RegisterTimeoutUs)!=1)
                return fail("Programming INIT state request failed at slave "+std::to_string(position)+".");
            const auto deadline=Clock::now()+StateTimeout;
            uint8_t actual[6]{};bool confirmed=false;
            do {
                if(cancelled())return fail("Programming scan cancelled.",true);
                if(ecx_APRD(&context_->port,ap,ECT_REG_ALSTAT,sizeof(actual),actual,RegisterTimeoutUs)!=1)
                    return fail("Cannot read actual AL state during programming scan at slave "+std::to_string(position)+".");
                if((littleWord(actual)&15)==EC_STATE_INIT){confirmed=true;break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }while(Clock::now()<deadline);
            if(!confirmed)return fail("Slave "+std::to_string(position)+" did not confirm INIT for programming.");
            Slave slave;slave.position=position;slave.escId=littleDword(identity);
            slave.state=littleWord(actual);slave.requested=EC_STATE_INIT;slave.alCode=littleWord(actual+4);
            slave.name="Unknown / blank SII";
            eeprom::Session session(eepromWire(position),cancelled);
            std::string issue;std::vector<uint8_t> header;
            const bool read=session.begin(issue)&&session.read(0,128,128,header,issue);
            std::string cleanup;
            if(!session.finish(cleanup))return fail("Slave "+std::to_string(position)+": "+cleanup);
            if(cancelled())return fail("Programming scan cancelled.",true);
            if(read&&validSiiHeader(header)){
                slave.vendor=littleDword(header.data()+16);slave.product=littleDword(header.data()+20);
                slave.revision=littleDword(header.data()+24);slave.serial=littleDword(header.data()+28);slave.eepromAlias=littleWord(header.data()+8);
                slave.eepromIdentityKnown=slave.vendor!=0&&slave.vendor!=0xFFFFFFFF&&slave.product!=0xFFFFFFFF;
                const size_t capacity=(size_t(littleWord(header.data()+124))+1)*128;
                slave.eepromSizeKnown=capacity<=eeprom::Session::MaxCapacity;
                slave.eepromCapacity=slave.eepromSizeKnown?uint32_t(capacity):0;
                if(slave.eepromIdentityKnown)slave.name="SII "+numberHex(slave.vendor,8)+" / "+numberHex(slave.product,8);
            }
            if(!read)diagnostics_.push_back({false,"Slave "+std::to_string(position)+": EEPROM inventory unavailable: "+issue,{}});
            auto& native=context_->slavelist[position];
            native.state=slave.state;native.ALstatuscode=slave.alCode;
            native.eep_man=slave.vendor;native.eep_id=slave.product;native.eep_rev=slave.revision;native.eep_ser=slave.serial;
            std::snprintf(native.name,sizeof(native.name),"%s",slave.name.c_str());
            view_.slaves.push_back(std::move(slave));
            view_.eeprom={EepromStage::Preparing,size_t(position),size_t(count),false,"Reading programming inventory"};
            if(control.progress)control.progress(view_.eeprom);
        }
        view_.connected=true;view_.mapped=false;view_.running=false;
        std::string issue;
        if(!programmingTopology(1,issue))return fail(issue);
        view_.eeprom={EepromStage::Idle,0,0,false,"Programming inventory ready; INIT confirmed"};
        if(control.progress)control.progress(view_.eeprom);
        return done("Programming scan complete: "+std::to_string(count)+" slave(s), INIT confirmed, no PDO mapping.");
    }
    Result readEeprom(int slave,size_t bytes,const EepromControl& control) override {
        return eepromOperation(slave,bytes,nullptr,control);
    }
    Result writeEeprom(int slave,const std::vector<uint8_t>& image,const EepromControl& control) override {
        return eepromOperation(slave,image.size(),&image,control);
    }
    Result foeTransfer(int slave,FoeDirection direction,const FoeOptions& options,const FoeControl& control) override {
        if(view_.connection.mode!=ConnectionMode::Hardware)return error("FoE is unavailable in Programming mode.");
        if(!validSlave(slave))return error("Select a connected slave for FoE.");
        if(view_.running)return error("Stop cyclic I/O before FoE transfer.");
        for(int s=1;s<=context_->slavecount;++s) {
            if(!readActualState(s)){updateSlaves();return error("FoE cannot confirm a live AL state for slave "+std::to_string(s)+".");}
            if((context_->slavelist[s].state&15)==EC_STATE_OPERATIONAL)return error("Move all OP slaves to PRE-OP before FoE transfer.");
        }
        updateSlaves();
        auto& native=context_->slavelist[slave];
        if(native.state!=EC_STATE_PRE_OP&&native.state!=EC_STATE_BOOT)return error("FoE requires confirmed PRE-OP or BOOT without AL error.");
        if(native.state==EC_STATE_PRE_OP&&!(native.mbx_proto&ECT_MBXPROT_FOE))return error("Slave does not declare FoE support in the normal mailbox.");
        if(native.state==EC_STATE_BOOT&&!mailboxSessions_.count(slave))return error("Establish BOOT through the state toolbar before FoE transfer.");
        if(native.mbx_l<=12||native.mbx_l>EC_MAXMBX||native.mbx_rl<=12||native.mbx_rl>EC_MAXMBX)return error("FoE mailbox sizes are invalid.");
        // A stopped master cannot service SOEM's queued cyclic mailbox path.
        // Use its synchronous link-layer mailbox transport with short waits.
        struct HandlerGuard {ec_slavet& slave;int saved;~HandlerGuard(){slave.mbxhandlerstate=saved;}} guard{native,native.mbxhandlerstate};
        native.mbxhandlerstate=0;
        bool drained=false;
        for(unsigned attempt=0;attempt<16;++attempt) {
            if(control.cancelled&&control.cancelled())return error("FoE cancelled before mailbox access.");
            ec_mbxbuft* old=nullptr;const int wkc=ecx_mbxreceive(context_.get(),uint16_t(slave),&old,0);
            if(old)ecx_dropmbx(context_.get(),old);
            if(wkc<=0){drained=true;break;}
        }
        if(!drained)return error("Slave mailbox did not become empty before the FoE request.");
        foe::Wire wire;wire.txPayload=native.mbx_l-12;wire.rxPayload=native.mbx_rl-12;
        wire.send=[this,slave](const foe::Packet& packet,int timeoutMs) {
            auto& target=context_->slavelist[slave];
            if(packet.data.size()>size_t(target.mbx_l)-12)return false;
            ec_mbxbuft* message=ecx_getmbx(context_.get());if(!message)return false;
            ec_clearmbx(message);auto* bytes=reinterpret_cast<uint8_t*>(message);
            const auto length=uint16_t(6+packet.data.size());bytes[0]=uint8_t(length);bytes[1]=uint8_t(length>>8);
            const uint8_t counter=ec_nextmbxcnt(target.mbx_cnt);target.mbx_cnt=counter;
            bytes[5]=uint8_t(ECT_MBXT_FOE|MBX_HDR_SET_CNT(counter));bytes[6]=uint8_t(packet.opcode);
            for(unsigned i=0;i<4;++i)bytes[8+i]=uint8_t(packet.number>>(i*8));
            std::copy(packet.data.begin(),packet.data.end(),bytes+12);
            // ecx_mbxsend takes ownership of message on every path.
            return ecx_mbxsend(context_.get(),uint16_t(slave),message,timeoutMs*1000)>0;
        };
        wire.receive=[this,slave](foe::Packet& packet,int timeoutMs) {
            ec_mbxbuft* message=nullptr;
            const int wkc=ecx_mbxreceive(context_.get(),uint16_t(slave),&message,timeoutMs*1000);
            if(wkc<=0||!message){if(message)ecx_dropmbx(context_.get(),message);return 0;}
            const auto* bytes=reinterpret_cast<const uint8_t*>(message);
            const size_t length=littleWord(bytes);
            if((bytes[5]&15)!=ECT_MBXT_FOE){ecx_dropmbx(context_.get(),message);return 0;}
            if(length<6||length+6>context_->slavelist[slave].mbx_rl){ecx_dropmbx(context_.get(),message);return -1;}
            packet.opcode=static_cast<foe::Opcode>(bytes[6]);packet.number=littleDword(bytes+8);
            packet.data.assign(bytes+12,bytes+6+length);ecx_dropmbx(context_.get(),message);return 1;
        };
        FoeControl relay=control;relay.progress=[&](const FoeProgress& progress){view_.foe=progress;if(control.progress)control.progress(progress);};
        auto result=foe::run(std::move(wire),slave,direction,options,relay);
        view_.status=result.message;return result;
    }
    Result setCycle(int cycleUs) override {
        if(view_.connection.mode==ConnectionMode::Programming)return error("Cycle configuration is unavailable in Programming mode.");
        if(!validCycle(cycleUs))return error("Target cycle must be 500, 1000, 2000, 5000, 10000, 20000, 50000 or 100000 us");
        if(!view_.connected || !context_)return error("Scan devices first");
        if(view_.running)return error("Stop cyclic exchange before changing the target cycle");
        ecx_readstate(context_.get());updateSlaves();
        for(const auto& slave:view_.slaves) {
            const auto state=slave.state&15;
            if(state!=EC_STATE_INIT && state!=EC_STATE_PRE_OP)
                return error("Target cycle can be changed only while all slaves are INIT or PRE-OP");
        }
        view_.connection.cycleUs=cycleUs;
        return done("Target cycle updated while the bus is stopped");
    }
    Result state(int slave,AlState desired) override {
        if(view_.connection.mode==ConnectionMode::Programming)return error("Ordinary state control is unavailable in Programming mode.");
        if(!view_.connected || !context_ || slave<0 || slave>context_->slavecount)return error("No connected slave at the requested position");
        const auto requested=static_cast<uint16_t>(desired);
        if(requested!=1 && requested!=2 && requested!=3 && requested!=4 && requested!=8)return error("Unsupported EtherCAT state request");
        if(requested==3)return enterBoot(slave);
        if(view_.running && (requested==1 || requested==2))return error("Stop cyclic exchange before requesting INIT or PRE-OP");
        if(requested>=4 && !view_.mapped)return error("Mapping is invalid; scan again before requesting SAFE-OP or OP");
        stateMonitor_.reset();statePoll_=Clock::now();
        for(int s=slave?slave:1;s<=(slave?slave:context_->slavecount);++s)requested_[s]=requested;
        // Advance through the standard intermediate states instead of asking an
        // INIT slave to make an illegal jump directly into OP.
        ecx_readstate(context_.get());
        bool ok=true;
        for(int s=slave?slave:1;s<=(slave?slave:context_->slavecount);++s) {
            if(mailboxSessions_.count(s)&&!restoreMailbox(s)) {
                ecx_readstate(context_.get());updateSlaves();
                return error("Could not restore normal mailbox through INIT for slave "+std::to_string(s)+". "+stateFailure(s));
            }
        }
        if(requested>=4) {
            for(int s=slave?slave:1;s<=(slave?slave:context_->slavecount);++s) {
                auto actual=context_->slavelist[s].state&15;
                if(actual==1)ok=applyState(s,2,false)&&ok;
            }
        }
        if(requested==8) {
            for(int s=slave?slave:1;s<=(slave?slave:context_->slavecount);++s)
                if((context_->slavelist[s].state&15)<4)ok=applyState(s,4,true)&&ok;
        }
        if(ok)ok=applyState(slave,requested,requested>=4 || view_.running);
        ecx_readstate(context_.get());updateSlaves(view_.wkc>=view_.expectedWkc);
        if(requested>=4 && (ok || hasOp())) {view_.running=true;previousCycle_={};invalidSince_={};invalidCycles_=0;}
        if(!ok)return error(errors(stateFailure(slave)));
        return done(std::string("Confirmed ")+stateName(requested)+" for "+(slave?"slave "+std::to_string(slave):"all slaves"));
    }
    Result start() override {
        if(view_.connection.mode==ConnectionMode::Programming)return error("Cyclic exchange is unavailable in Programming mode.");
        if(!view_.connected || !view_.mapped)return error("Scan and validate process mapping before starting");
        stateMonitor_.reset();statePoll_=Clock::now();
        ecx_readstate(context_.get());updateSlaves();
        for(const auto& slave:view_.slaves)if(slave.state!=4 && slave.state!=8)return error("All slaves must be SAFE-OP or OP before starting cyclic exchange");
        view_.running=true;previousCycle_={};invalidSince_={};invalidCycles_=0;
        return done("Cyclic exchange started; actual AL states remain independent");
    }
    Result stop() override {
        if(view_.connection.mode==ConnectionMode::Programming)return done("Programming mode has no cyclic exchange.");
        if(!view_.connected)return done("Cyclic exchange stopped");
        stateMonitor_.reset();
        ecx_readstate(context_.get());bool ok=true;
        for(int s=1;s<=context_->slavecount;++s)if((context_->slavelist[s].state&15)==8) {
            if(size_t(s)<requested_.size())requested_[s]=4;
            ok=applyState(s,4,view_.mapped)&&ok;
        }
        view_.running=false;ecx_readstate(context_.get());updateSlaves();
        for(const auto& slave:view_.slaves)if(slave.state!=1 && slave.state!=2 && slave.state!=4)ok=false;
        if(!ok)return error(errors("Exchange stopped; SAFE-OP confirmation failed. "+stateFailure(0)));
        return done("Cyclic exchange stopped; OP slaves confirmed SAFE-OP");
    }
    Result readRegister(int slave,uint16_t offset,int length) override {
        if(view_.connection.mode==ConnectionMode::Programming)return error("Ordinary register access is unavailable in Programming mode.");
        if(!validSlave(slave))return error("Select a connected slave");
        if(length<1 || length>256 || uint32_t(offset)+uint32_t(length)>65536)return error("Register read must be 1..256 bytes within ESC address space");
        if(view_.running)return error("Stop cyclic exchange before synchronous ESC access in this build");
        std::vector<uint8_t> bytes(size_t(length),0);
        if(ecx_FPRD(&context_->port,context_->slavelist[slave].configadr,offset,uint16(length),bytes.data(),RegisterTimeoutUs)<=0)
            return error(errors("ESC read timed out: no slave working counter"));
        view_.status="ESC read completed";return {true,view_.status,std::move(bytes)};
    }
    Result writeRegister(int slave,uint16_t offset,const std::vector<uint8_t>& bytes) override {
        if(view_.connection.mode==ConnectionMode::Programming)return error("Ordinary register access is unavailable in Programming mode.");
        if(!validSlave(slave))return error("Select a connected slave");
        if(bytes.empty() || bytes.size()>256 || size_t(offset)+bytes.size()>65536)return error("Invalid register write length");
        if(view_.running)return error("Stop cyclic exchange before synchronous ESC access in this build");
        const size_t end=size_t(offset)+bytes.size();
        if(offset==ECT_REG_ALCTL && bytes.size()==2) {
            const uint16_t value=uint16_t(bytes[0])|(uint16_t(bytes[1])<<8);
            if(value!=1 && value!=2 && value!=3 && value!=4 && value!=8)return error("Use a regular BOOT/INIT/PRE-OP/SAFE-OP/OP value for AL Control");
            return state(slave,static_cast<AlState>(value));
        }
        const bool mapping=(offset>=0x0600 && end<=0x0640)||(offset>=0x0800 && end<=0x0840);
        const bool watchdog=(offset>=0x0400 && end<=0x0402)||(offset>=0x0410 && end<=0x0414);
        if(!mapping && !watchdog)return error("Register is read-only or its write permissions are unknown; writable ranges: AL Control, FMMU, SM and watchdog timers");
        ecx_readstate(context_.get());updateSlaves();
        if((context_->slavelist[slave].state&15)!=1 && (context_->slavelist[slave].state&15)!=2)
            return error("Configuration register writes require INIT or PRE-OP");
        // A timed-out write may still have reached the slave. Invalidate first,
        // never retain an apparently valid process map after an uncertain write.
        if(mapping)invalidateMapping();
        if(ecx_FPWR(&context_->port,context_->slavelist[slave].configadr,offset,uint16(bytes.size()),const_cast<uint8_t*>(bytes.data()),RegisterTimeoutUs)<=0)
            return error(errors("ESC write timed out; actual result is unknown"));
        if(mapping) {view_.mapped=false;return done("ESC configuration written; process mapping invalidated. Scan again to rebuild");}
        return done("ESC register write acknowledged");
    }
    Result readSdoDictionary(int slave) override {
        if(view_.connection.mode==ConnectionMode::Programming)return error("SDO access is unavailable in Programming mode.");
        if(!validSlave(slave))return error("Select a connected slave");
        if(view_.running)return error("Stop cyclic exchange before reading the SDO dictionary.");
        auto& native=context_->slavelist[slave];
        ec_alstatust actual{};
        if(ecx_FPRD(&context_->port,native.configadr,ECT_REG_ALSTAT,sizeof(actual),&actual,RegisterTimeoutUs)!=1)
            return error("Cannot confirm the selected slave's AL state for dictionary discovery.");
        native.state=etohs(actual.alstatus);native.ALstatuscode=etohs(actual.alstatuscode);updateSlaves();
        if(!(native.mbx_proto&ECT_MBXPROT_COE))return error("Slave does not declare CoE support");
        if(native.state!=EC_STATE_PRE_OP)return error("Online SDO dictionary discovery requires confirmed PRE-OP.");
        if(!(native.CoEdetails&ECT_COEDET_SDOINFO))return error("Slave does not declare SDO Information support.");
        const auto dictionary=sdo_discovery::read(context_.get(),uint16_t(slave));
        if(!dictionary->complete&&dictionary->objects.empty()&&view_.slaves[size_t(slave-1)].sdoDictionary)
            return error(dictionary->status);
        view_.slaves[size_t(slave-1)].sdoDictionary=dictionary;
        return dictionary->complete?done(dictionary->status):error(dictionary->status);
    }
    Result readSdo(int slave,uint16_t index,uint8_t sub,int capacity) override {
        if(view_.connection.mode==ConnectionMode::Programming)return error("SDO access is unavailable in Programming mode.");
        if(!validSlave(slave))return error("Select a connected slave");
        if(view_.running)return error("Stop cyclic exchange before synchronous SDO access in this build");
        if(capacity<1 || capacity>65536)return error("SDO receive capacity must be 1..65536 bytes");
        ecx_readstate(context_.get());updateSlaves();
        if((context_->slavelist[slave].mbx_proto & ECT_MBXPROT_COE)==0)return error("Slave does not declare CoE support");
        const auto actual=context_->slavelist[slave].state;
        if(actual!=2 && actual!=4 && actual!=8)return error("SDO requires confirmed PRE-OP, SAFE-OP or OP");
        std::vector<uint8_t> data(size_t(capacity),0);int size=capacity;
        if(ecx_SDOread(context_.get(),uint16(slave),index,sub,FALSE,&size,data.data(),ServiceTimeoutUs)<=0)
            return error(errors("SDO read failed or timed out"));
        if(size<0 || size>capacity)return error("SDO response length exceeds the requested buffer");
        data.resize(size_t(size));view_.status="SDO read completed: "+numberHex(index)+":"+numberHex(sub,2);
        return {true,view_.status,std::move(data)};
    }
    Result writeSdo(int slave,uint16_t index,uint8_t sub,const std::vector<uint8_t>& data) override {
        if(view_.connection.mode==ConnectionMode::Programming)return error("SDO access is unavailable in Programming mode.");
        if(!validSlave(slave))return error("Select a connected slave");
        if(view_.running)return error("Stop cyclic exchange before synchronous SDO access in this build");
        if(data.empty() || data.size()>65536)return error("SDO write must contain 1..65536 bytes");
        ecx_readstate(context_.get());updateSlaves();
        if((context_->slavelist[slave].mbx_proto & ECT_MBXPROT_COE)==0)return error("Slave does not declare CoE support");
        const auto actual=context_->slavelist[slave].state;
        if(actual!=2 && actual!=4 && actual!=8)return error("SDO requires confirmed PRE-OP, SAFE-OP or OP");
        if((index>=0x1600 && index<=0x1bff)||(index>=0x1c00 && index<=0x1c2f))invalidateMapping();
        if(ecx_SDOwrite(context_.get(),uint16(slave),index,sub,FALSE,int(data.size()),const_cast<uint8_t*>(data.data()),ServiceTimeoutUs)<=0)
            return error(errors("SDO write failed or timed out; actual write result may be unknown"));
        return done(view_.mapped?"SDO write acknowledged":"SDO mapping object written; scan again to rebuild process mapping");
    }
    Result writeOutput(int slave,int offset,const std::vector<uint8_t>& data) override {
        if(view_.connection.mode==ConnectionMode::Programming)return error("PDO access is unavailable in Programming mode.");
        if(!validSlave(slave))return error("Select a connected slave");
        if(!view_.mapped)return error("Process mapping is invalid; scan again");
        const auto& target=context_->slavelist[slave];
        const size_t size=(size_t(target.Obits)+7)/8;
        if(offset<0 || data.empty() || size_t(offset)>size || data.size()>size-size_t(offset))return error("Output write is outside the slave's process image");
        if(target.state!=2 && target.state!=4 && target.state!=8)return error("Output writes require confirmed PRE-OP, SAFE-OP or OP");
        if(!inMap(target.outputs,(size_t(target.Obits)+target.Ostartbit+7)/8))return error("Output pointer is outside the validated mapping");
        // UI/commands use compact per-slave images starting at bit zero. The
        // native map may have a leading start bit; preserve neighboring bits.
        std::vector<uint8_t> candidate(size,0);
        for(unsigned bit=0;bit<target.Obits;++bit) {
            const unsigned source=bit+target.Ostartbit;
            if(target.outputs[source/8]&(1u<<(source%8)))candidate[bit/8]|=uint8_t(1u<<(bit%8));
        }
        std::copy(data.begin(),data.end(),candidate.begin()+offset);
        for(unsigned bit=0;bit<target.Obits;++bit) {
            const unsigned destination=bit+target.Ostartbit;
            const uint8_t mask=uint8_t(1u<<(destination%8));
            if(candidate[bit/8]&(1u<<(bit%8)))target.outputs[destination/8]|=mask;
            else target.outputs[destination/8]&=uint8_t(~mask);
        }
        updateSlaves();
        return done(target.state==8 && view_.running?"Output image committed for the next valid cycle":"Local output image preloaded; not applied through an OP cycle");
    }
    void cycle() override {
        if(!view_.running || !view_.mapped || !initialized_)return;
        const auto now=Clock::now();
        if(previousCycle_!=Clock::time_point{}) {
            view_.actualCycleUs=std::chrono::duration<double,std::micro>(now-previousCycle_).count();
            const double jitter=std::abs(view_.actualCycleUs-view_.connection.cycleUs);
            view_.maxJitterUs=std::max(view_.maxJitterUs,jitter);
            if(view_.actualCycleUs>view_.connection.cycleUs*1.5)++view_.missed;
        }
        previousCycle_=now;exchange();++view_.cycles;
        const bool valid=view_.expectedWkc?view_.wkc>=view_.expectedWkc:view_.wkc>=0;
        if(!valid) {
            ++view_.errors;++invalidCycles_;
            if(invalidSince_==Clock::time_point{})invalidSince_=now;
            view_.status="Process inputs stale: working counter below expected";
        }
        else {
            invalidCycles_=0;invalidSince_={};
            const auto& group=context_->grouplist[0];
            view_.status=(!group.Obytes && !group.Ibytes && !group.mbxstatuslength)?
                "No PDOs mapped; read-only AL-status heartbeat running":
                "Cyclic exchange running; received working counter verified";
        }
        // SOEM may overwrite native states with NONE after a timed-out AL
        // read, even while PDOs are valid. Keep that monitoring failure separate
        // from a confirmed state change, with a bounded per-slave grace period.
        const auto monitoredAt=Clock::now();
        if(monitoredAt-statePoll_>=std::chrono::seconds(1)) {
            stateMonitor_.read(*context_,valid,monitoredAt);statePoll_=monitoredAt;
        }
        stateMonitor_.expire(*context_,valid,Clock::now());
        for(const auto& event:stateMonitor_.takeEvents()) {
            std::ostringstream message;
            const auto& slave=context_->slavelist[event.slave];
            const bool recovered=event.kind==CyclicStatePoll::EventKind::Recovered;
            if(recovered)message<<"AL status read recovered for slave "<<event.slave<<"; actual AL="<<stateName(slave.state)<<" ("<<numberHex(slave.state)<<"), AL code="<<numberHex(slave.ALstatuscode);
            else if(event.kind==CyclicStatePoll::EventKind::Unavailable)
                message<<"Cached AL state expired or was invalidated for slave "<<event.slave<<"; output writes require a fresh AL state";
            else message<<"AL status read failed for slave "<<event.slave<<" (NONE/0x0000); previous AL="<<stateName(event.previousState)<<" ("<<numberHex(event.previousState)<<"); retain the previous state for at most 2 s only while PDO WKC is valid";
            message<<"; PDO WKC="<<view_.wkc<<'/'<<view_.expectedWkc<<"; AL read failures="<<stateMonitor_.failures();
            diagnostics_.push_back({recovered,message.str(),{}});
        }
        if(stateMonitor_.pending()&&valid)
            view_.status="AL status read incomplete; output writes require a recent confirmed AL state (2 s maximum grace)";
        updateSlaves(valid);
        if(invalidSince_!=Clock::time_point{} && now-invalidSince_>=WkcFailureGrace) {
            view_.running=false;
            view_.status="Process data failed continuously for 2 s; exchange stopped. Inspect state and explicitly restart";
        }
    }
    bool hasPendingPdoDescriptions() const override {
        if(!initialized_||!view_.connected||!view_.mapped||view_.running||
           view_.connection.mode!=ConnectionMode::Hardware||pdoDescriptions_.empty())return false;
        return std::all_of(view_.slaves.begin(),view_.slaves.end(),
                           [](const Slave& slave){return slave.state==EC_STATE_PRE_OP;});
    }
    bool servicePdoDescriptions() override {
        if(!hasPendingPdoDescriptions())return false;
        auto pending=pdoDescriptions_.begin();
        auto& slave=view_.slaves[size_t(pending->first-1)];
        const bool changed=advancePdoDescriptions(slave,pending->second);
        if(!pending->second.pending())pdoDescriptions_.erase(pending);
        return changed;
    }
    void close() override {
        // Destructor/disconnect does not send any unrequested output value. If
        // OP is active, request SAFE-OP before closing the raw interfaces.
        if(initialized_ && context_) {
            if(view_.connected && hasOp())stop();
#ifdef _WIN32
            const bool primaryFailed=context_->port.sockhandle==nullptr;
#endif
            ecx_close(context_.get());
#ifdef _WIN32
            // SOEM initializes these before pcap_open but destroys them only
            // when the handle is non-null; balance the failed-open path too.
            if(primaryFailed) {DeleteCriticalSection(&context_->port.getindex_mutex);DeleteCriticalSection(&context_->port.tx_mutex);DeleteCriticalSection(&context_->port.rx_mutex);}
#else
            pthread_mutex_destroy(&context_->port.getindex_mutex);
            pthread_mutex_destroy(&context_->port.tx_mutex);
            pthread_mutex_destroy(&context_->port.rx_mutex);
#endif
        }
        initialized_=false;context_.reset();redundant_.reset();io_.clear();requested_.clear();
        pdoDescriptions_.clear();pdoObjects_.reset();pdoObjectEntries_.reset();
        bootstrapSlaves_.clear();mailboxSessions_.clear();
        stateMonitor_.reset();statePoll_={};diagnostics_.clear();
        view_.connected=false;view_.mapped=false;view_.running=false;view_.slaves.clear();view_.wkc=0;view_.expectedWkc=0;invalidSince_={};invalidCycles_=0;
        view_.status="Disconnected";
    }
    Snapshot snapshot() const override {return view_;}
    std::vector<Result> takeDiagnostics() override {std::vector<Result> result;result.swap(diagnostics_);return result;}
};
} // namespace
std::unique_ptr<Backend> makeSoem() {return std::make_unique<SoemBackend>();}
} // namespace niceecatm


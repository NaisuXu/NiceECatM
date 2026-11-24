// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "ethercat/backend.hpp"
#include "ethercat/foe_job.hpp"
#include "ethercat/trend_accumulator.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <utility>

namespace niceecatm {
namespace {
bool validCycle(int cycleUs) {
    return cycleUs==500||cycleUs==1000||cycleUs==2000||cycleUs==5000||cycleUs==10000||cycleUs==20000||cycleUs==50000||cycleUs==100000;
}
class Simulation final : public Backend {
    Snapshot view_;
    std::map<int,std::array<uint8_t,4096>> registers_;
    std::map<std::string,std::vector<uint8_t>> files_;
    uint16_t gain_=1000;
    TrendAccumulator trends_;
    Result success(std::string message,std::vector<uint8_t> data={}) {view_.status=message;return {true,std::move(message),std::move(data)};}
    Result failure(std::string message) {view_.status=message;return {false,std::move(message),{}};}
    Slave* find(int position) {
        auto it=std::find_if(view_.slaves.begin(),view_.slaves.end(),[position](const Slave& s){return s.position==position;});
        return it==view_.slaves.end()?nullptr:&*it;
    }
    bool serviceAllowed() const {return view_.connected&&!view_.running;}
    bool mailboxAllowed(const Slave& s) const {return (s.state&15)==2||(s.state&15)==4||(s.state&15)==8;}
public:
    Simulation() {
        view_.simulation=true;view_.connection.mode=ConnectionMode::Simulation;view_.driver="Built-in simulator / no hardware access";adapters();
        auto& firmware=files_["firmware.bin"];firmware.resize(4096);
        for(size_t i=0;i<firmware.size();++i)firmware[i]=uint8_t((i*17+3)&255);
        const std::string text="NiceECatM simulated FoE file. No hardware was accessed.\n";
        files_["demo.bin"]={text.begin(),text.end()};
    }
    Result adapters() override {
        view_.adapters={{"sim-primary","Simulated adapter A"},{"sim-secondary","Simulated adapter B"}};
        return success("Two simulated adapters available.");
    }
    Result scan(const Connection& connection) override {
        close();view_.connection=connection;view_.connection.mode=ConnectionMode::Simulation;
        if(connection.redundant&&(connection.primary.empty()||connection.secondary.empty()||connection.primary==connection.secondary))return failure("Redundancy requires two different adapters.");
        if(!validCycle(connection.cycleUs))return failure("Unsupported target cycle.");
        const std::array<const char*,5> names={"EK1100","EL1008","EL2008","EL3062","DEMO-PDO"};
        for(int i=1;i<=5;++i) {
            Slave slave;slave.position=i;slave.name=names[i-1];slave.address=uint16_t(0x1000+i);
            slave.vendor=i==5?0xEC01:2;slave.product=uint32_t(i*1000);slave.revision=1;slave.serial=uint32_t(20261100+i);
            slave.state=slave.requested=2;slave.dc=i>=4;slave.mailboxProtocols=i==4?0x003e:0;
            slave.sdoInfo=i==4;slave.bootstrapMailbox=i==4;
            if(i==2) {slave.inputs={0x2d};slave.inputBits=8;slave.inputOffset=20;}
            if(i==3) {slave.outputs={0};slave.outputBits=8;slave.outputOffset=0;}
            if(i==4) {slave.inputs={0,0x00,0x10,0x00,0x08};slave.inputBits=40;slave.inputOffset=21;}
            if(i==5) {slave.outputs.resize(19);slave.outputBits=152;slave.outputOffset=1;}
            // The simulator exposes the same stable names as the UI fixtures so
            // scripts can be developed without requiring hardware or an ESI file.
            const auto addEntry=[&](bool input,unsigned bit,unsigned width,const char* type,
                                    const char* name,uint16_t index,uint8_t sub) {
                PdoEntry entry;entry.input=input;entry.bitOffset=bit;entry.bitLength=width;
                entry.type=type;entry.index=index;entry.subindex=sub;entry.source="Built-in simulator";
                entry.names={{0x0409,name}};slave.pdoEntries.push_back(std::move(entry));
            };
            if(i==2) {
                slave.inputLayoutKnown=true;
                const char* names[]={"Start button","Stop button","Position sensor","Part detection",
                    "Door feedback","Air pressure","Spare input 7","Spare input 8"};
                for(unsigned bit=0;bit<8;++bit)addEntry(true,bit,1,"BOOL",names[bit],uint16_t(0x6000+bit*0x10),1);
            } else if(i==3) {
                slave.outputLayoutKnown=true;
                const char* names[]={"Run indicator","Alarm indicator","Solenoid A","Solenoid B",
                    "Spare output 5","Spare output 6","Spare output 7","Spare output 8"};
                for(unsigned bit=0;bit<8;++bit)addEntry(false,bit,1,"BOOL",names[bit],uint16_t(0x7000+bit*0x10),1);
            } else if(i==4) {
                slave.inputLayoutKnown=true;
                addEntry(true,0,8,"UINT8","Input status",0x6000,1);
                addEntry(true,8,16,"INT16","Analog input / Channel 1",0x6000,0x11);
                addEntry(true,24,16,"INT16","Analog input / Channel 2",0x6010,0x11);
            } else if(i==5) {
                slave.outputLayoutKnown=true;
                const char* names[]={"Speed setpoint","Current limit","Target position","Cycle counter",
                    "Voltage setpoint","Operating mode","Control bits","12-bit setpoint","Enable flag"};
                const char* types[]={"INT16","UINT16","INT32","UINT32","REAL32","UINT8","UINT3","UINT12","BOOL"};
                const unsigned bits[]={0,16,32,64,96,128,136,139,151};
                for(unsigned n=0;n<9;++n)addEntry(false,bits[n],n==4?32:n==2||n==3?32:n==7?12:n==8?1:n==0||n==1?16:8,
                    types[n],names[n],uint16_t(0x7100),uint8_t(n+1));
            }
            view_.slaves.push_back(slave);
            auto& registers=registers_[i];registers.fill(0);registers[0]=0x11;registers[1]=0x01;
            registers[0x10]=uint8_t(slave.address);registers[0x11]=uint8_t(slave.address>>8);
            registers[0x120]=registers[0x130]=2;
        }
        view_.connected=true;view_.mapped=true;view_.expectedWkc=6;view_.wkc=0;view_.cycles=0;
        view_.actualCycleUs=0;view_.maxJitterUs=0;view_.missed=0;view_.errors=0;gain_=1000;
        return success("Simulation scan complete: 5 slaves, PRE-OP, IN 6 B / OUT 20 B. No real hardware was accessed.");
    }
    Result setCycle(int cycleUs) override {
        if(!validCycle(cycleUs))return failure("Unsupported target cycle.");
        if(!view_.connected)return failure("Scan devices first.");
        if(view_.running)return failure("Stop cyclic exchange before changing the target cycle.");
        for(const auto& slave:view_.slaves)if((slave.state&15)!=1&&(slave.state&15)!=2)
            return failure("Target cycle can be changed only while all slaves are INIT or PRE-OP.");
        view_.connection.cycleUs=cycleUs;
        trends_.setCycle(cycleUs);
        return success("Target cycle updated while the bus is stopped.");
    }
    Result state(int position,AlState requested) override {
        if(!view_.connected)return failure("Scan devices first.");
        if(position!=0&&!find(position))return failure("Slave does not exist.");
        if(requested!=AlState::Boot&&requested!=AlState::Init&&requested!=AlState::PreOp&&requested!=AlState::SafeOp&&requested!=AlState::Op)return failure("Unsupported AL state.");
        if((requested==AlState::SafeOp||requested==AlState::Op)&&!view_.mapped)return failure("Mapping is invalid; scan devices again.");
        if(view_.running&&(requested==AlState::Boot||requested==AlState::Init||requested==AlState::PreOp))return failure("Stop cyclic exchange before lowering state.");
        if(requested==AlState::Boot) {
            // Preflight the complete scope before changing even one target.
            // The other virtual slaves deliberately have no bootstrap mailbox.
            for(const auto& slave:view_.slaves) {
                if((slave.state&15)==8)return failure("Stop all OP slaves before entering BOOT.");
                if((position==0||slave.position==position)&&slave.position!=4)
                    return failure("Slave "+std::to_string(slave.position)+" has no valid bootstrap mailbox; BOOT was not requested.");
            }
        }
        for(auto& s:view_.slaves)if(position==0||s.position==position) {
            if(requested==AlState::Boot||(s.state&15)==3) {
                // Real BOOT entry/exit passes through INIT and changes mailbox
                // configuration; keep the simulated ESC register view aligned.
                s.requested=s.state=1;registers_[s.position][0x120]=registers_[s.position][0x130]=1;
            }
            s.requested=s.state=static_cast<uint16_t>(requested);s.alCode=0;
            registers_[s.position][0x120]=uint8_t(s.requested);registers_[s.position][0x130]=uint8_t(s.state);
        }
        if(requested==AlState::Boot)view_.mapped=false;
        if(requested==AlState::SafeOp||requested==AlState::Op) {
            if(!view_.running)trends_.pause();
            view_.running=true;
        }
        return success(std::string("Simulated AL state confirmed: ")+stateName(static_cast<uint16_t>(requested))+".");
    }
    Result start() override {
        if(!view_.connected||!view_.mapped)return failure("Scan and map devices first.");
        if(!std::all_of(view_.slaves.begin(),view_.slaves.end(),[](const Slave& s){return (s.state&15)==4||(s.state&15)==8;}))return failure("All slaves must be SAFE-OP or OP before cyclic exchange.");
        trends_.pause();
        view_.running=true;return success("Simulated cyclic exchange started.");
    }
    Result stop() override {
        trends_.pause();
        view_.running=false;view_.wkc=0;
        for(auto& s:view_.slaves)if((s.state&15)==8) {
            s.state=s.requested=4;registers_[s.position][0x120]=registers_[s.position][0x130]=4;
        }
        return success("Simulated cyclic exchange stopped; OP slaves are SAFE-OP.");
    }
    Result readRegister(int position,uint16_t offset,int length) override {
        if(!serviceAllowed())return failure("Stop cyclic exchange before register access.");
        auto* s=find(position);if(!s)return failure("Slave does not exist.");
        if(length<1||length>256||unsigned(offset)+unsigned(length)>4096)return failure("Register range must be 1–256 bytes within 0x0000–0x0FFF.");
        auto& regs=registers_[position];regs[0x130]=uint8_t(s->state);regs[0x134]=uint8_t(s->alCode);regs[0x135]=uint8_t(s->alCode>>8);
        return success("Simulated register read complete.",std::vector<uint8_t>(regs.begin()+offset,regs.begin()+offset+length));
    }
    Result writeRegister(int position,uint16_t offset,const std::vector<uint8_t>& bytes) override {
        if(!serviceAllowed())return failure("Stop cyclic exchange before register access.");
        auto* s=find(position);if(!s)return failure("Slave does not exist.");
        if(offset==0x120&&bytes.size()==2) {
            const uint16_t requested=uint16_t(bytes[0]|unsigned(bytes[1])<<8);
            if(requested!=1&&requested!=2&&requested!=3&&requested!=4&&requested!=8)return failure("AL Control requires BOOT, INIT, PRE-OP, SAFE-OP or OP.");
            return state(position,static_cast<AlState>(requested));
        }
        if(bytes.empty()||bytes.size()>256||offset<0x800||size_t(offset)+bytes.size()>0x900)return failure("Writable simulated registers: AL Control (0x0120, 2 B) and SM configuration (0x0800–0x08FF).");
        if((s->state&15)!=1&&(s->state&15)!=2)return failure("SM configuration is writable in INIT / PRE-OP only.");
        auto& regs=registers_[position];std::copy(bytes.begin(),bytes.end(),regs.begin()+offset);view_.mapped=false;
        return success("Simulated SM write complete; mapping invalidated. Rescan before process exchange.");
    }
    Result readSdoDictionary(int position) override {
        if(!serviceAllowed())return failure("Stop cyclic exchange before reading the SDO dictionary.");
        auto* slave=find(position);if(!slave)return failure("Slave does not exist.");
        if(!(slave->mailboxProtocols&4))return failure("Slave does not declare CoE support.");
        if(slave->state!=2)return failure("Online SDO dictionary discovery requires confirmed PRE-OP.");
        if(!slave->sdoInfo)return failure("Slave does not declare SDO Information support.");
        auto dictionary=std::make_shared<SdoDictionary>();
        const auto add=[&](uint16_t index,const char* name,uint8_t code) -> SdoObject& {
            dictionary->objects.push_back({});auto& object=dictionary->objects.back();
            object.index=index;object.objectCode=code;object.online=true;object.names={{0x0409,name}};return object;
        };
        const auto entry=[](SdoObject& object,uint8_t sub,const char* name,const char* type,unsigned bits,uint16_t access) {
            SdoEntry value;value.subindex=sub;value.names={{0x0409,name}};value.type=type;value.bitLength=bits;
            value.access=access;value.accessKnown=true;value.online=true;
            object.maxSubindex=std::max(object.maxSubindex,sub);object.entries.push_back(std::move(value));
        };
        entry(add(0x1000,"Device type",7),0,"Device type","UINT32",32,7);
        entry(add(0x1008,"Device name",7),0,"Device name","STRING",104,7);
        auto& identity=add(0x1018,"Identity",9);
        entry(identity,0,"Number of entries","UINT8",8,7);
        entry(identity,1,"Vendor ID","UINT32",32,7);entry(identity,2,"Product code","UINT32",32,7);
        entry(identity,3,"Revision","UINT32",32,7);entry(identity,4,"Serial number","UINT32",32,7);
        entry(add(0x8000,"Channel parameters",9),0x11,"Gain","UINT16",16,63);
        dictionary->complete=true;dictionary->status="Online SDO dictionary complete: 4 objects, 8 entries.";
        slave->sdoDictionary=std::move(dictionary);
        return success(slave->sdoDictionary->status);
    }
    Result readSdo(int position,uint16_t index,uint8_t sub,int capacity) override {
        if(!serviceAllowed())return failure("Stop cyclic exchange before synchronous SDO access.");
        auto* s=find(position);if(!s)return failure("Slave does not exist.");
        if(!(s->mailboxProtocols&4))return failure("Slave does not declare CoE support.");
        if(!mailboxAllowed(*s))return failure("SDO access requires PRE-OP, SAFE-OP or OP.");
        if(capacity<1||capacity>1024*1024)return failure("SDO capacity must be 1–1048576 bytes.");
        std::vector<uint8_t> data;
        if(index==0x1000&&sub==0)data={0x89,0x13,0,0};
        else if(index==0x1008&&sub==0) {const std::string name="EL3062 (DEMO)";data.assign(name.begin(),name.end());}
        else if(index==0x1018&&sub<=4) {
            if(sub==0)data={4};else {uint32_t value=sub==1?s->vendor:sub==2?s->product:sub==3?s->revision:s->serial;for(int i=0;i<4;++i)data.push_back(uint8_t(value>>(i*8)));}
        } else if(index==0x8000&&sub==0x11)data={uint8_t(gain_),uint8_t(gain_>>8)};
        else return failure("SDO Abort 0x06020000: object does not exist (simulation).");
        if(size_t(capacity)<data.size())return failure("SDO response exceeds receive capacity.");
        return success("Simulated SDO read complete.",std::move(data));
    }
    Result writeSdo(int position,uint16_t index,uint8_t sub,const std::vector<uint8_t>& data) override {
        if(!serviceAllowed())return failure("Stop cyclic exchange before synchronous SDO access.");
        auto* s=find(position);if(!s)return failure("Slave does not exist.");
        if(!(s->mailboxProtocols&4))return failure("Slave does not declare CoE support.");
        if(!mailboxAllowed(*s))return failure("SDO access requires PRE-OP, SAFE-OP or OP.");
        if(index==0x1000||index==0x1008||index==0x1018)return failure("SDO Abort 0x06010002: object is read-only (simulation).");
        if(index!=0x8000||sub!=0x11)return failure("SDO Abort 0x06020000: object does not exist (simulation).");
        if(data.size()!=2)return failure("SDO Abort 0x06070010: data type length mismatch (simulation).");
        gain_=uint16_t(data[0]|unsigned(data[1])<<8);return success("Simulated SDO write complete.");
    }
    Result writeOutput(int position,int offset,const std::vector<uint8_t>& data) override {
        auto* s=find(position);if(!s)return failure("Slave does not exist.");
        if(!view_.mapped)return failure("Mapping is invalid; scan devices again.");
        if(!mailboxAllowed(*s))return failure("Output preparation requires PRE-OP, SAFE-OP or OP.");
        if(offset<0||data.empty()||size_t(offset)>s->outputs.size()||data.size()>s->outputs.size()-size_t(offset))return failure("Write is outside the slave output image.");
        std::copy(data.begin(),data.end(),s->outputs.begin()+offset);
        return success((s->state&15)==8?"Output image updated; transmission occurs on subsequent simulated cycles.":"Output image preloaded locally; outputs are not yet applied by OP cycles.");
    }
    Result foeTransfer(int position,FoeDirection direction,const FoeOptions& options,const FoeControl& control) override {
        auto* slave=find(position);
        if(!view_.connected||!slave)return failure("Scan and select a slave before FoE transfer.");
        if(view_.running||std::any_of(view_.slaves.begin(),view_.slaves.end(),[](const Slave& s){return (s.state&15)==8;}))
            return failure("Stop cyclic I/O and move all OP slaves to PRE-OP before FoE transfer.");
        if(position!=4)return failure("The selected simulated slave does not support FoE.");
        if(slave->state!=2&&slave->state!=3)return failure("FoE transfer requires confirmed PRE-OP or BOOT.");

        // Use the production protocol/file job, with only the mailbox wire
        // replaced. Files remain private to this virtual slave, never Snapshot,
        // Command.data or Result.data. A partial download is not committed.
        std::deque<foe::Packet> replies;
        std::vector<uint8_t> incoming;
        const std::vector<uint8_t>* outgoing=nullptr;
        std::string filename;
        size_t outgoingOffset=0;
        uint32_t expectedWrite=1,readNumber=0;
        uint64_t virtualMs=0;
        bool finalRead=false,writeActive=false;
        constexpr size_t payload=512,ceiling=128u*1024u*1024u;
        const auto remoteError=[&](uint32_t code,const char* text){
            replies.push_back({foe::Opcode::Error,code,std::vector<uint8_t>(text,text+std::char_traits<char>::length(text))});
        };
        const auto nextRead=[&]{
            const size_t count=std::min(payload,outgoing->size()-outgoingOffset);
            std::vector<uint8_t> bytes(outgoing->begin()+outgoingOffset,outgoing->begin()+outgoingOffset+count);
            outgoingOffset+=count;finalRead=count<payload;
            replies.push_back({foe::Opcode::Data,++readNumber,std::move(bytes)});
        };
        foe::Wire wire;
        wire.txPayload=payload;wire.rxPayload=payload;
        wire.nowMs=[&]{return virtualMs;};
        wire.waitMs=[&](unsigned ms){virtualMs+=ms;};
        wire.send=[&](const foe::Packet& packet,int){
            ++virtualMs;
            switch(packet.opcode) {
            case foe::Opcode::Write:
                filename.assign(packet.data.begin(),packet.data.end());incoming.clear();expectedWrite=1;writeActive=true;
                replies.push_back({foe::Opcode::Ack,0,{}});break;
            case foe::Opcode::Data:
                if(!writeActive){remoteError(0x8004,"No active simulated download.");break;}
                if(packet.number==expectedWrite) {
                    if(packet.data.size()>ceiling-incoming.size()){remoteError(0x8003,"Simulated file exceeds the size limit.");break;}
                    incoming.insert(incoming.end(),packet.data.begin(),packet.data.end());++expectedWrite;
                    if(packet.data.size()<payload){files_[filename]=std::move(incoming);writeActive=false;}
                }else if(packet.number!=expectedWrite-1){remoteError(0x8005,"Unexpected simulated DATA packet.");break;}
                replies.push_back({foe::Opcode::Ack,packet.number,{}});break;
            case foe::Opcode::Read: {
                filename.assign(packet.data.begin(),packet.data.end());
                const auto found=files_.find(filename);
                if(found==files_.end()){remoteError(0x8001,"File not found in the simulated slave.");break;}
                outgoing=&found->second;outgoingOffset=0;readNumber=0;finalRead=false;nextRead();break;
            }
            case foe::Opcode::Ack:
                if(outgoing&&packet.number==readNumber&&!finalRead)nextRead();
                break;
            case foe::Opcode::Error:replies.clear();writeActive=false;break;
            case foe::Opcode::Busy:break;
            }
            return true;
        };
        wire.receive=[&](foe::Packet& packet,int timeoutMs){
            if(replies.empty()){virtualMs+=unsigned(std::max(1,timeoutMs));return 0;}
            ++virtualMs;packet=std::move(replies.front());replies.pop_front();return 1;
        };
        FoeControl callbacks=control;
        callbacks.progress=[&](const FoeProgress& progress){view_.foe=progress;if(control.progress)control.progress(progress);};
        auto result=foe::run(std::move(wire),position,direction,options,callbacks);
        view_.status=result.message;
        return result;
    }
    void cycle() override {
        if(!view_.running)return;
        ++view_.cycles;view_.wkc=view_.expectedWkc;
        view_.status="Simulated cyclic exchange running; no real hardware accessed.";
        const auto phase=view_.cycles/20;
        if(auto* s=find(2))s->inputs[0]=uint8_t(0x2c|(phase&1));
        if(auto* s=find(4)) {
            const uint16_t voltage=uint16_t(5000+std::sin(double(view_.cycles)*0.025)*500);
            s->inputs[1]=uint8_t(voltage);s->inputs[2]=uint8_t(voltage>>8);
        }
    }
    void sampleTrend() override {
        trends_.observe(view_.slaves,view_.connection.cycleUs,view_.wkc>=view_.expectedWkc);
    }
    void setTrendSelection(const std::vector<std::string>& keys) override {trends_.setSelection(keys,view_.slaves);}
    TrendHistory trendHistory() const override {return trends_.history();}
    void clearTrend() override {trends_.clear();}
    void close() override {
        view_.connected=false;view_.mapped=false;view_.running=false;view_.slaves.clear();view_.wkc=0;view_.expectedWkc=0;registers_.clear();
        view_.foe={};view_.status="Disconnected";
        clearTrend();
    }
    Snapshot snapshot() const override {return view_;}
};
}
std::unique_ptr<Backend> makeSimulation() {return std::make_unique<Simulation>();}
} // namespace niceecatm

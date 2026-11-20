// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "script/bridge.hpp"
#include "script/runtime.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace niceecatm::script {
namespace {
const PdoEntry* findSignal(const Slave& slave,const std::string& name,bool& ambiguous) {
    const PdoEntry* found=nullptr;
    ambiguous=false;
    for(const auto& entry:slave.pdoEntries) {
        if(entry.padding||pdoEntryName(entry,false)!=name)continue;
        if(found) {ambiguous=true;return nullptr;}
        found=&entry;
    }
    return found;
}
const PdoEntry* findSignal(const Slave& slave,uint16_t index,uint8_t subindex,bool& ambiguous) {
    const PdoEntry* found=nullptr;ambiguous=false;
    for(const auto& entry:slave.pdoEntries) {
        if(entry.padding||entry.index!=index||entry.subindex!=subindex)continue;
        if(found){ambiguous=true;return nullptr;}
        found=&entry;
    }
    return found;
}
const Slave* findSlave(const Snapshot& snapshot,int position) {
    for(const auto& slave:snapshot.slaves)if(slave.position==position)return &slave;
    return nullptr;
}
std::vector<uint8_t> rawField(const std::vector<uint8_t>& image,const PdoEntry& entry) {
    const auto value=readBits(image,entry.bitOffset,entry.bitLength);
    std::vector<uint8_t> bytes((entry.bitLength+7)/8);
    for(size_t n=0;n<bytes.size();++n)bytes[n]=static_cast<uint8_t>(value>>(8*n));
    return bytes;
}
std::string signalStateError(const Snapshot& snapshot,const Slave& slave,const char* reason) {
    std::ostringstream message;
    message<<reason<<" Slave "<<slave.position<<": AL="<<stateName(slave.state)
           <<" (0x"<<std::uppercase<<std::hex<<std::setw(4)<<std::setfill('0')<<slave.state
           <<"), AL code=0x"<<std::setw(4)<<slave.alCode<<std::dec
           <<", connected="<<(snapshot.connected?"true":"false")
           <<", mapped="<<(snapshot.mapped?"true":"false")
           <<", WKC="<<snapshot.wkc<<'/'<<snapshot.expectedWkc<<'.';
    return message.str();
}
}

uint64_t Bridge::submit(Command command) {
    const auto snapshot=controller_.snapshot();
    command.generation=snapshot.generation;
    return controller_.submit(std::move(command));
}

uint64_t Bridge::submit(const ScriptRequest& request,std::string* error) {
    auto fail=[&](const std::string& message)->uint64_t {if(error)*error=message;return 0;};
    Command command;
    switch(request.kind) {
    case RequestKind::RefreshAdapters:
        command.kind=CommandKind::RefreshAdapters;
        // Adapter enumeration is explicitly a hardware operation.  Preserve
        // the connection mode supplied by Runtime::adapters() so a script
        // started while the GUI is in simulation mode can still discover
        // physical SOEM interfaces.
        command.connection=request.connection;
        break;
    case RequestKind::Scan:command.kind=CommandKind::Scan;command.connection=request.connection;break;
    case RequestKind::Disconnect:command.kind=CommandKind::Disconnect;break;
    case RequestKind::State:command.kind=CommandKind::State;command.slave=request.slave;command.state=request.state;break;
    case RequestKind::SetCycle:command.kind=CommandKind::SetCycle;command.connection.cycleUs=request.connection.cycleUs;break;
    case RequestKind::Start:command.kind=CommandKind::Start;break;
    case RequestKind::Stop:command.kind=CommandKind::Stop;break;
    case RequestKind::ReadRegister:command.kind=CommandKind::ReadRegister;command.slave=request.slave;command.address=request.address;command.length=request.length;break;
    case RequestKind::WriteRegister:command.kind=CommandKind::WriteRegister;command.slave=request.slave;command.address=request.address;command.data=request.data;break;
    case RequestKind::ReadSdo:command.kind=CommandKind::ReadSdo;command.slave=request.slave;command.index=request.index;command.subindex=request.subindex;command.length=request.length;break;
    case RequestKind::WriteSdo:command.kind=CommandKind::WriteSdo;command.slave=request.slave;command.index=request.index;command.subindex=request.subindex;command.data=request.data;break;
    case RequestKind::WriteOutput:command.kind=CommandKind::WriteOutput;command.slave=request.slave;command.offset=request.offset;command.data=request.data;break;
    case RequestKind::ReadSignal:return fail("ReadSignal is a snapshot query; call Bridge::readSignal.");
    case RequestKind::WriteSignal:
        return request.signal.empty()?writeSignal(request.slave,request.index,request.subindex,request.value,false,error):writeSignal(request.slave,request.signal,request.value,false,error);
    }
    const auto id=submit(std::move(command));
    if(!id)return fail("Command queue is full.");
    if(error)error->clear();
    return id;
}

bool Bridge::execute(const ScriptRequest& request,HostResult& result) const {
    if(request.kind!=RequestKind::ReadSignal)return false;
    const auto info=request.signal.empty()?readSignal(request.slave,request.index,request.subindex):readSignal(request.slave,request.signal);
    result.ok=info.valid;
    result.message=info.valid?info.value:info.error;
    // Keep the decoded engineering value as the Lua return value. Raw bytes
    // remain available to the native Bridge API when a caller needs them.
    result.data.clear();
    return true;
}

uint64_t Bridge::adapters() {
    Command command;command.kind=CommandKind::RefreshAdapters;return submit(std::move(command));
}
uint64_t Bridge::scan(const Connection& connection) {
    Command command;command.kind=CommandKind::Scan;command.connection=connection;return submit(std::move(command));
}
uint64_t Bridge::disconnect() {
    Command command;command.kind=CommandKind::Disconnect;return submit(std::move(command));
}
uint64_t Bridge::state(int slave,AlState state) {
    Command command;command.kind=CommandKind::State;command.slave=slave;command.state=state;return submit(std::move(command));
}
uint64_t Bridge::setCycle(int cycleUs) {
    Command command;command.kind=CommandKind::SetCycle;command.connection.cycleUs=cycleUs;return submit(std::move(command));
}
uint64_t Bridge::readRegister(int slave,uint16_t offset,int length) {
    Command command;command.kind=CommandKind::ReadRegister;command.slave=slave;command.address=offset;command.length=length;return submit(std::move(command));
}
uint64_t Bridge::writeRegister(int slave,uint16_t offset,const std::vector<uint8_t>& data) {
    Command command;command.kind=CommandKind::WriteRegister;command.slave=slave;command.address=offset;command.data=data;return submit(std::move(command));
}
uint64_t Bridge::readSdo(int slave,uint16_t index,uint8_t subindex,int length) {
    Command command;command.kind=CommandKind::ReadSdo;command.slave=slave;command.index=index;command.subindex=subindex;command.length=length;return submit(std::move(command));
}
uint64_t Bridge::writeSdo(int slave,uint16_t index,uint8_t subindex,const std::vector<uint8_t>& data) {
    Command command;command.kind=CommandKind::WriteSdo;command.slave=slave;command.index=index;command.subindex=subindex;command.data=data;return submit(std::move(command));
}

Result Bridge::readSignalResult(int slavePosition,const std::string& name) const {
    const auto info=readSignal(slavePosition,name);
    if(!info.valid)return {false,info.error,{}};
    return {true,info.value,info.raw};
}
SignalInfo Bridge::readSignal(int slavePosition,const std::string& name) const {
    SignalInfo info;info.slave=slavePosition;info.name=name;
    const auto snapshot=controller_.snapshot();
    const auto* slave=findSlave(snapshot,slavePosition);
    if(!slave) {info.error="Signal target slave is not connected.";return info;}
    if((slave->state&15)==3){info.error="PDO signals are unavailable in BOOT.";return info;}
    bool ambiguous=false;
    const auto* entry=findSignal(*slave,name,ambiguous);
    if(!entry) {
        info.error=ambiguous?"Signal name is ambiguous; use bus:signal(slave, index, subindex).":"Signal name was not found.";
        return info;
    }
    info.input=entry->input;info.bitOffset=entry->bitOffset;info.bitLength=entry->bitLength;
    info.type=entry->type;info.source=entry->source;
    const auto& image=entry->input?slave->inputs:slave->outputs;
    if(entry->padding||entry->type.empty()||entry->bitLength==0||entry->bitLength>64||
       uint64_t(entry->bitOffset)+entry->bitLength>image.size()*8) {
        info.error="Signal has no supported mapped image value.";return info;
    }
    info.raw=rawField(image,*entry);info.value=decodeValue(info.raw,entry->type);
    if(info.value.empty()) {info.error="Signal value could not be decoded.";return info;}
    info.valid=true;return info;
}

SignalInfo Bridge::readSignal(int slavePosition,uint16_t index,uint8_t subindex) const {
    SignalInfo info;info.slave=slavePosition;info.bitLength=0;
    const auto snapshot=controller_.snapshot();const auto* slave=findSlave(snapshot,slavePosition);
    if(!slave){info.error="Signal target slave is not connected.";return info;}
    if((slave->state&15)==3){info.error="PDO signals are unavailable in BOOT.";return info;}
    bool ambiguous=false;const auto* entry=findSignal(*slave,index,subindex,ambiguous);
    if(!entry){info.error=ambiguous?"PDO index/subindex is ambiguous.":"PDO index/subindex was not found.";return info;}
    info.name=pdoEntryName(*entry,false);info.input=entry->input;info.bitOffset=entry->bitOffset;info.bitLength=entry->bitLength;info.type=entry->type;info.source=entry->source;
    const auto& image=entry->input?slave->inputs:slave->outputs;
    if(entry->type.empty()||entry->bitLength==0||entry->bitLength>64||uint64_t(entry->bitOffset)+entry->bitLength>image.size()*8){info.error="Signal has no supported mapped image value.";return info;}
    info.raw=rawField(image,*entry);info.value=decodeValue(info.raw,entry->type);
    if(info.value.empty()){info.error="Signal value could not be decoded.";return info;}
    info.valid=true;return info;
}

uint64_t Bridge::writeSignal(int slavePosition,const std::string& name,const std::string& value,
                             bool hex,std::string* error) {
    auto fail=[&](const std::string& text)->uint64_t {if(error)*error=text;return 0;};
    const auto snapshot=controller_.snapshot();
    const auto* slave=findSlave(snapshot,slavePosition);
    if(!slave)return fail("Signal target slave is not connected.");
    bool ambiguous=false;
    const auto* entry=findSignal(*slave,name,ambiguous);
    if(!entry)return fail(ambiguous?"Signal name is ambiguous; use bus:signal(slave, index, subindex).":"Signal name was not found.");
    if(entry->input)return fail("Input signals are read only.");
    if(entry->padding||entry->type.empty()||entry->bitLength==0||entry->bitLength>64)
        return fail("Signal does not have a supported writable type.");
    const auto state=slave->state&0x0f;
    if(!snapshot.connected||!snapshot.mapped||(state!=2&&state!=4&&state!=8))
        return fail(signalStateError(snapshot,*slave,"Signal write requires a mapped PRE-OP, SAFE-OP or OP slave."));
    std::vector<uint8_t> encoded;std::string conversion;
    if(!encodeValue(value,entry->type,hex,encoded,conversion))return fail(conversion);
    auto candidate=slave->outputs;
    if(!writeBits(candidate,entry->bitOffset,entry->bitLength,readBits(encoded,0,entry->bitLength),conversion))return fail(conversion);
    Command command;command.kind=CommandKind::WriteOutput;command.slave=slavePosition;command.offset=0;command.data=std::move(candidate);
    const auto request=submit(std::move(command));
    if(!request)return fail("Command queue is full.");
    if(error)error->clear();
    return request;
}

uint64_t Bridge::writeSignal(int slavePosition,uint16_t index,uint8_t subindex,const std::string& value,
                             bool hex,std::string* error) {
    auto fail=[&](const std::string& text)->uint64_t {if(error)*error=text;return 0;};
    const auto snapshot=controller_.snapshot();const auto* slave=findSlave(snapshot,slavePosition);
    if(!slave)return fail("Signal target slave is not connected.");
    bool ambiguous=false;const auto* entry=findSignal(*slave,index,subindex,ambiguous);
    if(!entry)return fail(ambiguous?"PDO index/subindex is ambiguous.":"PDO index/subindex was not found.");
    if(entry->input)return fail("Input signals are read only.");
    if(entry->type.empty()||entry->bitLength==0||entry->bitLength>64)return fail("Signal does not have a supported writable type.");
    const auto state=slave->state&0x0f;
    if(!snapshot.connected||!snapshot.mapped||(state!=2&&state!=4&&state!=8))
        return fail(signalStateError(snapshot,*slave,"Signal write requires a mapped PRE-OP, SAFE-OP or OP slave."));
    std::vector<uint8_t> encoded;std::string conversion;
    if(!encodeValue(value,entry->type,hex,encoded,conversion))return fail(conversion);
    auto candidate=slave->outputs;
    if(!writeBits(candidate,entry->bitOffset,entry->bitLength,readBits(encoded,0,entry->bitLength),conversion))return fail(conversion);
    Command command;command.kind=CommandKind::WriteOutput;command.slave=slavePosition;command.offset=0;command.data=std::move(candidate);
    const auto request=submit(std::move(command));if(!request)return fail("Command queue is full.");
    if(error)error->clear();
    return request;
}
}


// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "master/controller.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace niceecatm::script {
struct ScriptRequest;
struct HostResult;

// The script layer talks to this bridge only. It never receives a SOEM
// context, IOmap pointer, mailbox buffer or Backend object. Requests are
// queued and completed by the controller's single I/O owner thread.
struct SignalInfo {
    bool input=false;
    bool valid=false;
    int slave=0;
    unsigned bitOffset=0, bitLength=0;
    std::string name,type,source,value,error;
    std::vector<uint8_t> raw;
};

class Bridge {
public:
    explicit Bridge(Controller& controller):controller_(controller){}

    // A snapshot is a copy and may safely be consumed by a script thread.
    Snapshot snapshot() const {return controller_.snapshot();}
    // Results remain available in the controller's bounded history. False
    // means that the request is still pending or has aged out of that history.
    bool result(uint64_t request,Result& value) const {return controller_.result(request,value);}

    // Generic command entry point for ScriptRuntime HostCallbacks. It stamps
    // the current connection generation before submitting, so a scan or
    // disconnect invalidates already queued script operations.
    uint64_t submit(Command command);
    bool cancel(uint64_t request) {return controller_.cancel(request);}
    // Adapter for ScriptRuntime requests. ReadSignal is a snapshot query and
    // should be handled with readSignal(); command-like requests are queued.
    uint64_t submit(const ScriptRequest& request,std::string* error=nullptr);
    // Immediate host path for snapshot-only requests such as ReadSignal.
    bool execute(const ScriptRequest& request,HostResult& result) const;

    uint64_t adapters();
    uint64_t scan(const Connection& connection);
    uint64_t disconnect();
    uint64_t state(int slave,AlState state);
    uint64_t setCycle(int cycleUs);
    uint64_t readRegister(int slave,uint16_t offset,int length);
    uint64_t writeRegister(int slave,uint16_t offset,const std::vector<uint8_t>& data);
    uint64_t readSdo(int slave,uint16_t index,uint8_t subindex,int length);
    uint64_t writeSdo(int slave,uint16_t index,uint8_t subindex,const std::vector<uint8_t>& data);

    // Signal reads use the latest published compact image and never access the
    // backend. Writes validate the current generation and submit a complete
    // candidate output image through the same queue as BufferPDO.
    SignalInfo readSignal(int slave,const std::string& name) const;
    SignalInfo readSignal(int slave,uint16_t index,uint8_t subindex) const;
    Result readSignalResult(int slave,const std::string& name) const;
    uint64_t writeSignal(int slave,const std::string& name,const std::string& value,
                         bool hex=false,std::string* error=nullptr);
    uint64_t writeSignal(int slave,uint16_t index,uint8_t subindex,const std::string& value,
                         bool hex=false,std::string* error=nullptr);

private:
    Controller& controller_;
};
}

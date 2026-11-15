// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "script/bridge.hpp"
#include "script/runtime.hpp"
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int checks=0;
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
bool waitFor(const std::function<bool()>& condition) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline) {
        if(condition())return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
}
bool completed(const niceecatm::script::Bridge& bridge,uint64_t id,niceecatm::Result& result) {
    return bridge.result(id,result);
}
}

int main() {
    using namespace niceecatm;
    using script::Bridge;
    using script::HostResult;
    using script::RequestKind;
    using script::ScriptRequest;
    try {
        Controller controller;
        Bridge bridge(controller);
        check(waitFor([&]{return !bridge.snapshot().adapters.empty();}),"Bridge sees initial adapters");

        Connection connection;connection.simulation=true;connection.primary="script-simulation";connection.cycleUs=500;
        std::string error;
        ScriptRequest request;request.kind=RequestKind::Scan;request.connection=connection;
        const auto scan=bridge.submit(request,&error);check(scan!=0,"Bridge adapter admits scan");
        Result result;
        check(waitFor([&]{return completed(bridge,scan,result);}),"Bridge scan result is available");
        check(result.ok,"Bridge scan succeeds");
        auto snapshot=bridge.snapshot();
        const auto inputSignal=bridge.readSignal(2,"Start button");
        check(inputSignal.valid&&inputSignal.type=="BOOL","Bridge resolves simulator input signal metadata");
        const auto outputSignal=bridge.readSignal(5,"Speed setpoint");
        check(outputSignal.valid&&outputSignal.type=="INT16","Bridge resolves multi-bit output signal metadata");
        std::string writeError;
        const auto writeSignal=bridge.writeSignal(5,"Speed setpoint","120",false,&writeError);
        check(writeSignal!=0&&writeError.empty(),"Bridge queues multi-bit signal write");
        check(waitFor([&]{return completed(bridge,writeSignal,result);}),"Bridge signal write result is available");
        check(result.ok&&bridge.readSignal(5,"Speed setpoint").value=="120","Bridge writes multi-bit signal value");
        ScriptRequest readValue;readValue.kind=RequestKind::ReadSignal;readValue.slave=5;readValue.signal="Speed setpoint";
        HostResult hostValue;
        check(bridge.execute(readValue,hostValue)&&hostValue.ok&&hostValue.message=="120","Bridge immediate read adapter returns decoded value");
        check(snapshot.connected&&snapshot.mapped&&snapshot.generation>0,"Bridge snapshot publishes mapped scan");
        check(snapshot.slaves.size()==5,"Bridge snapshot includes simulation slaves");

        const auto cycle=bridge.setCycle(5000);check(cycle!=0,"Bridge admits target cycle");
        check(waitFor([&]{return completed(bridge,cycle,result);}),"Bridge cycle result is available");
        check(result.ok&&bridge.snapshot().connection.cycleUs==5000,"Bridge accepts 5 ms cycle");

        const auto state=bridge.state(0,AlState::Op);check(state!=0,"Bridge admits state transition");
        check(waitFor([&]{return completed(bridge,state,result);}),"Bridge state result is available");
        check(result.ok&&bridge.snapshot().running,"Bridge state starts cyclic exchange");
        check(waitFor([&]{return bridge.snapshot().cycles>1;}),"Bridge snapshot publishes cycle progress");

        const auto missing=bridge.readSignal(1,"missing");
        check(!missing.valid&&!missing.error.empty(),"Missing signal reports a stable error");
        ScriptRequest readRequest;readRequest.kind=RequestKind::ReadSignal;
        check(bridge.submit(readRequest,&error)==0&&!error.empty(),"Read signal adapter requires snapshot API");
        check(bridge.writeSignal(1,"missing","1",false,&error) == 0&&!error.empty(),"Missing signal write is rejected without queueing");

        const auto preop=bridge.state(0,AlState::PreOp);check(preop!=0,"Bridge admits lower state");
        check(waitFor([&]{return completed(bridge,preop,result);}),"Bridge lower-state result is available");
        check(result.ok&&!bridge.snapshot().running,"Bridge lower state stops cyclic exchange");

        const auto init=bridge.state(5,AlState::Init);check(init!=0,"Bridge admits genuine INIT state");
        check(waitFor([&]{return completed(bridge,init,result);}),"Bridge INIT result is available");
        check(result.ok&&bridge.snapshot().slaves[4].state==1,"Bridge publishes confirmed INIT");
        check(bridge.writeSignal(5,"Speed setpoint","121",false,&error)==0,"Named signal write still rejects INIT");
        const auto namedStateError=error;
        check(error.find("Slave 5: AL=INIT (0x0001), AL code=0x0000")!=std::string::npos&&
              error.find("connected=true, mapped=true, WKC=")!=std::string::npos,
              "State rejection includes target, raw AL/code, mapping and transport context");
        check(bridge.writeSignal(5,0x7100,1,"121",false,&error)==0&&error==namedStateError,
              "Indexed signal write rejects INIT with the same diagnostic context");
        check(bridge.readSignal(5,"Speed setpoint").value=="120","Rejected INIT writes preserve the output image");

        const auto disconnect=bridge.disconnect();check(disconnect!=0,"Bridge admits disconnect");
        check(waitFor([&]{return completed(bridge,disconnect,result);}),"Bridge disconnect result is available");
        check(result.ok&&!bridge.snapshot().connected&&bridge.snapshot().slaves.empty(),"Bridge disconnect clears snapshot");
        std::cout<<checks<<" script bridge checks passed.\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"Script bridge check failed: "<<error.what()<<'\n';
        return 1;
    }
}




// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "master/controller.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
std::mutex gateMutex;
std::condition_variable gate;
bool entered=false,released=false;
std::atomic<bool> staleHealth{false};
std::atomic<uint64_t> observedCycles{0};
std::atomic<int> pendingDiagnostic{0};
std::atomic<unsigned> descriptionReads{0};
int checks=0;
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
bool waitFor(const std::function<bool()>& condition) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline) {if(condition())return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    return condition();
}
}

// The controller test substitutes only the hardware factory. No driver or network is touched.
namespace niceecatm {
class BlockingBackend final:public Backend {
    std::unique_ptr<Backend> simulation_=makeSimulation();
    unsigned scanNumber_=0;
    bool descriptionPending_=false;
public:
    Result adapters() override {return simulation_->adapters();}
    Result scan(const Connection& c) override {
        auto result=simulation_->scan(c);
        if(result.ok)++scanNumber_;
        descriptionPending_=result.ok;
        return result;
    }
    bool hasPendingPdoDescriptions() const override {return descriptionPending_;}
    bool servicePdoDescriptions() override {
        if(!descriptionPending_)return false;
        descriptionPending_=false;++descriptionReads;return true;
    }
    Result state(int slave,AlState s) override {return simulation_->state(slave,s);}
    Result setCycle(int cycleUs) override {return simulation_->setCycle(cycleUs);}
    Result start() override {return simulation_->start();}
    Result stop() override {return simulation_->stop();}
    Result readRegister(int slave,uint16_t offset,int length) override {
        std::unique_lock<std::mutex> lock(gateMutex);entered=true;gate.notify_all();
        if(!gate.wait_for(lock,std::chrono::seconds(3),[]{return released;}))return {false,"Test gate timed out.",{}};
        return simulation_->readRegister(slave,offset,length);
    }
    Result writeRegister(int slave,uint16_t offset,const std::vector<uint8_t>& b) override {return simulation_->writeRegister(slave,offset,b);}
    Result readSdo(int slave,uint16_t index,uint8_t sub,int size) override {return simulation_->readSdo(slave,index,sub,size);}
    Result writeSdo(int slave,uint16_t index,uint8_t sub,const std::vector<uint8_t>& b) override {return simulation_->writeSdo(slave,index,sub,b);}
    Result readSdoDictionary(int slave) override {return simulation_->readSdoDictionary(slave);}
    Result writeOutput(int slave,int offset,const std::vector<uint8_t>& b) override {return simulation_->writeOutput(slave,offset,b);}
    void cycle() override {simulation_->cycle();++observedCycles;}
    void close() override {descriptionPending_=false;simulation_->close();}
    Snapshot snapshot() const override {
        auto s=simulation_->snapshot();s.simulation=false;
        if(staleHealth.load())s.status="Working counter failed: inputs stale";
        if(s.connected) {
            auto& names=s.slaves.back().pdoEntries.back().names;
            names.clear();
            if(!descriptionPending_)names={{0,"Discovered on scan "+std::to_string(scanNumber_)}};
        }
        return s;
    }
    std::vector<Result> takeDiagnostics() override {
        const int notice=pendingDiagnostic.exchange(0);
        if(notice==1)return {{false,"AL test: status read failed",{}}};
        if(notice==2)return {{true,"AL test: status read recovered",{}}};
        return {};
    }
};
std::unique_ptr<Backend> makeSoem() {return std::make_unique<BlockingBackend>();}
}

int main() {
    using namespace niceecatm;
    try {
        Controller controller;
        check(waitFor([&]{return !controller.snapshot().adapters.empty();}),"Worker initializes without blocking GUI");
        auto initial=controller.snapshot();check(!initial.simulation&&!initial.connected&&initial.connection.mode==ConnectionMode::Hardware,"Default is unconnected Hardware");
        auto send=[&](Command command) {
            auto id=controller.submit(std::move(command));check(id!=0,"Command admitted");
            check(waitFor([&]{return controller.snapshot().completed==id&&!controller.snapshot().busy;}),"Command completes");
            return controller.snapshot();
        };
        Command scan;scan.kind=CommandKind::Scan;scan.connection.mode=ConnectionMode::Simulation;scan.connection.primary="sim-primary";
        auto view=send(scan);check(view.result.ok&&view.connected&&view.generation>initial.generation,"Scan creates a fresh generation");
        Command cycle;cycle.kind=CommandKind::SetCycle;cycle.connection.cycleUs=20000;
        view=send(cycle);check(view.result.ok&&view.connection.cycleUs==20000,"Connected PRE-OP accepts target cycle update");
        const auto oldGeneration=view.generation;
        view=send(scan);check(view.result.ok&&view.generation>oldGeneration,"Rescan invalidates prior generation");
        Command output;output.kind=CommandKind::WriteOutput;output.slave=5;output.offset=0;output.data={0xaa};output.generation=oldGeneration;
        view=send(output);check(!view.result.ok&&view.slaves[4].outputs[0]==0,"Old output command rejected after rescan");
        output.generation=0;view=send(output);check(view.result.ok&&view.slaves[4].outputs[0]==0xaa,"Current output command applied directly");
        Command dictionary;dictionary.kind=CommandKind::ReadSdoDictionary;dictionary.slave=4;
        const auto dictionaryGeneration=view.generation;
        view=send(dictionary);check(view.result.ok&&view.slaves[3].sdoDictionary&&view.slaves[3].sdoDictionary->complete,"PRE-OP dictionary command publishes immutable metadata");
        check(view.generation==dictionaryGeneration&&!view.running&&view.cycles==0,"Dictionary discovery does not reconnect, change state or start I/O");
        dictionary.generation=oldGeneration;view=send(dictionary);check(!view.result.ok,"Stale dictionary command is rejected");dictionary.generation=0;
        Command state;state.kind=CommandKind::State;state.state=AlState::Op;
        view=send(state);check(view.result.ok&&view.running,"OP starts cyclic owner");
        check(waitFor([&]{return controller.snapshot().cycles>2;}),"Cyclic process exchange advances independently of GUI");
        Command sdo;sdo.kind=CommandKind::ReadSdo;sdo.slave=4;sdo.index=0x1000;sdo.length=4;
        view=send(sdo);check(!view.result.ok&&view.running,"Synchronous Mailbox access cannot stall PDO cycles");
        view=send(dictionary);check(!view.result.ok&&view.running,"Dictionary discovery cannot interrupt active cyclic I/O");
        state.state=AlState::PreOp;view=send(state);check(view.result.ok&&!view.running&&view.slaves[4].state==2,"Lower-state request stops cycles first");
        check(view.slaves[4].outputs[0]==0xaa,"State control preserves output image");

        Command disconnectBeforeMode;disconnectBeforeMode.kind=CommandKind::Disconnect;view=send(disconnectBeforeMode);
        Command hardware;hardware.kind=CommandKind::SetMode;hardware.connection.mode=ConnectionMode::Hardware;
        const auto beforeMode=view.generation;view=send(hardware);
        check(!view.connected&&view.slaves.empty()&&view.generation>beforeMode,"Mode changes clear prior devices and advance generation");
        auto discoveredName=[](const Snapshot& s){return pdoEntryName(s.slaves.back().pdoEntries.back(),false);};
        check(controller.setScriptRunning(true),"Script acquires bus for its scan");
        scan.connection.mode=ConnectionMode::Hardware;view=send(scan);check(view.result.ok&&view.connected,"Test backend scan");
        check(discoveredName(view).empty()&&descriptionReads==0,"Script can inspect layout before optional mailbox reads");
        controller.setScriptRunning(false);
        check(waitFor([&]{return discoveredName(controller.snapshot())=="Discovered on scan 1";}),"Idle worker publishes newly discovered name to GUI");
        check(discoveredName(controller.scriptSnapshot())=="Discovered on scan 1"&&controller.snapshot().generation==view.generation,
              "Metadata update reaches script snapshot in the same connection generation");

        check(controller.setScriptRunning(true),"Hold optional mailbox reads across rescan");
        view=send(scan);check(view.result.ok&&discoveredName(view).empty(),"Rescan clears previous description even with identical PDO mapping");
        state.state=AlState::Op;view=send(state);check(view.result.ok&&view.running,"Start cyclic exchange with descriptions pending");
        controller.setScriptRunning(false);
        check(waitFor([&]{return controller.snapshot().cycles>=3;}),"Cyclic exchange continues while descriptions are pending");
        check(descriptionReads==1&&discoveredName(controller.snapshot()).empty(),"No optional mailbox accesses during cyclic I/O");
        state.state=AlState::PreOp;view=send(state);check(view.result.ok,"Return to stopped PRE-OP");
        check(waitFor([&]{return discoveredName(controller.snapshot())=="Discovered on scan 2";}),"Returning to PRE-OP resumes and publishes descriptions for new scan");
        cycle.connection.cycleUs=100000;view=send(cycle);
        observedCycles=0;state.state=AlState::Op;view=send(state);
        check(waitFor([]{return observedCycles.load()>0;}),"Long-period exchange starts");
        const auto stopStart=std::chrono::steady_clock::now();
        state.state=AlState::PreOp;view=send(state);
        check(view.result.ok&&!view.running&&std::chrono::steady_clock::now()-stopStart<std::chrono::milliseconds(80),
              "Priority state command interrupts a 100 ms cycle wait");
#ifdef _WIN32
        // This backend does not call SOEM or timeBeginPeriod: the worker must
        // sustain short deadlines without depending on a driver's timer state.
        constexpr DWORD HighResolution=0x00000002;
        const HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,HighResolution,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if(timer) {
            CloseHandle(timer);
            cycle.connection.cycleUs=500;view=send(cycle);
            observedCycles=0;state.state=AlState::Op;view=send(state);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            state.state=AlState::PreOp;view=send(state);
            // A broad margin allows loaded CI machines, but catches the old
            // approximately 15.6 ms CV tick (only about 20 cycles in 300 ms).
            check(view.result.ok&&observedCycles.load()>100,"High-resolution wait avoids the default 15.6 ms timer tick");
        }
#endif
        state.state=AlState::Op;view=send(state);staleHealth=true;
        check(waitFor([&]{return controller.snapshot().status=="Working counter failed: inputs stale";}),"Live transport health is not hidden by the previous command result");
        pendingDiagnostic=1;
        check(waitFor([&]{
            for(const auto& entry:controller.snapshot().logs)
                if(entry.message=="AL test: status read failed")return entry.error;
            return false;
        }),"Asynchronous AL failures reach the global log during cyclic exchange");
        pendingDiagnostic=2;
        staleHealth=false;state.state=AlState::PreOp;view=send(state);
        unsigned failedNotices=0,recoveredNotices=0;
        for(const auto& entry:view.logs) {
            if(entry.message=="AL test: status read failed")++failedNotices;
            if(entry.message=="AL test: status read recovered"&&!entry.error)++recoveredNotices;
        }
        check(failedNotices==1&&recoveredNotices==1,"Global log keeps one failure and one recovery across command publication");
        Command read;read.kind=CommandKind::ReadRegister;read.slave=1;read.address=0x130;read.length=2;
        const auto blockingId=controller.submit(read);check(blockingId!=0,"Blocking test read queued");
        check(waitFor([]{std::lock_guard<std::mutex> lock(gateMutex);return entered;}),"Backend is in bounded operation");
        check(controller.pending(blockingId)&&!controller.pending(0),"Pending query identifies executing requests without relying on last completion order");
        const auto busyStart=std::chrono::steady_clock::now();view=controller.snapshot();
        check(view.busy&&std::chrono::steady_clock::now()-busyStart<std::chrono::milliseconds(30),"Snapshot remains responsive during network wait");
        std::vector<uint64_t> queued;
        for(int i=0;i<120;++i) {output.generation=0;auto id=controller.submit(output);check(id!=0,"Normal queue accepts bounded capacity");queued.push_back(id);}
        check(controller.submit(output)==0,"Normal queue overflow explicitly rejected");
        Command stop;stop.kind=CommandKind::Stop;const auto stopId=controller.submit(stop);
        check(stopId!=0,"Reserved capacity accepts priority Stop under full normal queue");
        check(controller.pending(stopId)&&controller.pending(queued.front()),"Pending query covers both priority and normal queues");
        Command disconnect;disconnect.kind=CommandKind::Disconnect;const auto disconnectId=controller.submit(disconnect);
        check(disconnectId!=0,"Reserved capacity accepts priority Disconnect");
        {std::lock_guard<std::mutex> lock(gateMutex);released=true;}gate.notify_all();
        check(waitFor([&]{auto s=controller.snapshot();return !s.connected&&s.completed==queued.back()&&!s.busy;}),"Disconnect runs first and queued previous-session commands drain as rejected");
        view=controller.snapshot();check(view.slaves.empty()&&view.generation>scan.generation,"Disconnected snapshot contains no stale device data");
        check(!controller.pending(blockingId)&&!controller.pending(stopId)&&!controller.pending(queued.back()),"Completed and drained requests are no longer pending");
        check(view.logs.size()<=500&&view.logs.size()>120,"Bounded command result history retained");
        {
            auto quitting=std::make_unique<Controller>();
            check(waitFor([&]{return !quitting->snapshot().adapters.empty();}),"Shutdown test worker initializes");
            scan.generation=0;scan.connection.cycleUs=100000;
            const auto scanId=quitting->submit(scan);
            check(scanId&&waitFor([&]{Result result;return quitting->result(scanId,result)&&result.ok;}),"Shutdown test bus scanned");
            state.state=AlState::Op;state.generation=0;observedCycles=0;
            const auto stateId=quitting->submit(state);
            check(stateId&&waitFor([]{return observedCycles.load()>0;}),"Shutdown test enters a long cycle wait");
            const auto exitStart=std::chrono::steady_clock::now();
            quitting.reset();
            check(std::chrono::steady_clock::now()-exitStart<std::chrono::milliseconds(80),"Shutdown interrupts a 100 ms cycle wait");
        }
        std::cout<<checks<<" controller checks passed.\n";return 0;
    } catch(const std::exception& error) {
        {std::lock_guard<std::mutex> lock(gateMutex);released=true;}gate.notify_all();
        std::cerr<<"Controller check failed: "<<error.what()<<'\n';return 1;
    }
}

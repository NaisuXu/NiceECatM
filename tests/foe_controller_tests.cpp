// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "master/controller.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace niceecatm;
std::atomic<unsigned> transfers{0};
std::atomic<unsigned> boots{0},registerWrites{0};
std::atomic<bool> slow{false},entered{false},cancelled{false},inTransfer{false},unsafeClose{false};
std::atomic<bool> slowBoot{false},enteredBoot{false};
int checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
bool waitFor(const std::function<bool()>& condition){
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<until){if(condition())return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    return condition();
}
class MockFoe final:public Backend {
    Snapshot view_;
    Result unused(){return {false,"Unused mock service",{}};}
public:
    Result adapters() override{view_.adapters={{"foe-mock","FoE mock"}};return {true,"Mock interface",{}};}
    Result scan(const Connection& connection) override{
        view_.connection=connection;view_.connected=view_.mapped=true;view_.running=false;
        Slave slave;slave.position=1;slave.name="FoE mock";slave.state=slave.requested=2;slave.mailboxProtocols=4;slave.bootstrapMailbox=true;
        view_.slaves={slave};return {true,"Mock scanned",{}};
    }
    Result scanProgramming(const Connection& connection,const EepromControl&) override{
        scan(connection);view_.mapped=false;view_.slaves[0].state=1;return {true,"Programming inventory",{}};
    }
    Result state(int,AlState state) override{
        if(!view_.connected)return unused();
        if(state==AlState::Boot){
            ++boots;enteredBoot=true;
            if(slowBoot)std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
        view_.slaves[0].state=uint16_t(state);view_.running=state==AlState::Op||state==AlState::SafeOp;
        if(state==AlState::Boot)view_.mapped=false;
        return {true,"Mock state confirmed",{}};
    }
    Result foeTransfer(int slave,FoeDirection direction,const FoeOptions&,const FoeControl& control) override{
        ++transfers;inTransfer=true;entered=true;
        FoeProgress progress;progress.slave=slave;progress.direction=direction;progress.total=32;
        for(size_t i=0;i<32;++i){
            if(control.cancelled&&control.cancelled()){
                cancelled=true;progress.stage=FoeStage::Cancelled;progress.detail="Mock FoE cancelled";
                if(control.progress)control.progress(progress);
                inTransfer=false;return {false,"Mock FoE cancelled",{}};
            }
            progress.stage=direction==FoeDirection::Upload?FoeStage::Receiving:FoeStage::Sending;
            progress.completed=i;progress.blocks=i;
            if(control.progress)control.progress(progress);
            if(slow)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        progress.stage=FoeStage::Complete;progress.completed=progress.total;progress.detail="Mock FoE completed";
        if(control.progress)control.progress(progress);
        inTransfer=false;
        // Even a buggy backend must not put file bytes into command history.
        return {true,"Mock FoE completed",std::vector<uint8_t>(4096,0xaa)};
    }
    Result setCycle(int) override{return unused();}
    Result start() override{return unused();}
    Result stop() override{view_.running=false;return {true,"Mock stopped",{}};}
    Result readRegister(int,uint16_t,int) override{return unused();}
    Result writeRegister(int slave,uint16_t address,const std::vector<uint8_t>& bytes) override{
        ++registerWrites;
        if(address==0x0120&&bytes==std::vector<uint8_t>{3,0})return state(slave,AlState::Boot);
        return unused();
    }
    Result readSdo(int,uint16_t,uint8_t,int) override{return unused();}
    Result writeSdo(int,uint16_t,uint8_t,const std::vector<uint8_t>&) override{return unused();}
    Result writeOutput(int,int,const std::vector<uint8_t>&) override{return unused();}
    void cycle() override{}
    void close() override{if(inTransfer)unsafeClose=true;view_.connected=view_.mapped=view_.running=false;view_.slaves.clear();}
    Snapshot snapshot() const override{return view_;}
};
}
namespace niceecatm {std::unique_ptr<Backend> makeSoem(){return std::make_unique<MockFoe>();}}

int main(){
    using namespace niceecatm;
    try{
        Controller controller;
        check(waitFor([&]{return !controller.snapshot().adapters.empty();}),"Worker ready");
        auto submit=[&](Command command){const auto id=controller.submit(std::move(command));check(id!=0,"Command admitted");return id;};
        auto result=[&](uint64_t id){Result value;check(waitFor([&]{return controller.result(id,value);}),"Bounded result available");return value;};
        auto send=[&](Command command){return result(submit(std::move(command)));};
        Command scan;scan.kind=CommandKind::Scan;scan.connection.primary="foe-mock";
        check(send(scan).ok,"Hardware fixture scanned without accessing network");
        Command transfer;transfer.kind=CommandKind::FoeDownload;transfer.slave=1;transfer.foe.remoteName="firmware.bin";
        transfer.foe.localPath="unused-by-mock";transfer.foe.password=0x51abcdef;
        check(controller.setScriptRunning(true),"Idle controller accepts script lifecycle");
        check(!send(transfer).ok&&transfers==0,"Active script blocks FoE before backend call");
        Command boot;boot.kind=CommandKind::State;boot.state=AlState::Boot;boot.slave=1;
        check(!send(boot).ok&&boots==0,"Active script blocks the BOOT button path");
        Command bootRegister;bootRegister.kind=CommandKind::WriteRegister;bootRegister.slave=1;bootRegister.address=0x0120;bootRegister.data={3,0};
        check(!send(bootRegister).ok&&boots==0&&registerWrites==0,"AL Control register cannot bypass the BOOT script gate");
        controller.setScriptRunning(false);
        Command state;state.kind=CommandKind::State;state.state=AlState::Op;
        check(send(state).ok,"Mock OP starts I/O");
        check(!send(transfer).ok&&transfers==0,"Running I/O blocks FoE without stopping the bus");
        state.state=AlState::PreOp;check(send(state).ok,"Explicit PRE-OP stops I/O");
        auto id=submit(transfer);check(result(id).ok,"FoE download completes");
        auto view=controller.snapshot();
        check(view.foe.request==id&&view.foe.slave==1&&view.foe.direction==FoeDirection::Download,"Progress retains request and target binding");
        check(view.foe.stage==FoeStage::Complete&&view.foe.completed==32,"Completion publishes acknowledged bytes");
        check(view.result.data.empty()&&view.result.data.capacity()==0,"Snapshot retains no file allocation");
        Result remembered;check(controller.result(id,remembered)&&remembered.data.empty(),"Result history retains no file bytes");
        for(const auto& log:view.logs)check(log.message.find("51abcdef")==std::string::npos,"Password never enters global logs");

        slowBoot=true;enteredBoot=false;id=submit(boot);
        check(waitFor([]{return enteredBoot.load();}),"BOOT transition entered");
        check(!controller.setScriptRunning(true),"Script start is atomically refused during BOOT transition");
        check(result(id).ok,"Bounded BOOT transition finishes");
        slowBoot=false;check(send(scan).ok,"Rescan rebuilds mapping after BOOT test");

        slow=true;entered=false;cancelled=false;id=submit(transfer);
        check(waitFor([]{return entered.load();}),"Cancellable job entered");
        check(!controller.setScriptRunning(true),"Script start is atomically refused during FoE");
        check(controller.submit(state)==0&&controller.submit(scan)==0&&controller.submit(transfer)==0,"Conflicting operations are rejected during FoE");
        check(controller.cancel(id),"Active FoE cancellation admitted");
        check(!result(id).ok&&cancelled,"Owner cooperatively observes cancellation");
        view=controller.snapshot();check(view.foe.stage==FoeStage::Cancelled&&!view.busy&&view.connected,"Cancellation preserves connection and terminal progress");

        entered=false;cancelled=false;id=submit(transfer);
        check(waitFor([]{return entered.load();}),"Second FoE job entered");
        Command disconnect;disconnect.kind=CommandKind::Disconnect;
        const auto disconnected=submit(disconnect);
        check(!result(id).ok&&result(disconnected).ok,"Disconnect cancels then completes on I/O owner");
        check(cancelled&&!unsafeClose&&!controller.snapshot().connected,"Transport closes only after FoE cleanup");
        slow=false;check(send(scan).ok,"Rescan after disconnect");
        auto stale=transfer;stale.generation=controller.snapshot().generation;
        check(send(scan).ok,"Second scan changes generation");
        const auto before=transfers.load();check(!send(stale).ok&&transfers==before,"Stale transfer never reaches a new bus");
        transfer.slave=0;check(!send(transfer).ok&&transfers==before,"FoE cannot broadcast to all slaves");transfer.slave=1;
        state.state=AlState::Init;check(send(state).ok&&!send(transfer).ok,"INIT cannot transfer FoE");
        check(send(disconnect).ok,"Disconnected before mode change");
        Command mode;mode.kind=CommandKind::SetMode;mode.connection.mode=ConnectionMode::Programming;
        check(send(mode).ok,"Programming selected");scan.connection.mode=ConnectionMode::Programming;check(send(scan).ok,"Programming inventory scanned");
        check(!send(transfer).ok&&transfers==before,"Programming mode exposes no FoE backend access");

        {
            Controller shutdownController;
            check(waitFor([&]{return !shutdownController.snapshot().adapters.empty();}),"Shutdown fixture ready");
            scan.connection.mode=ConnectionMode::Hardware;
            const auto scanned=shutdownController.submit(scan);Result value;
            check(waitFor([&]{return shutdownController.result(scanned,value);})&&value.ok,"Shutdown fixture scanned");
            slow=true;entered=false;cancelled=false;shutdownController.submit(transfer);
            check(waitFor([]{return entered.load();}),"Shutdown job entered");
        }
        check(cancelled&&!unsafeClose,"Destructor cancels FoE before closing the owner transport");
        std::cout<<checks<<" FoE controller checks passed.\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FoE controller check failed: "<<error.what()<<'\n';return 1;}
}

// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "master/controller.hpp"
#include "esi/sii_image.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace niceecatm;
sii::Image fixture;
std::vector<uint8_t> writtenImage;
std::atomic<int> scans{0},writes{0},reads{0},ordinary{0};
std::atomic<bool> slow{false},entered{false},cancelled{false},cleanupFailure{false};
int checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
bool waitFor(const std::function<bool()>& predicate){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline){if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    return predicate();
}
class MockProgramming final:public Backend {
    Snapshot view_;
    Result unavailable(){++ordinary;return {false,"Unexpected ordinary backend operation",{}};}
    Result operation(bool write,size_t size,const EepromControl& control){
        if(write)++writes;else ++reads;
        entered=true;
        for(size_t offset=0;offset<size;offset+=64){
            if(control.cancelled&&control.cancelled()){
                cancelled=true;
                if(control.progress)control.progress({cleanupFailure?EepromStage::Failed:EepromStage::Cancelled,offset,size,write&&offset>0,cleanupFailure?"EEPROM ownership restoration failed":"Cancelled bounded operation"});
                return {false,write?"EEPROM programming cancelled; image may be partial.":"EEPROM read cancelled.",{}};
            }
            if(control.progress)control.progress({write?EepromStage::Writing:EepromStage::Reading,offset,size,write&&offset>0,"Mock bounded transfer"});
            if(slow)std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        if(control.progress)control.progress({EepromStage::Complete,size,size,write,write?"Verified mock image":"Read mock image"});
        return {true,write?"Verified EEPROM; activation pending.":"EEPROM read complete.",write?std::vector<uint8_t>{}:fixture.bytes};
    }
public:
    Result adapters() override{view_.adapters={{"mock-wired","Mock physical interface"}};return {true,"Mock adapters",{}};}
    Result scan(const Connection&) override{return unavailable();}
    Result scanProgramming(const Connection& config,const EepromControl&) override{
        ++scans;view_.connection=config;view_.connected=true;view_.mapped=view_.running=false;
        Slave target;target.position=1;target.name="Read hardware identity";target.state=target.requested=1;
        target.vendor=fixture.vendor;target.product=fixture.product;target.revision=fixture.revision;
        target.eepromIdentityKnown=target.eepromSizeKnown=true;target.eepromCapacity=uint32_t(fixture.capacity);
        view_.slaves={target};return {true,"Programming inventory ready",{}};
    }
    Result readEeprom(int,size_t length,const EepromControl& control) override{return operation(false,length,control);}
    Result writeEeprom(int,const std::vector<uint8_t>& image,const EepromControl& control) override{
        writtenImage=image;
        const auto result=operation(true,image.size(),control);
        if(image!=writtenImage)return {false,"Submitted image was not immutable",{}};
        return result;
    }
    Result state(int,AlState) override{return unavailable();}
    Result setCycle(int) override{return unavailable();}
    Result start() override{return unavailable();}
    Result stop() override{return unavailable();}
    Result readRegister(int,uint16_t,int) override{return unavailable();}
    Result writeRegister(int,uint16_t,const std::vector<uint8_t>&) override{return unavailable();}
    Result readSdo(int,uint16_t,uint8_t,int) override{return unavailable();}
    Result writeSdo(int,uint16_t,uint8_t,const std::vector<uint8_t>&) override{return unavailable();}
    Result writeOutput(int,int,const std::vector<uint8_t>&) override{return unavailable();}
    void cycle() override{++ordinary;}
    void close() override{view_.connected=view_.mapped=view_.running=false;view_.slaves.clear();}
    Snapshot snapshot() const override{return view_;}
};
}
namespace niceecatm {std::unique_ptr<Backend> makeSoem(){return std::make_unique<MockProgramming>();}}

int main(){
    using namespace niceecatm;
    try{
        std::string error;
        check(sii::fromEsi(std::filesystem::path(NICEECATM_SOURCE_DIR)/"resources/esi/AX58100_EVB_DIO_ESI_00010200_00000001_20220323.xml",0,fixture,error),error.c_str());
        Controller controller;
        check(waitFor([&]{return !controller.snapshot().adapters.empty();}),"Worker initializes without opening a bus");
        auto send=[&](Command command){
            const auto id=controller.submit(std::move(command));check(id!=0,"Command admitted");Result result;
            check(waitFor([&]{return controller.result(id,result);}),"Bounded command finishes");return result;
        };
        auto view=controller.snapshot();
        check(view.connection.mode==ConnectionMode::Hardware&&!view.connected&&scans==0,"Default Hardware is disconnected");
        Command mode;mode.kind=CommandKind::SetMode;mode.connection.mode=ConnectionMode::Programming;
        controller.setScriptRunning(true);
        check(!send(mode).ok,"Running scripts prevent explicit mode changes");controller.setScriptRunning(false);
        check(send(mode).ok,"Disconnected idle bus can select Programming");view=controller.snapshot();
        check(view.connection.mode==ConnectionMode::Programming&&!view.connected&&scans==0,"Selecting Programming sends no scan or I/O");
        Command state;state.kind=CommandKind::State;state.state=AlState::Op;
        check(!send(state).ok&&ordinary==0,"Programming rejects ordinary state control before backend access");
        Command read;read.kind=CommandKind::ReadEeprom;read.slave=1;read.length=int(fixture.capacity);
        check(!send(read).ok&&reads==0,"EEPROM access requires dedicated scan");
        Command scan;scan.kind=CommandKind::Scan;scan.connection.mode=ConnectionMode::Programming;scan.connection.primary="mock-wired";
        scan.connection.redundant=true;check(!send(scan).ok&&scans==0,"Programming rejects redundant configuration");scan.connection.redundant=false;
        controller.setScriptRunning(true);check(!send(scan).ok&&scans==0,"Programming scan refuses active scripts");controller.setScriptRunning(false);
        check(send(scan).ok&&scans==1,"Dedicated scan enumerates INIT without mapping");
        view=controller.snapshot();check(view.connected&&!view.mapped&&!view.running&&view.connection.mode==ConnectionMode::Programming,"Programming publishes a stopped unmapped inventory");
        mode.connection.mode=ConnectionMode::Hardware;check(!send(mode).ok,"Connected mode cannot switch implicitly");
        const auto readResult=send(read);check(readResult.ok&&readResult.data==fixture.bytes,"Completed read returns only complete immutable data");
        controller.setScriptRunning(true);check(!send(read).ok,"Read gate also checks the script lifecycle");controller.setScriptRunning(false);
        Command write;write.kind=CommandKind::WriteEeprom;write.slave=1;write.length=int(fixture.capacity);write.data=fixture.bytes;
        auto invalid=write;invalid.data[0]^=1;const auto before=writes.load();
        check(!send(invalid).ok&&writes==before,"Invalid configuration CRC never reaches the backend");
        invalid=write;invalid.data[130]=invalid.data[131]=0xff;
        check(!send(invalid).ok&&writes==before,"Invalid category length never reaches the backend");
        invalid=write;invalid.length=int(fixture.capacity/2);
        check(!send(invalid).ok&&writes==before,"Confirmed physical capacity bounds the image");
        const auto generation=controller.snapshot().generation;
        check(send(write).ok,"Valid full image is programmed and verified");view=controller.snapshot();
        check(view.generation>generation&&view.connection.mode==ConnectionMode::Programming&&!view.mapped&&!view.running,"Programming invalidates old handles and never resumes OP");
        state.generation=generation;check(!send(state).ok&&ordinary==0,"Old handles stay invalid after programming");
        auto staleWrite=write;staleWrite.generation=generation;const auto beforeStale=writes.load();
        const auto staleResult=send(staleWrite);
        check(!staleResult.ok&&staleResult.message=="Stale command rejected: the connection generation changed."&&writes==beforeStale,"Stale EEPROM write cannot reach the backend");
        for(unsigned changed=1;changed<=3;++changed){
            auto replacement=write;
            if(changed&1)replacement.data[16]^=1; // Vendor ID is outside configuration CRC.
            if(changed&2)replacement.data[20]^=1;
            sii::Image parsed;
            check(sii::fromBin(replacement.data,parsed,error),"A replacement identity preserves valid SII structure and CRC");
            const auto beforeReplacement=writes.load();
            check(send(replacement).ok&&writes==beforeReplacement+1&&writtenImage==replacement.data,
                changed==1?"Vendor ID replacement is submitted unchanged":
                changed==2?"Product code replacement is submitted unchanged":
                           "Vendor and product replacement is submitted unchanged");
        }
        slow=true;entered=false;cancelled=false;
        const auto cancelId=controller.submit(read);check(cancelId&&waitFor([]{return entered.load();}),"Long read starts cooperatively");
        check(controller.snapshot().busy,"Progress publication keeps the GUI responsive");
        check(controller.cancel(cancelId),"Active read cancellation is accepted");Result result;
        check(waitFor([&]{return controller.result(cancelId,result);})&&!result.ok&&cancelled,"Canceled read returns no successful backup");
        check(controller.snapshot().eeprom.stage==EepromStage::Cancelled&&!controller.snapshot().eeprom.written,"Read cancel publishes an unchanged target");
        entered=false;cleanupFailure=true;
        const auto failedCleanup=controller.submit(read);
        check(failedCleanup&&waitFor([]{return entered.load();})&&controller.cancel(failedCleanup),"Cancellation during a cleanup failure is accepted");
        check(waitFor([&]{return controller.result(failedCleanup,result);})&&!result.ok&&controller.snapshot().eeprom.stage==EepromStage::Failed,"Ownership failure is not hidden by a simultaneous cancellation");
        cleanupFailure=false;
        entered=false;cancelled=false;const auto writeId=controller.submit(write);
        check(writeId&&waitFor([&]{return entered.load()&&controller.snapshot().eeprom.completed>=64;}),"Long write publishes a written prefix");
        write.data.clear(); // A user edit cannot change the submitted image.
        Command disconnect;disconnect.kind=CommandKind::Disconnect;disconnect.generation=generation;
        const auto disconnectId=controller.submit(disconnect);check(disconnectId!=0,"Disconnect accepted during a long write");
        check(waitFor([&]{return controller.result(disconnectId,result);})&&result.ok&&cancelled,"Disconnect waits for cooperative cancellation despite changed generation");
        view=controller.snapshot();check(!view.connected&&view.slaves.empty()&&view.connection.mode==ConnectionMode::Programming,"Disconnect clears active targets and retains Programming");
        check(view.eeprom.stage==EepromStage::Cancelled&&view.eeprom.written,"Partial write outcome remains visible after disconnect");
        check(send(mode).ok&&!controller.snapshot().connected&&scans==1,"Returning to Hardware does not reconnect or scan");
        mode.connection.mode=ConnectionMode::Simulation;check(send(mode).ok,"Idle bus can return to Simulation");
        const auto simulationGeneration=controller.snapshot().generation;
        controller.setScriptRunning(true);Command enumerate;enumerate.kind=CommandKind::RefreshAdapters;enumerate.connection.mode=ConnectionMode::Hardware;
        const auto enumeration=send(enumerate);
        check(enumeration.ok&&enumeration.adapters&&enumeration.adapters->size()==1&&enumeration.adapters->front().name=="mock-wired","Scripts receive physical interface metadata independently of selected mode");
        check(controller.snapshot().connection.mode==ConnectionMode::Simulation&&controller.snapshot().generation==simulationGeneration,"Hardware enumeration neither changes mode nor invalidates the current bus");
        controller.setScriptRunning(false);
        slow=false;
        std::cout<<checks<<" EEPROM controller checks passed.\n";return 0;
    }catch(const std::exception& e){std::cerr<<"EEPROM controller test failed: "<<e.what()<<'\n';return 1;}
}

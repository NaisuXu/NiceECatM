// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "master/controller.hpp"
#include "esi/catalog.hpp"
#include "esi/sii_image.hpp"
#include "platform/paths.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <ctime>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
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

namespace niceecatm {
namespace {
using Clock=std::chrono::steady_clock;
#ifdef _WIN32
// A timed condition-variable wait on Windows rounds sub-millisecond deadlines
// to the system timer tick. Use a one-shot high-resolution timer instead, and
// an event so commands and shutdown can interrupt even a long cycle period.
class CycleWait {
    HANDLE wake_=nullptr,timer_=nullptr;
public:
    CycleWait() {
        wake_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        constexpr DWORD HighResolution=0x00000002;
        timer_=CreateWaitableTimerExW(nullptr,nullptr,HighResolution,TIMER_MODIFY_STATE|SYNCHRONIZE);
        // Older Windows versions do not support the high-resolution flag.
        if(!timer_)timer_=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    }
    ~CycleWait() {
        if(timer_)CloseHandle(timer_);
        if(wake_)CloseHandle(wake_);
    }
    CycleWait(const CycleWait&)=delete;
    CycleWait& operator=(const CycleWait&)=delete;
    bool available() const {return wake_&&timer_;}
    void notify() {if(wake_)SetEvent(wake_);}
    bool until(Clock::time_point deadline) {
        const auto remaining=deadline-Clock::now();
        if(remaining<=Clock::duration::zero())return true;
        const auto nanoseconds=std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
        LARGE_INTEGER due{};
        // Negative values are relative deadlines in units of 100 ns. Round
        // upward so an early wake cannot turn into a polling loop.
        due.QuadPart=-std::max<int64_t>(1,(nanoseconds+99)/100);
        if(!SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE))return false;
        const HANDLE handles[]={wake_,timer_};
        const DWORD result=WaitForMultipleObjects(2,handles,FALSE,INFINITE);
        CancelWaitableTimer(timer_);
        return result==WAIT_OBJECT_0||result==WAIT_OBJECT_0+1;
    }
};
#endif
std::string timestamp() {
    const auto now=std::chrono::system_clock::now();
    const auto seconds=std::chrono::system_clock::to_time_t(now);std::tm local{};
#ifdef _WIN32
    localtime_s(&local,&seconds);
#else
    localtime_r(&seconds,&local);
#endif
    std::ostringstream stream;stream<<std::put_time(&local,"%H:%M:%S");return stream.str();
}
bool priority(const Command& command) {
    return command.kind==CommandKind::Stop||command.kind==CommandKind::Disconnect||
        (command.kind==CommandKind::State&&(command.state==AlState::Init||command.state==AlState::PreOp));
}
bool raisesState(const Command& command) {
    return command.kind==CommandKind::Start||(command.kind==CommandKind::State&&(command.state==AlState::SafeOp||command.state==AlState::Op));
}
bool entersBoot(const Command& command) {
    return (command.kind==CommandKind::State&&command.state==AlState::Boot)||
        (command.kind==CommandKind::WriteRegister&&command.address==0x0120&&command.data.size()==2&&command.data[0]==3&&command.data[1]==0);
}
}

struct Controller::Impl {
    struct Queued {uint64_t id;Command command;bool canceled=false;};
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable scriptUpdated;
    uint64_t scriptVersion=0;
#ifdef _WIN32
    CycleWait cycleWait;
#endif
    std::deque<Queued> urgent,normal;
    std::thread worker;
    bool quitting=false;
    uint64_t nextId=1,generation=1,lastCompleted=0,logSequence=0;
    uint64_t activeRequest=0;
    Snapshot published,current,scriptPublished;
    std::atomic<bool> scriptSnapshots{false};
    std::atomic<bool> scriptRunning{false},cancelActive{false},shutdown{false};
    bool activeEeprom=false,activeFoe=false,activeBoot=false;
    std::vector<std::string> trendSelection;
    bool trendSelectionDirty=false;
    EepromProgress eeprom;
    FoeProgress foe;
    Result lastResult;
    std::map<uint64_t,Result> resultHistory;
    std::vector<LogEntry> logs;
    std::unique_ptr<Backend> backend;
    EsiCatalog esi{executableDirectory()/"esi"};
    struct Description {
        std::vector<PdoEntry> entries;
        std::string file,status;
        std::shared_ptr<const SdoDictionary> dictionary;
    };
    std::vector<Description> descriptions;
    Snapshot library;
    bool descriptionDirty=true,descriptionMapped=false;
    uint64_t descriptionGeneration=0;
    ConnectionMode mode=ConnectionMode::Hardware;
    uint64_t cycles=0,missed=0;
    double actualCycle=0,maxJitter=0;
    Clock::time_point nextCycle{},previousCycle{},nextPublication{};

    Impl() {
        published.generation=generation;
        scriptPublished=published;
        worker=std::thread([this]{run();});
    }
    ~Impl() {
        shutdown.store(true);cancelActive.store(true);
        {std::lock_guard<std::mutex> lock(mutex);quitting=true;}
        scriptUpdated.notify_all();
        notify();if(worker.joinable())worker.join();
    }
    void notify() {
        ready.notify_one();
#ifdef _WIN32
        cycleWait.notify();
#endif
    }
    // Called with mutex held, after publishing data visible to the Lua host.
    void notifyScriptUpdate() {
        if(scriptSnapshots.load(std::memory_order_relaxed)) {
            ++scriptVersion;
            scriptUpdated.notify_all();
        }
    }
    void clearTrendSelection() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            trendSelection.clear();
            trendSelectionDirty=true;
        }
        // Wake a stopped worker so it applies the empty selection and
        // publishes a snapshot with no stale trend history.
        notify();
    }
    void waitForCycle(std::unique_lock<std::mutex>& lock) {
        const auto canWake=[this]{return quitting||!urgent.empty()||!normal.empty()||trendSelectionDirty;};
#ifdef _WIN32
        if(cycleWait.available()) {
            // The auto-reset event retains notifications between releasing the
            // queue mutex and entering the wait; no command wake can be lost.
            lock.unlock();
            const bool waited=cycleWait.until(nextCycle);
            lock.lock();
            if(waited)return;
        }
#endif
        ready.wait_until(lock,nextCycle,canWake);
    }
    void appendLog(std::string message,bool error) {
        if(message.empty())return;
        std::lock_guard<std::mutex> lock(mutex);
        logs.push_back({++logSequence,timestamp(),std::move(message),error});
        if(logs.size()>500)logs.erase(logs.begin(),logs.begin()+static_cast<std::ptrdiff_t>(logs.size()-500));
        // Script messages may arrive while the bus is stopped, when the I/O
        // worker has no reason to publish another snapshot yet.
        published.logs=logs;scriptPublished.logs=logs;
    }
    void log(const Result& result) {
        // A successful cyclic output commit is routine transport activity;
        // keep it out of the shared log so actual diagnostics remain visible.
        if(result.ok&&result.message=="Output image committed for the next valid cycle")return;
        appendLog(result.message,!result.ok);
    }
    void publish(bool busy=false) {
        for(const auto& notice:backend->takeDiagnostics())log(notice);
        current=backend->snapshot();
        current.connection.mode=mode;current.simulation=mode==ConnectionMode::Simulation;
        current.eeprom=eeprom;
        current.foe=foe;
        current.trends=backend->trendHistory();
        // Cache the decorated layout between scans and idle description updates.
        if(mode==ConnectionMode::Programming) {
            descriptions.clear();library={};
            current.esiDirectory.clear();current.esiStatus.clear();current.esiFiles=current.esiDevices=0;
            for(auto& slave:current.slaves) {
                slave.pdoEntries.clear();slave.esiFile.clear();slave.esiStatus.clear();
                slave.sdoDictionary.reset();slave.esiSdoDictionary.reset();
            }
            descriptionDirty=true;
        } else if(descriptionDirty||descriptionGeneration!=generation||descriptionMapped!=current.mapped||descriptions.size()!=current.slaves.size()) {
            esi.decorate(current);
            descriptions.clear();descriptions.reserve(current.slaves.size());
            for(const auto& slave:current.slaves)descriptions.push_back({slave.pdoEntries,slave.esiFile,slave.esiStatus,slave.esiSdoDictionary});
            library.esiDirectory=current.esiDirectory;library.esiStatus=current.esiStatus;
            library.esiFiles=current.esiFiles;library.esiDevices=current.esiDevices;library.descriptionRevision=current.descriptionRevision;
            descriptionDirty=false;descriptionGeneration=generation;descriptionMapped=current.mapped;
        } else {
            current.esiDirectory=library.esiDirectory;current.esiStatus=library.esiStatus;
            current.esiFiles=library.esiFiles;current.esiDevices=library.esiDevices;current.descriptionRevision=library.descriptionRevision;
            for(size_t n=0;n<current.slaves.size();++n) {
                current.slaves[n].pdoEntries=descriptions[n].entries;
                current.slaves[n].esiFile=descriptions[n].file;current.slaves[n].esiStatus=descriptions[n].status;
                current.slaves[n].esiSdoDictionary=descriptions[n].dictionary;
            }
        }
        current.generation=generation;current.completed=lastCompleted;current.result=lastResult;current.busy=busy;
        current.cycles=cycles;current.missed=missed;current.actualCycleUs=actualCycle;current.maxJitterUs=maxJitter;
        // Keep live transport health from the backend; the last command result
        // is a separate field and must never hide a later stale/WKC failure.
        {
            std::lock_guard<std::mutex> lock(mutex);
            current.logs=logs;published=current;scriptPublished=current;
            notifyScriptUpdate();
        }
        nextPublication=Clock::now()+std::chrono::milliseconds(100);
    }
    void publishScriptSnapshot() {
        if(!scriptSnapshots.load(std::memory_order_relaxed))return;
        Snapshot fast=backend->snapshot();
        fast.connection.mode=mode;fast.simulation=mode==ConnectionMode::Simulation;fast.eeprom=eeprom;fast.foe=foe;
        fast.generation=generation;fast.completed=lastCompleted;fast.result=lastResult;fast.busy=false;
        fast.cycles=cycles;fast.missed=missed;fast.actualCycleUs=actualCycle;fast.maxJitterUs=maxJitter;
        std::lock_guard<std::mutex> lock(mutex);
        fast.logs=logs;
        fast.esiDirectory=published.esiDirectory;fast.esiStatus=published.esiStatus;
        fast.esiFiles=published.esiFiles;fast.esiDevices=published.esiDevices;fast.descriptionRevision=published.descriptionRevision;
        if(fast.slaves.size()==published.slaves.size())for(size_t n=0;n<fast.slaves.size();++n) {
            fast.slaves[n].pdoEntries=published.slaves[n].pdoEntries;
            fast.slaves[n].esiFile=published.slaves[n].esiFile;fast.slaves[n].esiStatus=published.slaves[n].esiStatus;
            fast.slaves[n].esiSdoDictionary=published.slaves[n].esiSdoDictionary;
        }
        scriptPublished=std::move(fast);
        notifyScriptUpdate();
    }
    void markBusy() {
        std::lock_guard<std::mutex> lock(mutex);published.busy=true;published.generation=generation;
    }
    void resetTiming() {
        cycles=missed=0;actualCycle=maxJitter=0;previousCycle={};nextCycle={};
    }
    Result selectMode(ConnectionMode next) {
        if(current.connected)return {false,"Disconnect before changing operating mode.",{}};
        if(scriptRunning.load())return {false,"Stop the script before changing operating mode.",{}};
        if(next!=ConnectionMode::Simulation&&next!=ConnectionMode::Hardware&&next!=ConnectionMode::Programming)
            return {false,"Unknown operating mode.",{}};
        if(next==mode)return {true,"Operating mode unchanged.",{}};
        backend->close();clearTrendSelection();mode=next;
        backend=mode==ConnectionMode::Simulation?makeSimulation():makeSoem();
        ++generation;resetTiming();markBusy();eeprom={};foe={};descriptions.clear();library={};descriptionDirty=true;
        current=backend->snapshot();current.connection.mode=mode;
        // Enumerating host interfaces opens no capture handle and sends no
        // EtherCAT frames. Selecting a mode never connects or scans a bus.
        auto adapters=backend->adapters();
        if(!adapters.ok)log(adapters);
        return {true,mode==ConnectionMode::Programming?"Programming mode selected; choose an interface and scan.":
            mode==ConnectionMode::Simulation?"Simulation mode selected.":"Hardware mode selected.",{}};
    }
    EepromControl eepromControl() {
        return {[this]{return cancelActive.load()||shutdown.load();},[this](const EepromProgress& progress){
            const bool changed=eeprom.stage!=progress.stage;
            const auto request=eeprom.request;const auto slave=eeprom.slave;
            eeprom=progress;eeprom.request=request;eeprom.slave=slave;
            if(changed&&!progress.detail.empty())appendLog(progress.detail,progress.stage==EepromStage::Failed);
            publish(true);
        }};
    }
    Result finishEeprom(Result result) {
        if(!result.ok) {
            if(eeprom.stage!=EepromStage::Failed)eeprom.stage=cancelActive.load()||shutdown.load()?EepromStage::Cancelled:EepromStage::Failed;
        } else eeprom.stage=EepromStage::Complete;
        eeprom.detail=result.message;
        return result;
    }
    Result eepromJob(const Command& command) {
        eeprom={EepromStage::Preparing,0,command.kind==CommandKind::WriteEeprom?command.data.size():size_t(std::max(0,command.length)),false,{}};
        eeprom.request=activeRequest;eeprom.slave=command.slave;
        auto fail=[this](const char* message){return finishEeprom({false,message,{}});};
        if(mode!=ConnectionMode::Programming)return fail("Select Programming mode before EEPROM access.");
        if(scriptRunning.load())return fail("Stop the script before EEPROM access.");
        if(!current.connected||current.running||current.mapped)return fail("Scan in Programming mode before EEPROM access.");
        const auto target=std::find_if(current.slaves.begin(),current.slaves.end(),[&](const Slave& slave){return slave.position==command.slave;});
        if(target==current.slaves.end()||(target->state&15)!=1)return fail("EEPROM access requires a scanned slave with confirmed INIT.");
        if(command.length<128||command.length>131072||(command.length&1))return fail("Confirm an even EEPROM capacity from 128 to 131072 bytes.");
        if(target->eepromSizeKnown&&size_t(command.length)>target->eepromCapacity)return fail("The requested range exceeds the target's declared EEPROM capacity.");
        if(cancelActive.load()||shutdown.load())return fail("EEPROM operation cancelled before access.");
        if(command.kind==CommandKind::WriteEeprom) {
            sii::Image image;std::string error;
            if(!sii::fromBin(command.data,image,error))return finishEeprom({false,"Invalid EEPROM image: "+error,{}});
            if(image.bytes!=command.data)return fail("Prepare the complete padded image before programming.");
            if(image.bytes.size()>size_t(command.length))return fail("Image exceeds the confirmed target capacity.");
            // Reprogramming may intentionally replace the SII identity. The
            // backend binds the physical target to the scan, not the new IDs.
            // Any attempted programming invalidates handles even when a later
            // cancellation makes the exact persistent outcome uncertain.
            ++generation;descriptionDirty=true;descriptions.clear();library={};markBusy();
            return finishEeprom(backend->writeEeprom(command.slave,command.data,eepromControl()));
        }
        return finishEeprom(backend->readEeprom(command.slave,size_t(command.length),eepromControl()));
    }
    FoeControl foeControl() {
        return {[this]{return cancelActive.load()||shutdown.load();},[this](const FoeProgress& progress){
            const bool changed=foe.stage!=progress.stage;
            const auto request=foe.request;const auto slave=foe.slave;const auto direction=foe.direction;
            foe=progress;foe.request=request;foe.slave=slave;foe.direction=direction;
            if(changed&&!progress.detail.empty())appendLog(progress.detail,progress.stage==FoeStage::Failed);
            publish(true);
        }};
    }
    Result finishFoe(Result result) {
        // Files are streamed to disk by the backend. Never retain them in the
        // bounded command history or periodically copied snapshots.
        std::vector<uint8_t>{}.swap(result.data);
        if(result.ok)foe.stage=FoeStage::Complete;
        else if(foe.stage!=FoeStage::Failed&&foe.stage!=FoeStage::Cancelled)
            foe.stage=cancelActive.load()||shutdown.load()?FoeStage::Cancelled:FoeStage::Failed;
        foe.detail=result.message;
        return result;
    }
    Result foeJob(const Command& command) {
        foe={};foe.stage=FoeStage::Preparing;foe.request=activeRequest;foe.slave=command.slave;
        foe.direction=command.kind==CommandKind::FoeUpload?FoeDirection::Upload:FoeDirection::Download;
        const auto fail=[](const char* message){return Result{false,message,{}};};
        if(mode==ConnectionMode::Programming)return fail("FoE is unavailable in Programming mode.");
        if(scriptRunning.load())return fail("Stop the script before FoE transfer.");
        if(!current.connected)return fail("Connect and scan before FoE transfer.");
        if(current.running||std::any_of(current.slaves.begin(),current.slaves.end(),[](const Slave& slave){return (slave.state&15)==8;}))
            return fail("Switch all slaves to PRE-OP and stop cyclic I/O before FoE transfer.");
        const auto target=std::find_if(current.slaves.begin(),current.slaves.end(),[&](const Slave& slave){return slave.position==command.slave;});
        if(target==current.slaves.end()||target->alCode||(target->state!=2&&target->state!=3))
            return fail("FoE transfer requires a selected slave with confirmed PRE-OP or BOOT and no AL error.");
        if(cancelActive.load()||shutdown.load())return fail("FoE transfer cancelled before access.");
        publish(true);
        return backend->foeTransfer(command.slave,foe.direction,command.foe,foeControl());
    }
    Result dispatch(const Command& command) {
        if(command.kind==CommandKind::SetMode)return selectMode(command.connection.mode);
        if(command.kind==CommandKind::RefreshAdapters) {
            if(current.running)return {false,"Stop cyclic exchange before refreshing adapters.",{}};
            const bool sameBackend=(command.connection.mode==ConnectionMode::Simulation)==(mode==ConnectionMode::Simulation);
            if(!sameBackend) {
                // Scripts can discover wired interfaces while Simulation is
                // selected, without replacing or disconnecting its bus.
                auto enumerator=command.connection.mode==ConnectionMode::Simulation?makeSimulation():makeSoem();
                auto result=enumerator->adapters();result.adapters=enumerator->snapshot().adapters;return result;
            }
            auto result=backend->adapters();result.adapters=backend->snapshot().adapters;return result;
        }
        if(command.kind==CommandKind::Scan&&command.connection.mode!=mode) {
            const auto changed=selectMode(command.connection.mode);if(!changed.ok)return changed;
        }
        if(mode==ConnectionMode::Programming&&command.kind!=CommandKind::RefreshAdapters&&command.kind!=CommandKind::Scan&&
           command.kind!=CommandKind::Disconnect&&command.kind!=CommandKind::ReadEeprom&&command.kind!=CommandKind::WriteEeprom)
            return {false,"Ordinary workbench and script operations are unavailable in Programming mode.",{}};
        // The AL Control register is another entry point into the same BOOT
        // transition. It must use the same script lifecycle gate as the button.
        if(entersBoot(command)&&scriptRunning.load())return {false,"Stop the script before entering BOOT.",{}};
        switch(command.kind) {
        case CommandKind::SetMode:break;
        case CommandKind::RefreshAdapters:break;
        case CommandKind::Scan:
            if(current.running)return {false,"Stop cyclic exchange before scanning devices.",{}};
            if(mode==ConnectionMode::Programming&&scriptRunning.load())return {false,"Stop the script before a programming scan.",{}};
            if(mode==ConnectionMode::Programming&&(command.connection.redundant||command.connection.overlap))return {false,"Programming requires a single interface without PDO mapping.",{}};
            ++generation;resetTiming();markBusy();
            eeprom={};foe={};descriptionDirty=true;backend->clearTrend();
            clearTrendSelection();
            if(mode==ConnectionMode::Programming)return backend->scanProgramming(command.connection,eepromControl());
            return backend->scan(command.connection);
        case CommandKind::Disconnect:
            if(current.running) {auto stop=backend->stop();if(!stop.ok)log(stop);}
            backend->close();clearTrendSelection();++generation;resetTiming();markBusy();
            return {true,"Disconnected; queued commands from the previous connection are invalid.",{}};
        case CommandKind::ReadEeprom:case CommandKind::WriteEeprom:return eepromJob(command);
        case CommandKind::FoeUpload:case CommandKind::FoeDownload:return foeJob(command);
        case CommandKind::State:
            if(current.running&&(command.state==AlState::Init||command.state==AlState::PreOp)) {
                auto stopped=backend->stop();log(stopped);if(!stopped.ok)return stopped;
            }
            return backend->state(command.slave,command.state);
        case CommandKind::SetCycle:
            return backend->setCycle(command.connection.cycleUs);
        case CommandKind::Start:return backend->start();
        case CommandKind::Stop:return backend->stop();
        case CommandKind::ReadRegister:
            if(current.running)return {false,"Stop cyclic exchange before register access.",{}};
            return backend->readRegister(command.slave,command.address,command.length);
        case CommandKind::WriteRegister:
            if(current.running)return {false,"Stop cyclic exchange before register access.",{}};
            return backend->writeRegister(command.slave,command.address,command.data);
        case CommandKind::ReadSdo:
            if(current.running)return {false,"Stop cyclic exchange before synchronous SDO access.",{}};
            return backend->readSdo(command.slave,command.index,command.subindex,command.length);
        case CommandKind::WriteSdo:
            if(current.running)return {false,"Stop cyclic exchange before synchronous SDO access.",{}};
            return backend->writeSdo(command.slave,command.index,command.subindex,command.data);
        case CommandKind::ReadSdoDictionary: {
            if(current.running)return {false,"Stop cyclic exchange before reading the object dictionary.",{}};
            const auto selected=std::find_if(current.slaves.begin(),current.slaves.end(),[&](const Slave& slave){return slave.position==command.slave;});
            if(!current.connected||selected==current.slaves.end())return {false,"Select a connected slave first.",{}};
            if(selected->state!=2)return {false,"Object dictionary discovery requires confirmed PRE-OP.",{}};
            return backend->readSdoDictionary(command.slave);
        }
        case CommandKind::WriteOutput:return backend->writeOutput(command.slave,command.offset,command.data);
        }
        return {false,"Unknown command.",{}};
    }
    void execute(Queued queued) {
        markBusy();
        if(activeFoe) {
            foe={};foe.stage=FoeStage::Preparing;foe.request=queued.id;foe.slave=queued.command.slave;
            foe.direction=queued.command.kind==CommandKind::FoeUpload?FoeDirection::Upload:FoeDirection::Download;
            if(queued.canceled)foe.stage=FoeStage::Cancelled;
        }
        try {
            if(queued.canceled)lastResult={false,"Queued command canceled by a stop or disconnect request.",{}};
            else if(queued.command.generation!=generation&&queued.command.kind!=CommandKind::Disconnect)lastResult={false,"Stale command rejected: the connection generation changed.",{}};
            else lastResult=dispatch(queued.command);
        } catch(const std::exception& error) {
            lastResult={false,std::string("Backend error: ")+error.what(),{}};
            try{backend->close();}catch(...){}
            ++generation;resetTiming();
            if(activeEeprom){eeprom.stage=EepromStage::Failed;eeprom.detail=lastResult.message;}
            if(activeFoe){foe.stage=FoeStage::Failed;foe.detail=lastResult.message;}
        } catch(...) {
            lastResult={false,"Unknown backend error; connection closed.",{}};
            try{backend->close();}catch(...){}
            ++generation;resetTiming();
            if(activeEeprom){eeprom.stage=EepromStage::Failed;eeprom.detail=lastResult.message;}
            if(activeFoe){foe.stage=FoeStage::Failed;foe.detail=lastResult.message;}
        }
        if(activeFoe)lastResult=finishFoe(std::move(lastResult));
        lastCompleted=queued.id;log(lastResult);
        const bool wasRunning=current.running;
        publish();
        if(lastResult.ok&&queued.command.kind==CommandKind::Scan&&mode==ConnectionMode::Hardware) {
            for(auto& message:esi.scanDiagnostics(current))appendLog(std::move(message),false);
        }
        if(!current.running) {previousCycle={};nextCycle={};}
        else if(!wasRunning||nextCycle==Clock::time_point{})nextCycle=Clock::now();
        // Publish the backend state before making the result visible to
        // Bridge callers. This keeps result() and snapshot() consistent for
        // state transitions that start or stop cyclic exchange.
        {
            std::lock_guard<std::mutex> lock(mutex);
            resultHistory[queued.id]=lastResult;
            activeRequest=0;
            activeEeprom=false;
            activeFoe=false;
            activeBoot=false;
            while(resultHistory.size()>512)resultHistory.erase(resultHistory.begin());
            // A command may be the last event before the bus goes idle.
            // Wake again now that result() can see its completion.
            notifyScriptUpdate();
        }
    }
    void exchange() {
        const auto started=Clock::now();
        const auto period=std::chrono::microseconds(std::max(500,current.connection.cycleUs));
        if(previousCycle!=Clock::time_point{}) {
            actualCycle=std::chrono::duration<double,std::micro>(started-previousCycle).count();
            maxJitter=std::max(maxJitter,std::abs(actualCycle-double(period.count())));
        }
        previousCycle=started;
        try {
            backend->cycle();++cycles;
            backend->sampleTrend();
            for(const auto& notice:backend->takeDiagnostics())log(notice);
        }
        catch(const std::exception& error) {
            lastResult={false,std::string("Cyclic exchange failed: ")+error.what(),{}};log(lastResult);
            try{backend->stop();}catch(...){}
            publish();return;
        }
        nextCycle+=period;
        const auto completed=Clock::now();
        if(completed>nextCycle) {
            auto skipped=(completed-nextCycle)/period+1;
            missed+=uint64_t(skipped);nextCycle+=period*skipped;
        }
        if(completed>=nextPublication)publish();
        else publishScriptSnapshot();
    }
    void run() {
        try {
            backend=makeSoem();log(backend->adapters());log({true,"I/O worker ready. Hardware traffic begins only after selecting an interface and scanning.",{}});publish();
            while(true) {
                Queued queued{};bool haveCommand=false,cycleDue=false,readDescription=false,applyTrendSelection=false;
                std::vector<std::string> selectedTrends;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    auto canReadDescription=[this]{
                        return mode==ConnectionMode::Hardware&&current.connected&&current.mapped&&
                            !scriptRunning.load()&&backend->hasPendingPdoDescriptions();
                    };
                    if(!current.running)ready.wait(lock,[&]{
                        return quitting||!urgent.empty()||!normal.empty()||trendSelectionDirty||canReadDescription();
                    });
                    else if(urgent.empty()&&normal.empty())waitForCycle(lock);
                    if(quitting)break;
                    if(trendSelectionDirty) {
                        selectedTrends=trendSelection;trendSelectionDirty=false;applyTrendSelection=true;
                    }
                    cycleDue=current.running&&Clock::now()>=nextCycle;
                    if(!urgent.empty()) {queued=std::move(urgent.front());urgent.pop_front();haveCommand=true;}
                    else if(!cycleDue&&!normal.empty()) {queued=std::move(normal.front());normal.pop_front();haveCommand=true;}
                    else if(!current.running)readDescription=canReadDescription();
                    if(haveCommand) {
                        activeRequest=queued.id;cancelActive.store(false);
                        activeEeprom=queued.command.kind==CommandKind::ReadEeprom||queued.command.kind==CommandKind::WriteEeprom||
                            (queued.command.kind==CommandKind::Scan&&queued.command.connection.mode==ConnectionMode::Programming);
                        activeFoe=queued.command.kind==CommandKind::FoeUpload||queued.command.kind==CommandKind::FoeDownload;
                        activeBoot=entersBoot(queued.command);
                    }
                }
                if(applyTrendSelection)backend->setTrendSelection(selectedTrends);
                if(haveCommand)execute(std::move(queued));
                else if(cycleDue)exchange();
                else if(applyTrendSelection)publish();
                else if(readDescription) {
                    if(backend->servicePdoDescriptions())descriptionDirty=true;
                    if(!backend->hasPendingPdoDescriptions()||(descriptionDirty&&Clock::now()>=nextPublication))publish();
                }
            }
            if(current.running)backend->stop();
            backend->close();
        } catch(const std::exception& error) {
            std::lock_guard<std::mutex> lock(mutex);published.busy=false;published.running=false;published.connected=false;
            published.result={false,std::string("I/O worker failed: ")+error.what(),{}};published.status=published.result.message;
            scriptPublished=published;
            notifyScriptUpdate();
        }
    }
};

Controller::Controller():impl_(std::make_unique<Impl>()){}
Controller::~Controller()=default;
bool Controller::cancel(uint64_t request) {
    if(!request)return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->activeRequest==request&&(impl_->activeEeprom||impl_->activeFoe)){impl_->cancelActive.store(true);return true;}
    for(auto* queue:{&impl_->urgent,&impl_->normal}) {
        for(auto& queued:*queue) if(queued.id==request) {queued.canceled=true;return true;}
    }
    return false;
}
uint64_t Controller::submit(Command command) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->activeFoe&&command.kind!=CommandKind::Disconnect)return 0;
    const auto queued=impl_->urgent.size()+impl_->normal.size();
    // Reserve eight slots for Stop/Disconnect/lower-state requests even under a full normal queue.
    if(impl_->quitting||queued>=128||(!priority(command)&&queued>=120))return 0;
    if(command.generation==0)command.generation=impl_->published.generation;
    if(priority(command)) {
        if(command.kind==CommandKind::Disconnect&&(impl_->activeEeprom||impl_->activeFoe))impl_->cancelActive.store(true);
        // Prevent an already queued OP/Start from undoing a user's subsequent stop.
        for(auto* queue:{&impl_->urgent,&impl_->normal})for(auto& entry:*queue)
            if(command.kind==CommandKind::Disconnect||raisesState(entry.command))entry.canceled=true;
    }
    const auto id=impl_->nextId++;
    auto& queue=priority(command)?impl_->urgent:impl_->normal;
    queue.push_back({id,std::move(command),false});impl_->notify();return id;
}
Snapshot Controller::snapshot() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->published;}
void Controller::setTrendSelection(std::vector<std::string> keys) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->quitting)return;
    impl_->trendSelection=std::move(keys);impl_->trendSelectionDirty=true;impl_->notify();
}
void Controller::enableScriptSnapshots(bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->scriptSnapshots.exchange(enabled,std::memory_order_relaxed)==enabled)return;
    ++impl_->scriptVersion;
    impl_->scriptUpdated.notify_all();
}
bool Controller::waitForScriptUpdate(uint64_t& version,Snapshot& snapshot) const {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->scriptUpdated.wait(lock,[&]{
        return impl_->quitting||!impl_->scriptSnapshots.load(std::memory_order_relaxed)||impl_->scriptVersion!=version;
    });
    if(impl_->quitting||!impl_->scriptSnapshots.load(std::memory_order_relaxed))return false;
    version=impl_->scriptVersion;
    snapshot=impl_->scriptPublished;
    return true;
}
bool Controller::setScriptRunning(bool running) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(running&&(impl_->activeFoe||impl_->activeBoot))return false;
    impl_->scriptRunning.store(running);
    if(!running)impl_->ready.notify_one();
    return true;
}
Snapshot Controller::scriptSnapshot() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->scriptSnapshots.load(std::memory_order_relaxed)?impl_->scriptPublished:impl_->published;}
void Controller::appendLog(std::string message,bool error) { impl_->appendLog(std::move(message),error); }
void Controller::clearLogs() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->logs.clear();
    impl_->published.logs.clear();
    impl_->scriptPublished.logs.clear();
}
bool Controller::result(uint64_t request,Result& value) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found=impl_->resultHistory.find(request);
    if(found==impl_->resultHistory.end())return false;
    value=found->second;return true;
}
bool Controller::pending(uint64_t request) const {
    if(!request)return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->activeRequest==request)return true;
    for(const auto* queue:{&impl_->urgent,&impl_->normal})
        for(const auto& queued:*queue)if(queued.id==request)return true;
    return false;
}
} // namespace niceecatm

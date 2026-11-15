// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "master/controller.hpp"
#include "esi/catalog.hpp"
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
}

struct Controller::Impl {
    struct Queued {uint64_t id;Command command;bool canceled=false;};
    mutable std::mutex mutex;
    std::condition_variable ready;
#ifdef _WIN32
    CycleWait cycleWait;
#endif
    std::deque<Queued> urgent,normal;
    std::thread worker;
    bool quitting=false;
    uint64_t nextId=1,generation=1,lastCompleted=0,logSequence=0;
    Snapshot published,current,scriptPublished;
    std::atomic<bool> scriptSnapshots{false};
    Result lastResult;
    std::map<uint64_t,Result> resultHistory;
    std::vector<LogEntry> logs;
    std::unique_ptr<Backend> backend;
    EsiCatalog esi{executableDirectory()/"esi"};
    struct Description {
        std::vector<PdoEntry> entries;
        std::string file,status;
    };
    std::vector<Description> descriptions;
    Snapshot library;
    bool descriptionDirty=true,descriptionMapped=false;
    uint64_t descriptionGeneration=0;
    bool simulation=true;
    uint64_t cycles=0,missed=0;
    double actualCycle=0,maxJitter=0;
    Clock::time_point nextCycle{},previousCycle{},nextPublication{};

    Impl() {
        published.generation=generation;
        scriptPublished=published;
        worker=std::thread([this]{run();});
    }
    ~Impl() {
        {std::lock_guard<std::mutex> lock(mutex);quitting=true;}
        notify();if(worker.joinable())worker.join();
    }
    void notify() {
        ready.notify_one();
#ifdef _WIN32
        cycleWait.notify();
#endif
    }
    void waitForCycle(std::unique_lock<std::mutex>& lock) {
        const auto canWake=[this]{return quitting||!urgent.empty()||!normal.empty();};
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
        // ESI matching is static until scan/import/invalidation. Do it while
        // stopped, then only copy the resulting metadata into live snapshots.
        // A cyclic publication must not search a potentially large XML catalog.
        if(descriptionDirty||descriptionGeneration!=generation||descriptionMapped!=current.mapped||descriptions.size()!=current.slaves.size()) {
            esi.decorate(current);
            descriptions.clear();descriptions.reserve(current.slaves.size());
            for(const auto& slave:current.slaves)descriptions.push_back({slave.pdoEntries,slave.esiFile,slave.esiStatus});
            library.esiDirectory=current.esiDirectory;library.esiStatus=current.esiStatus;
            library.esiFiles=current.esiFiles;library.esiDevices=current.esiDevices;library.descriptionRevision=current.descriptionRevision;
            descriptionDirty=false;descriptionGeneration=generation;descriptionMapped=current.mapped;
        } else {
            current.esiDirectory=library.esiDirectory;current.esiStatus=library.esiStatus;
            current.esiFiles=library.esiFiles;current.esiDevices=library.esiDevices;current.descriptionRevision=library.descriptionRevision;
            for(size_t n=0;n<current.slaves.size();++n) {
                current.slaves[n].pdoEntries=descriptions[n].entries;
                current.slaves[n].esiFile=descriptions[n].file;current.slaves[n].esiStatus=descriptions[n].status;
            }
        }
        current.generation=generation;current.completed=lastCompleted;current.result=lastResult;current.busy=busy;
        current.cycles=cycles;current.missed=missed;current.actualCycleUs=actualCycle;current.maxJitterUs=maxJitter;
        // Keep live transport health from the backend; the last command result
        // is a separate field and must never hide a later stale/WKC failure.
        {std::lock_guard<std::mutex> lock(mutex);current.logs=logs;published=current;scriptPublished=current;}
        nextPublication=Clock::now()+std::chrono::milliseconds(100);
    }
    void publishScriptSnapshot() {
        if(!scriptSnapshots.load(std::memory_order_relaxed))return;
        Snapshot fast=backend->snapshot();
        fast.generation=generation;fast.completed=lastCompleted;fast.result=lastResult;fast.busy=false;
        fast.cycles=cycles;fast.missed=missed;fast.actualCycleUs=actualCycle;fast.maxJitterUs=maxJitter;
        std::lock_guard<std::mutex> lock(mutex);
        fast.logs=logs;
        fast.esiDirectory=published.esiDirectory;fast.esiStatus=published.esiStatus;
        fast.esiFiles=published.esiFiles;fast.esiDevices=published.esiDevices;fast.descriptionRevision=published.descriptionRevision;
        if(fast.slaves.size()==published.slaves.size())for(size_t n=0;n<fast.slaves.size();++n) {
            fast.slaves[n].pdoEntries=published.slaves[n].pdoEntries;
            fast.slaves[n].esiFile=published.slaves[n].esiFile;fast.slaves[n].esiStatus=published.slaves[n].esiStatus;
        }
        scriptPublished=std::move(fast);
    }
    void markBusy() {
        std::lock_guard<std::mutex> lock(mutex);published.busy=true;published.generation=generation;
    }
    void resetTiming() {
        cycles=missed=0;actualCycle=maxJitter=0;previousCycle={};nextCycle={};
    }
    Result dispatch(const Command& command) {
        if(command.kind==CommandKind::RefreshAdapters||command.kind==CommandKind::Scan) {
            if(command.connection.simulation!=simulation) {
                if(current.running)backend->stop();
                backend->close();
                simulation=command.connection.simulation;
                backend=simulation?makeSimulation():makeSoem();
                ++generation;resetTiming();markBusy();
                current=backend->snapshot();
            }
        }
        switch(command.kind) {
        case CommandKind::RefreshAdapters:
            if(current.running)return {false,"Stop cyclic exchange before refreshing adapters.",{}};
            return backend->adapters();
        case CommandKind::Scan:
            if(current.running)return {false,"Stop cyclic exchange before scanning devices.",{}};
            ++generation;resetTiming();markBusy();
            return backend->scan(command.connection);
        case CommandKind::Disconnect:
            if(current.running) {auto stop=backend->stop();if(!stop.ok)log(stop);}
            backend->close();++generation;resetTiming();markBusy();
            return {true,"Disconnected; queued commands from the previous connection are invalid.",{}};
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
        case CommandKind::WriteOutput:return backend->writeOutput(command.slave,command.offset,command.data);
        }
        return {false,"Unknown command.",{}};
    }
    void execute(Queued queued) {
        markBusy();
        try {
            if(queued.canceled)lastResult={false,"Queued command canceled by a stop or disconnect request.",{}};
            else if(queued.command.generation!=generation)lastResult={false,"Stale command rejected: the connection generation changed.",{}};
            else lastResult=dispatch(queued.command);
        } catch(const std::exception& error) {
            lastResult={false,std::string("Backend error: ")+error.what(),{}};
            try{backend->close();}catch(...){}
            ++generation;resetTiming();
        } catch(...) {
            lastResult={false,"Unknown backend error; connection closed.",{}};
            try{backend->close();}catch(...){}
            ++generation;resetTiming();
        }
        lastCompleted=queued.id;log(lastResult);
        const bool wasRunning=current.running;
        publish();
        if(!current.running) {previousCycle={};nextCycle={};}
        else if(!wasRunning||nextCycle==Clock::time_point{})nextCycle=Clock::now();
        // Publish the backend state before making the result visible to
        // Bridge callers. This keeps result() and snapshot() consistent for
        // state transitions that start or stop cyclic exchange.
        {
            std::lock_guard<std::mutex> lock(mutex);
            resultHistory[queued.id]=lastResult;
            while(resultHistory.size()>512)resultHistory.erase(resultHistory.begin());
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
        publishScriptSnapshot();
        if(completed>=nextPublication)publish();
    }
    void run() {
        try {
            backend=makeSimulation();log({true,"I/O worker ready. Hardware traffic begins only after selecting an interface and scanning.",{}});publish();
            while(true) {
                Queued queued{};bool haveCommand=false,cycleDue=false;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    auto canWake=[this]{return quitting||!urgent.empty()||!normal.empty();};
                    if(!current.running)ready.wait(lock,canWake);
                    else if(urgent.empty()&&normal.empty())waitForCycle(lock);
                    if(quitting)break;
                    cycleDue=current.running&&Clock::now()>=nextCycle;
                    if(!urgent.empty()) {queued=std::move(urgent.front());urgent.pop_front();haveCommand=true;}
                    else if(!cycleDue&&!normal.empty()) {queued=std::move(normal.front());normal.pop_front();haveCommand=true;}
                }
                if(haveCommand)execute(std::move(queued));else if(cycleDue)exchange();
            }
            if(current.running)backend->stop();
            backend->close();
        } catch(const std::exception& error) {
            std::lock_guard<std::mutex> lock(mutex);published.busy=false;published.running=false;published.connected=false;
            published.result={false,std::string("I/O worker failed: ")+error.what(),{}};published.status=published.result.message;
        }
    }
};

Controller::Controller():impl_(std::make_unique<Impl>()){}
Controller::~Controller()=default;
bool Controller::cancel(uint64_t request) {
    if(!request)return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for(auto* queue:{&impl_->urgent,&impl_->normal}) {
        for(auto& queued:*queue) if(queued.id==request) {queued.canceled=true;return true;}
    }
    return false;
}
uint64_t Controller::submit(Command command) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto queued=impl_->urgent.size()+impl_->normal.size();
    // Reserve eight slots for Stop/Disconnect/lower-state requests even under a full normal queue.
    if(impl_->quitting||queued>=128||(!priority(command)&&queued>=120))return 0;
    if(command.generation==0)command.generation=impl_->published.generation;
    if(priority(command)) {
        // Prevent an already queued OP/Start from undoing a user's subsequent stop.
        for(auto* queue:{&impl_->urgent,&impl_->normal})for(auto& entry:*queue)
            if(command.kind==CommandKind::Disconnect||raisesState(entry.command))entry.canceled=true;
    }
    const auto id=impl_->nextId++;
    auto& queue=priority(command)?impl_->urgent:impl_->normal;
    queue.push_back({id,std::move(command),false});impl_->notify();return id;
}
Snapshot Controller::snapshot() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->published;}
void Controller::enableScriptSnapshots(bool enabled) { impl_->scriptSnapshots.store(enabled,std::memory_order_relaxed); }
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
} // namespace niceecatm

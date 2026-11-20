// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "script/runtime.hpp"

#include "model/types.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace niceecatm::script {
namespace {
using Clock=std::chrono::steady_clock;

std::string nowText() {
    const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local,&now);
#else
    localtime_r(&now,&local);
#endif
    std::ostringstream out;out<<std::put_time(&local,"%H:%M:%S");return out.str();
}

struct Allocation { size_t size; };

struct BusUserdata;
struct SignalUserdata;

} // namespace

struct Runtime::Impl {
    struct Timer {
        uint64_t id=0;
        bool repeating=false, cycle=false;
        uint32_t periodMs=0;
        uint64_t cycleEvery=0;
        Clock::time_point next{};
        int functionRef=LUA_NOREF;
        uint64_t lastCycles=0;
    };

    RuntimeOptions options;
    HostCallbacks callbacks;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::thread thread;
    std::atomic<bool> stopRequested{false};
    bool loaded=false;
    std::string name, source;
    Status state=Status::Empty;
    std::string error;
    std::atomic<uint64_t> instructionCount{0}, callbackCount{0};
    // The total remains available for diagnostics, while the guard applies
    // to one top-level chunk or callback, not the script's entire lifetime.
    uint64_t executionInstructionCount=0;
    uint64_t logSequence=0, nextTimer=1;
    std::vector<LogEvent> logEntries;
    std::vector<TimerView> timerEntries;
    std::map<uint64_t,HostResult> pending;
    Snapshot liveSnapshot;
    lua_State* lua=nullptr;

    explicit Impl(HostCallbacks c, RuntimeOptions o):options(o),callbacks(std::move(c)) {}
    ~Impl() { stop(); }

    void setState(Status s, std::string message={}) {
        std::lock_guard<std::mutex> lock(mutex);state=s;if(!message.empty())error=std::move(message);
    }
    void log(LogLevel level, std::string message) {
        if(message.empty())return;
        std::lock_guard<std::mutex> lock(mutex);
        logEntries.push_back({++logSequence,nowText(),level,std::move(message)});
        if(logEntries.size()>options.maxLogEntries)logEntries.erase(logEntries.begin(),logEntries.begin()+static_cast<std::ptrdiff_t>(logEntries.size()-options.maxLogEntries));
    }
    HostResult perform(const ScriptRequest& request) {
        if(stopRequested.load())return {false,"Script stopped.",{}};
        if(callbacks.execute) {
            HostResult immediate;
            try { if(callbacks.execute(request,immediate))return immediate; }
            catch(const std::exception& e) { return {false,e.what(),{}}; }
        }
        if(!callbacks.submit)return {false,"No EtherCAT host is attached.",{}};
        uint64_t id=0;
        try { id=callbacks.submit(request); } catch(const std::exception& e) { return {false,e.what(),{}}; }
        if(id==0)return {false,"Host rejected the script request.",{}};
        std::unique_lock<std::mutex> lock(mutex);
        state=Status::Waiting;
        const auto ready=[this,id]{return stopRequested.load()||pending.find(id)!=pending.end();};
        condition.wait_for(lock,std::chrono::milliseconds(options.hostTimeoutMs),ready);
        if(stopRequested.load())return {false,"Script stopped while waiting for host.",{}};
        const auto found=pending.find(id);
        if(found==pending.end()) { state=Status::Running;return {false,"Host request timed out.",{}}; }
        HostResult result=std::move(found->second);pending.erase(found);state=Status::Running;return result;
    }
    void complete(uint64_t id, HostResult result) {
        if(id==0)return;
        std::lock_guard<std::mutex> lock(mutex);
        pending[id]=std::move(result);
        while(pending.size()>512)pending.erase(pending.begin());
        condition.notify_all();
    }
    void update(const Snapshot& snapshot) {
        std::lock_guard<std::mutex> lock(mutex);liveSnapshot=snapshot;condition.notify_all();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);stopRequested=true;condition.notify_all();
        }
        if(thread.joinable())thread.join();
        if(lua){lua_close(lua);lua=nullptr;}
        std::lock_guard<std::mutex> lock(mutex);
        if(state==Status::Running||state==Status::Waiting)state=Status::Stopped;
    }

    static void* allocator(void* user, void* oldPtr, size_t oldSize, size_t newSize) {
        auto* self=static_cast<Impl*>(user);(void)oldSize;
        if(!oldPtr) {
            if(newSize==0)return nullptr;
            if(newSize>self->options.maxMemoryBytes)return nullptr;
            auto* block=static_cast<Allocation*>(std::malloc(sizeof(Allocation)+newSize));if(!block)return nullptr;
            block->size=newSize;self->memoryUsed.fetch_add(newSize);return block+1;
        }
        auto* block=static_cast<Allocation*>(oldPtr)-1;const size_t old=block->size;
        if(newSize==0){self->memoryUsed.fetch_sub(old);std::free(block);return nullptr;}
        const size_t used=self->memoryUsed.load();if(newSize>old&&newSize-old>self->options.maxMemoryBytes-used)return nullptr;
        auto* replacement=static_cast<Allocation*>(std::realloc(block,sizeof(Allocation)+newSize));if(!replacement)return nullptr;
        replacement->size=newSize;
        if(newSize>old)self->memoryUsed.fetch_add(newSize-old);else self->memoryUsed.fetch_sub(old-newSize);
        return replacement+1;
    }
    std::atomic<size_t> memoryUsed{0};

    static Impl* from(lua_State* state) { Impl* self=nullptr;std::memcpy(&self,lua_getextraspace(state),sizeof(self));return self; }
    static void instructionHook(lua_State* state, lua_Debug*) {
        auto* self=from(state);if(!self)return;
        ++self->instructionCount;
        ++self->executionInstructionCount;
        if(self->stopRequested.load())luaL_error(state,"script stopped");
        if(self->executionInstructionCount>self->options.maxInstructions)luaL_error(state,"script instruction budget exceeded");
    }

    static bool duration(lua_State* state,int index,uint32_t maxMs,uint32_t& output) {
        double value=0;std::string suffix="ms";
        if(lua_isnumber(state,index))value=lua_tonumber(state,index);
        else if(lua_isstring(state,index)) {
            size_t length=0;const char* text=lua_tolstring(state,index,&length);std::string input(text,length);
            size_t pos=0;try { value=std::stod(input,&pos); } catch(...) { return false; }
            suffix=input.substr(pos);if(suffix.empty())suffix="ms";
        } else return false;
        if(suffix=="us")value/=1000.0;else if(suffix=="s")value*=1000.0;else if(suffix!="ms")return false;
        if(!std::isfinite(value)||value<1.0||value>double(maxMs)||value>double(std::numeric_limits<uint32_t>::max()))return false;
        output=static_cast<uint32_t>(value);return true;
    }

    void updateTimerView() {
        timerEntries.clear();timerEntries.reserve(timers.size());
        for(const auto& timer:timers)timerEntries.push_back({timer.id,timer.repeating,timer.cycle,timer.periodMs,timer.cycleEvery});
    }
    std::vector<Timer> timers;

    // Registration and cancellation run only on the Lua worker. Removing a
    // running callback is safe: its closure stays on the Lua stack until it
    // returns, while its registry reference can be released immediately.
    bool cancelTimer(lua_State* state,uint64_t id,bool cycle) {
        const auto found=std::find_if(timers.begin(),timers.end(),[id,cycle](const Timer& timer){return timer.id==id&&timer.cycle==cycle;});
        if(found==timers.end())return false;
        luaL_unref(state,LUA_REGISTRYINDEX,found->functionRef);
        timers.erase(found);
        std::lock_guard<std::mutex> lock(mutex);updateTimerView();condition.notify_all();
        return true;
    }

    void openSafeLibraries(lua_State* state) {
        luaL_requiref(state,LUA_GNAME,luaopen_base,1);lua_pop(state,1);
        luaL_requiref(state,LUA_TABLIBNAME,luaopen_table,1);lua_pop(state,1);
        luaL_requiref(state,LUA_STRLIBNAME,luaopen_string,1);lua_pop(state,1);
        luaL_requiref(state,LUA_MATHLIBNAME,luaopen_math,1);lua_pop(state,1);
        luaL_requiref(state,LUA_UTF8LIBNAME,luaopen_utf8,1);lua_pop(state,1);
        luaL_requiref(state,LUA_COLIBNAME,luaopen_coroutine,1);lua_pop(state,1);
        lua_pushnil(state);lua_setglobal(state,"dofile");lua_pushnil(state);lua_setglobal(state,"loadfile");lua_pushnil(state);lua_setglobal(state,"load");
    }

    void run() {
        lua_State* L=lua_newstate(&allocator,this);
        if(!L) {
            const std::string message="Lua memory limit is too small.";
            log(LogLevel::Error,message);setState(Status::Error,message);return;
        }
        lua=L;
        const auto finish=[&](Status finalState,const std::string& message=std::string{}) {
            // Timer references belong to this Lua state. Clear them on every
            // exit, including a top-level error after registering a callback,
            // so restarting the runtime cannot reuse stale registry entries.
            for(auto& timer:timers)if(timer.functionRef!=LUA_NOREF)luaL_unref(L,LUA_REGISTRYINDEX,timer.functionRef);
            timers.clear();{std::lock_guard<std::mutex> lock(mutex);updateTimerView();}
            lua_close(L);lua=nullptr;setState(finalState,message);
        };
        Impl* self=this;std::memcpy(lua_getextraspace(L),&self,sizeof(self));openSafeLibraries(L);registerApi(L);
        lua_sethook(L,&instructionHook,LUA_MASKCOUNT,1000);
        std::string sourceCopy;std::string nameCopy;{std::lock_guard<std::mutex> lock(mutex);sourceCopy=source;nameCopy=name;state=Status::Running;error.clear();}
        if(luaL_loadbufferx(L,sourceCopy.data(),sourceCopy.size(),nameCopy.c_str(),"t")!=LUA_OK) {
            const std::string message=lua_tostring(L,-1)?lua_tostring(L,-1):"Lua parse error";lua_pop(L,1);log(LogLevel::Error,message);finish(Status::Error,message);return;
        }
        executionInstructionCount=0;
        if(lua_pcall(L,0,LUA_MULTRET,0)!=LUA_OK) {
            const std::string message=lua_tostring(L,-1)?lua_tostring(L,-1):"Lua runtime error";lua_pop(L,1);log(LogLevel::Error,message);finish(Status::Error,message);return;
        }
        eventLoop(L);
        finish(Status::Stopped);
    }

    void eventLoop(lua_State* L) {
        while(true) {
            {std::lock_guard<std::mutex> lock(mutex);if(stopRequested.load())break;state=Status::Running;}
            // Only the Lua worker can register or cancel callbacks. Once its
            // last task is cancelled or its last one-shot callback returns,
            // no future work can wake this loop and the script is complete.
            // A callback that schedules another timer leaves it in the list.
            if(timers.empty())break;
            const auto now=Clock::now();size_t selected=timers.size();auto next=now+std::chrono::hours(24);
            Snapshot snapshotCopy;
            {std::lock_guard<std::mutex> lock(mutex);snapshotCopy=liveSnapshot;}
            for(size_t n=0;n<timers.size();++n) {
                auto& candidate=timers[n];
                // A reconnect resets the host cycle counter. Re-anchor the
                // callback to the new generation instead of waiting for the
                // old counter value to come back around.
                if(candidate.cycle&&snapshotCopy.cycles<candidate.lastCycles)candidate.lastCycles=snapshotCopy.cycles;
                const bool cycleDue=candidate.cycle&&snapshotCopy.cycles>candidate.lastCycles&&snapshotCopy.cycles-candidate.lastCycles>=candidate.cycleEvery;
                if((candidate.cycle&&cycleDue)||(!candidate.cycle&&candidate.next<=now)){selected=n;break;}
                if(!candidate.cycle&&candidate.next<next)next=candidate.next;
            }
            if(selected==timers.size()){
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait_until(lock,next,[this,next]{
                    if(stopRequested.load())return true;
                    for(const auto& candidate:timers) {
                        if(candidate.cycle&&liveSnapshot.cycles>candidate.lastCycles&&liveSnapshot.cycles-candidate.lastCycles>=candidate.cycleEvery)return true;
                    }
                    return Clock::now()>=next;
                });
                continue;
            }
            // A callback may erase itself or register work that reallocates
            // the vector. Keep values, not a reference into that vector.
            const Timer timer=timers[selected];
            if(timer.cycle)timers[selected].lastCycles=snapshotCopy.cycles;
            else timers[selected].next=Clock::now()+std::chrono::milliseconds(timer.periodMs);
            lua_rawgeti(L,LUA_REGISTRYINDEX,timer.functionRef);
            if(timer.cycle)pushSnapshot(L,snapshotCopy);
            const int arguments=timer.cycle?1:0;
            ++callbackCount;
            executionInstructionCount=0;
            if(lua_pcall(L,arguments,0,0)!=LUA_OK){const std::string message=lua_tostring(L,-1)?lua_tostring(L,-1):"Timer callback failed";lua_pop(L,1);log(LogLevel::Error,message);}
            // A self-cancelled one-shot has already released its reference.
            // Looking up its ID avoids releasing a new task's reused slot.
            if(!timer.repeating)cancelTimer(L,timer.id,timer.cycle);
        }
    }

    static void registerApi(lua_State*);
    static void pushSnapshot(lua_State*,const Snapshot&);
};

namespace {
enum class SignalMode { Signal, Register, Sdo };
struct BusUserdata { Runtime::Impl* runtime=nullptr;Connection connection; };
struct SignalUserdata { Runtime::Impl* runtime=nullptr;SignalMode mode=SignalMode::Signal;int slave=0;uint16_t address=0,index=0;uint8_t subindex=0;std::string name; };

Runtime::Impl* impl(lua_State* state) { return Runtime::Impl::from(state); }
BusUserdata* bus(lua_State* state) { return static_cast<BusUserdata*>(luaL_checkudata(state,1,"niceecatm.bus")); }
SignalUserdata* signal(lua_State* state) { return static_cast<SignalUserdata*>(luaL_checkudata(state,1,"niceecatm.signal")); }
int busGc(lua_State* state) { auto* value=static_cast<BusUserdata*>(luaL_checkudata(state,1,"niceecatm.bus"));value->~BusUserdata();return 0; }
int signalGc(lua_State* state) { auto* value=static_cast<SignalUserdata*>(luaL_checkudata(state,1,"niceecatm.signal"));value->~SignalUserdata();return 0; }
std::string luaText(lua_State* state,int index) { size_t n=0;const char* text=luaL_checklstring(state,index,&n);return std::string(text,n); }
bool parseState(lua_State* state,int index,AlState& output) {
    std::string text=luaText(state,index);std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return char(std::toupper(c));});
    if(text=="INIT")output=AlState::Init;else if(text=="PRE-OP"||text=="PREOP")output=AlState::PreOp;else if(text=="SAFE-OP"||text=="SAFEOP")output=AlState::SafeOp;else if(text=="OP")output=AlState::Op;else return false;return true;
}
int pushHostResult(lua_State* state,HostResult result) {
    if(!result.ok)return luaL_error(state,"%s",result.message.empty()?"Host request failed":result.message.c_str());
    if(!result.data.empty()){lua_pushstring(state,hexBytes(result.data).c_str());return 1;}
    if(!result.message.empty()){lua_pushlstring(state,result.message.data(),result.message.size());return 1;}
    lua_pushboolean(state,1);return 1;
}
int logFunction(lua_State* state) { auto* self=impl(state);self->log(LogLevel::Info,luaText(state,1));return 0; }
int logWarningFunction(lua_State* state) { auto* self=impl(state);self->log(LogLevel::Warning,luaText(state,1));return 0; }
int logErrorFunction(lua_State* state) { auto* self=impl(state);self->log(LogLevel::Error,luaText(state,1));return 0; }

int openFunction(lua_State* state) {
    auto* self=impl(state);auto* result=static_cast<BusUserdata*>(lua_newuserdatauv(state,sizeof(BusUserdata),0));new(result) BusUserdata;result->runtime=self;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        result->connection=self->liveSnapshot.connection;
    }
    if(lua_istable(state,1)) {
        lua_getfield(state,1,"simulation");if(lua_isboolean(state,-1))result->connection.mode=lua_toboolean(state,-1)?ConnectionMode::Simulation:ConnectionMode::Hardware;lua_pop(state,1);
        lua_getfield(state,1,"redundant");if(lua_isboolean(state,-1))result->connection.redundant=lua_toboolean(state,-1);lua_pop(state,1);
        lua_getfield(state,1,"overlap");if(lua_isboolean(state,-1))result->connection.overlap=lua_toboolean(state,-1);lua_pop(state,1);
        lua_getfield(state,1,"primary");if(lua_isstring(state,-1))result->connection.primary=luaText(state,-1);lua_pop(state,1);
        lua_getfield(state,1,"interface");if(lua_isstring(state,-1))result->connection.primary=luaText(state,-1);lua_pop(state,1);
        lua_getfield(state,1,"secondary");if(lua_isstring(state,-1))result->connection.secondary=luaText(state,-1);lua_pop(state,1);
        lua_getfield(state,1,"cycle_us");if(lua_isinteger(state,-1))result->connection.cycleUs=int(lua_tointeger(state,-1));lua_pop(state,1);
    }
    luaL_getmetatable(state,"niceecatm.bus");lua_setmetatable(state,-2);return 1;
}
int currentFunction(lua_State* state) { return openFunction(state); }
int statusFunction(lua_State* state) {
    auto* b=bus(state);Snapshot snapshot;
    { std::lock_guard<std::mutex> lock(b->runtime->mutex);snapshot=b->runtime->liveSnapshot; }
    lua_createtable(state,0,12);
    lua_pushstring(state,snapshot.connection.mode==ConnectionMode::Programming?"programming":snapshot.connection.mode==ConnectionMode::Simulation?"simulation":"hardware");lua_setfield(state,-2,"mode");
    lua_pushboolean(state,snapshot.connected);lua_setfield(state,-2,"connected");
    lua_pushboolean(state,snapshot.mapped);lua_setfield(state,-2,"mapped");
    lua_pushboolean(state,snapshot.running);lua_setfield(state,-2,"running");
    lua_pushboolean(state,snapshot.busy);lua_setfield(state,-2,"busy");
    lua_pushinteger(state,lua_Integer(snapshot.generation));lua_setfield(state,-2,"generation");
    lua_pushinteger(state,lua_Integer(snapshot.cycles));lua_setfield(state,-2,"cycles");
    lua_pushinteger(state,snapshot.wkc);lua_setfield(state,-2,"wkc");
    lua_pushinteger(state,snapshot.expectedWkc);lua_setfield(state,-2,"expected_wkc");
    lua_pushinteger(state,lua_Integer(snapshot.slaves.size()));lua_setfield(state,-2,"slaves");
    std::string alState=snapshot.slaves.empty()?"UNKNOWN":stateName(snapshot.slaves.front().state);
    for(const auto& slave:snapshot.slaves)if(std::string(stateName(slave.state))!=alState){alState="MIXED";break;}
    lua_pushlstring(state,alState.data(),alState.size());lua_setfield(state,-2,"state");
    lua_pushstring(state,snapshot.status.c_str());lua_setfield(state,-2,"status");
    return 1;
}
int signalsFunction(lua_State* state) {
    auto* b=bus(state);const int position=int(luaL_checkinteger(state,2));Snapshot snapshot;
    { std::lock_guard<std::mutex> lock(b->runtime->mutex);snapshot=b->runtime->liveSnapshot; }
    const auto found=std::find_if(snapshot.slaves.begin(),snapshot.slaves.end(),[position](const Slave& slave){return slave.position==position;});
    if(found==snapshot.slaves.end())return luaL_error(state,"signal target slave does not exist");
    lua_createtable(state,static_cast<int>(found->pdoEntries.size()),0);int outputIndex=1;
    for(const auto& entry:found->pdoEntries) {
        if(entry.padding)continue;
        const auto name=pdoEntryName(entry,false);if(name.empty())continue;
        lua_createtable(state,0,7);
        lua_pushlstring(state,name.data(),name.size());lua_setfield(state,-2,"name");
        lua_pushboolean(state,entry.input);lua_setfield(state,-2,"input");
        lua_pushlstring(state,entry.type.data(),entry.type.size());lua_setfield(state,-2,"type");
        lua_pushinteger(state,entry.bitOffset);lua_setfield(state,-2,"bit_offset");
        lua_pushinteger(state,entry.bitLength);lua_setfield(state,-2,"bit_length");
        lua_pushinteger(state,entry.index);lua_setfield(state,-2,"index");
        lua_pushinteger(state,entry.subindex);lua_setfield(state,-2,"subindex");
        lua_rawseti(state,-2,outputIndex++);
    }
    return 1;
}
void pushAdapters(lua_State* state,const Snapshot& snapshot) {
    lua_createtable(state,static_cast<int>(snapshot.adapters.size()),0);int index=1;
    for(const auto& adapter:snapshot.adapters) {
        lua_createtable(state,0,2);lua_pushlstring(state,adapter.name.data(),adapter.name.size());lua_setfield(state,-2,"name");
        lua_pushlstring(state,adapter.description.data(),adapter.description.size());lua_setfield(state,-2,"description");lua_rawseti(state,-2,index++);
    }
}
void pushDevices(lua_State* state,const Snapshot& snapshot) {
    lua_createtable(state,static_cast<int>(snapshot.slaves.size()),0);int index=1;
    for(const auto& slave:snapshot.slaves) {
        lua_createtable(state,0,4);lua_pushinteger(state,slave.position);lua_setfield(state,-2,"position");
        lua_pushlstring(state,slave.name.data(),slave.name.size());lua_setfield(state,-2,"name");
        lua_pushstring(state,stateName(slave.state));lua_setfield(state,-2,"state");lua_pushboolean(state,slave.dc);lua_setfield(state,-2,"dc");
        lua_rawseti(state,-2,index++);
    }
}
int adaptersFunction(lua_State* state) {
    auto* self=impl(state);
    // Host enumeration returns its own metadata. It must not switch the
    // selected operating mode or disconnect an existing workbench bus.
    ScriptRequest req;req.kind=RequestKind::RefreshAdapters;req.connection.mode=ConnectionMode::Hardware;
    const auto result=self->perform(req);
    if(!result.ok)return luaL_error(state,"%s",result.message.empty()?"Adapter enumeration failed":result.message.c_str());
    Snapshot snapshot;{std::lock_guard<std::mutex> lock(self->mutex);snapshot=self->liveSnapshot;}
    if(result.adapters)snapshot.adapters=*result.adapters;
    pushAdapters(state,snapshot);return 1;
}
int scanFunction(lua_State* state) {
    auto* b=bus(state);ScriptRequest request;request.kind=RequestKind::Scan;request.connection=b->connection;const auto result=b->runtime->perform(request);
    if(!result.ok)return luaL_error(state,"%s",result.message.empty()?"EtherCAT scan failed":result.message.c_str());
    Snapshot snapshot;{std::lock_guard<std::mutex> lock(b->runtime->mutex);snapshot=b->runtime->liveSnapshot;}pushDevices(state,snapshot);return 1;
}
int disconnectFunction(lua_State* state) { auto* b=bus(state);ScriptRequest request;request.kind=RequestKind::Disconnect;return pushHostResult(state,b->runtime->perform(request)); }
int startFunction(lua_State* state) { auto* b=bus(state);ScriptRequest request;request.kind=RequestKind::Start;return pushHostResult(state,b->runtime->perform(request)); }
int stopFunction(lua_State* state) { auto* b=bus(state);ScriptRequest request;request.kind=RequestKind::Stop;return pushHostResult(state,b->runtime->perform(request)); }
int cycleFunction(lua_State* state) { auto* b=bus(state);ScriptRequest request;request.kind=RequestKind::SetCycle;request.connection.cycleUs=int(luaL_checkinteger(state,2));return pushHostResult(state,b->runtime->perform(request)); }
int stateFunction(lua_State* state) {
    auto* b=bus(state);ScriptRequest request;request.kind=RequestKind::State;
    if(lua_isstring(state,2)) {
        const auto target=luaText(state,2);
        if(target=="all"||target=="ALL"||target=="selected")request.slave=0;
        else request.slave=int(luaL_checkinteger(state,2));
    } else request.slave=int(luaL_checkinteger(state,2));
    if(!parseState(state,3,request.state))return luaL_error(state,"unknown EtherCAT state");
    return pushHostResult(state,b->runtime->perform(request));
}
int signalFunction(lua_State* state) {
    auto* b=bus(state);auto* result=static_cast<SignalUserdata*>(lua_newuserdatauv(state,sizeof(SignalUserdata),0));new(result) SignalUserdata;
    result->runtime=b->runtime;result->mode=SignalMode::Signal;result->slave=int(luaL_checkinteger(state,2));
    if(lua_gettop(state)>=4&&lua_isinteger(state,3)&&lua_isinteger(state,4)) {
        result->index=uint16_t(luaL_checkinteger(state,3));result->subindex=uint8_t(luaL_checkinteger(state,4));
    } else result->name=luaText(state,3);
    luaL_getmetatable(state,"niceecatm.signal");lua_setmetatable(state,-2);return 1;
}
int registerFunction(lua_State* state) { auto* b=bus(state);auto* result=static_cast<SignalUserdata*>(lua_newuserdatauv(state,sizeof(SignalUserdata),0));new(result) SignalUserdata;result->runtime=b->runtime;result->mode=SignalMode::Register;result->slave=int(luaL_checkinteger(state,2));result->address=uint16_t(luaL_checkinteger(state,3));luaL_getmetatable(state,"niceecatm.signal");lua_setmetatable(state,-2);return 1; }
int sdoFunction(lua_State* state) { auto* b=bus(state);auto* result=static_cast<SignalUserdata*>(lua_newuserdatauv(state,sizeof(SignalUserdata),0));new(result) SignalUserdata;result->runtime=b->runtime;result->mode=SignalMode::Sdo;result->slave=int(luaL_checkinteger(state,2));result->index=uint16_t(luaL_checkinteger(state,3));result->subindex=uint8_t(luaL_checkinteger(state,4));luaL_getmetatable(state,"niceecatm.signal");lua_setmetatable(state,-2);return 1; }

int signalRead(lua_State* state) {
    auto* s=signal(state);ScriptRequest request;request.slave=s->slave;
    if(s->mode==SignalMode::Signal){request.kind=RequestKind::ReadSignal;request.signal=s->name;request.index=s->index;request.subindex=s->subindex;}
    else if(s->mode==SignalMode::Register){request.kind=RequestKind::ReadRegister;request.address=s->address;request.length=lua_gettop(state)>=2?int(luaL_checkinteger(state,2)):1;}
    else {request.kind=RequestKind::ReadSdo;request.index=s->index;request.subindex=s->subindex;request.length=lua_gettop(state)>=2?int(luaL_checkinteger(state,2)):1;}
    return pushHostResult(state,s->runtime->perform(request));
}
int signalWrite(lua_State* state) {
    auto* s=signal(state);ScriptRequest request;request.slave=s->slave;
    if(s->mode==SignalMode::Signal){
        request.kind=RequestKind::WriteSignal;request.signal=s->name;request.index=s->index;request.subindex=s->subindex;
        // lua_isnumber() also returns true for numeric strings.  A value
        // returned by signal:read() is intentionally exposed as a string so
        // that the decoded engineering representation is preserved; coercing
        // it through lua_tonumber() turns "0" into "0.000000", which the
        // unsigned integer encoder correctly rejects.  Preserve strings and
        // format actual Lua numbers without a spurious fractional suffix.
        if(lua_type(state,2)==LUA_TNUMBER) {
            if(lua_isinteger(state,2)) request.value=std::to_string(static_cast<long long>(lua_tointeger(state,2)));
            else {
                std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(17)<<lua_tonumber(state,2);request.value=text.str();
            }
        } else request.value=luaText(state,2);
    }
    else {std::string text=luaText(state,2);std::string error;if(!parseHex(text,request.data,error))return luaL_error(state,"%s",error.c_str());if(s->mode==SignalMode::Register){request.kind=RequestKind::WriteRegister;request.address=s->address;}else{request.kind=RequestKind::WriteSdo;request.index=s->index;request.subindex=s->subindex;}}
    return pushHostResult(state,s->runtime->perform(request));
}

int addTimer(lua_State* state,bool cycle,bool repeating) {
    auto* self=impl(state);if(!lua_isfunction(state,2))return luaL_error(state,"timer callback must be a function");
    uint32_t period=0;uint64_t cycleEvery=0;
    if(cycle) {
        int isInteger=0;const auto count=lua_tointegerx(state,1,&isInteger);
        if(!isInteger||count<1)return luaL_error(state,"cycle count must be a positive whole integer");
        cycleEvery=static_cast<uint64_t>(count);
    } else if(!Runtime::Impl::duration(state,1,self->options.maxTimerPeriodMs,period))return luaL_error(state,"invalid or excessive timer period");
    if(self->timers.size()>=self->options.maxTimers)return luaL_error(state,"timer limit exceeded");
    Runtime::Impl::Timer timer;timer.id=self->nextTimer++;timer.periodMs=period;timer.cycleEvery=cycleEvery;timer.repeating=repeating;timer.cycle=cycle;timer.next=Clock::now()+std::chrono::milliseconds(period);
    if(cycle) {
        std::lock_guard<std::mutex> lock(self->mutex);
        timer.lastCycles=self->liveSnapshot.cycles;
        lua_pushvalue(state,2);timer.functionRef=luaL_ref(state,LUA_REGISTRYINDEX);self->timers.push_back(timer);self->updateTimerView();self->condition.notify_all();
    } else {
        lua_pushvalue(state,2);timer.functionRef=luaL_ref(state,LUA_REGISTRYINDEX);self->timers.push_back(timer);{
            std::lock_guard<std::mutex> lock(self->mutex);self->updateTimerView();self->condition.notify_all();
        }
    }
    lua_pushinteger(state,lua_Integer(timer.id));return 1;
}
int timerAfter(lua_State* state){return addTimer(state,false,false);} int timerEvery(lua_State* state){return addTimer(state,false,true);} int cycleEvery(lua_State* state){return addTimer(state,true,true);}
int cancelTimer(lua_State* state,bool cycle) {
    int isInteger=0;const auto id=lua_tointegerx(state,1,&isInteger);
    if(lua_type(state,1)!=LUA_TNUMBER||!isInteger||id<1)return luaL_error(state,"task ID must be a positive whole integer");
    lua_pushboolean(state,impl(state)->cancelTimer(state,static_cast<uint64_t>(id),cycle));return 1;
}
int timerCancel(lua_State* state){return cancelTimer(state,false);} int cycleCancel(lua_State* state){return cancelTimer(state,true);}

void metatable(lua_State* state,const char* name,const luaL_Reg* methods) { luaL_newmetatable(state,name);lua_pushvalue(state,-1);lua_setfield(state,-2,"__index");luaL_setfuncs(state,methods,0);lua_pop(state,1); }
} // namespace

void Runtime::Impl::pushSnapshot(lua_State* state,const Snapshot& snapshot) {
    lua_createtable(state,0,8);lua_pushinteger(state,lua_Integer(snapshot.cycles));lua_setfield(state,-2,"cycles");lua_pushinteger(state,lua_Integer(snapshot.missed));lua_setfield(state,-2,"missed");lua_pushinteger(state,lua_Integer(snapshot.wkc));lua_setfield(state,-2,"wkc");lua_pushinteger(state,lua_Integer(snapshot.expectedWkc));lua_setfield(state,-2,"expected_wkc");lua_pushboolean(state,snapshot.connected);lua_setfield(state,-2,"connected");lua_pushboolean(state,snapshot.running);lua_setfield(state,-2,"running");lua_pushstring(state,snapshot.status.c_str());lua_setfield(state,-2,"status");
    lua_createtable(state,static_cast<int>(snapshot.slaves.size()),0);int index=1;
    for(const auto& slave:snapshot.slaves){lua_createtable(state,0,5);lua_pushinteger(state,slave.position);lua_setfield(state,-2,"position");lua_pushstring(state,slave.name.c_str());lua_setfield(state,-2,"name");lua_pushstring(state,stateName(slave.state));lua_setfield(state,-2,"state");const auto inputs=hexBytes(slave.inputs),outputs=hexBytes(slave.outputs);lua_pushlstring(state,inputs.data(),inputs.size());lua_setfield(state,-2,"inputs");lua_pushlstring(state,outputs.data(),outputs.size());lua_setfield(state,-2,"outputs");lua_rawseti(state,-2,index++);}
    lua_setfield(state,-2,"slaves");
}
void Runtime::Impl::registerApi(lua_State* state) {
    const luaL_Reg busMethods[]={{"scan",scanFunction},{"disconnect",disconnectFunction},{"start",startFunction},{"stop",stopFunction},{"state",stateFunction},{"status",statusFunction},{"signals",signalsFunction},{"cycle",cycleFunction},{"signal",signalFunction},{"register",registerFunction},{"sdo",sdoFunction},{"__gc",busGc},{nullptr,nullptr}};
    const luaL_Reg signalMethods[]={{"read",signalRead},{"write",signalWrite},{"__gc",signalGc},{nullptr,nullptr}};
    metatable(state,"niceecatm.bus",busMethods);metatable(state,"niceecatm.signal",signalMethods);
    lua_newtable(state);lua_pushcfunction(state,adaptersFunction);lua_setfield(state,-2,"adapters");lua_pushcfunction(state,openFunction);lua_setfield(state,-2,"open");lua_pushcfunction(state,currentFunction);lua_setfield(state,-2,"current");lua_setglobal(state,"ecat");
    lua_newtable(state);lua_pushcfunction(state,timerAfter);lua_setfield(state,-2,"after");lua_pushcfunction(state,timerEvery);lua_setfield(state,-2,"every");lua_pushcfunction(state,timerCancel);lua_setfield(state,-2,"cancel");lua_setglobal(state,"timer");
    lua_newtable(state);lua_pushcfunction(state,cycleEvery);lua_setfield(state,-2,"every");lua_pushcfunction(state,cycleCancel);lua_setfield(state,-2,"cancel");lua_setglobal(state,"cycle");
    lua_newtable(state);
    lua_pushcfunction(state,logFunction);lua_setfield(state,-2,"info");
    lua_pushcfunction(state,logWarningFunction);lua_setfield(state,-2,"warn");
    lua_pushcfunction(state,logErrorFunction);lua_setfield(state,-2,"error");
    lua_setglobal(state,"log");
    lua_pushcfunction(state,logFunction);lua_setglobal(state,"print");
}

Runtime::Runtime(HostCallbacks callbacks,RuntimeOptions options):impl_(std::make_unique<Impl>(std::move(callbacks),options)){}
Runtime::~Runtime()=default;
bool Runtime::load(std::string name,std::string source) {
    stop();auto& self=*impl_;std::lock_guard<std::mutex> lock(self.mutex);
    if(source.empty()){self.error="Script source is empty.";self.state=Status::Error;return false;}
    if(source.size()>self.options.maxSourceBytes){self.error="Script source exceeds the configured size limit.";self.state=Status::Error;return false;}
    self.name=std::move(name);self.source=std::move(source);self.loaded=true;self.error.clear();self.instructionCount=self.callbackCount=0;self.logEntries.clear();self.timerEntries.clear();self.state=Status::Ready;return true;
}
bool Runtime::start() {
    auto& self=*impl_;
    {
        std::lock_guard<std::mutex> lock(self.mutex);
        if(!self.loaded)return false;
        if(self.liveSnapshot.connection.mode==ConnectionMode::Programming) {
            self.error="Scripts are unavailable in Programming mode.";self.state=Status::Error;return false;
        }
        if(self.thread.joinable()&&(self.state==Status::Running||self.state==Status::Waiting))return false;
    }
    if(self.thread.joinable())self.thread.join();
    std::lock_guard<std::mutex> lock(self.mutex);
    self.stopRequested=false;self.error.clear();self.instructionCount=self.callbackCount=0;self.state=Status::Running;self.thread=std::thread([p=impl_.get()]{p->run();});return true;
}
void Runtime::stop(){impl_->stop();}
void Runtime::updateSnapshot(const Snapshot& snapshot){impl_->update(snapshot);}
void Runtime::complete(uint64_t request,HostResult result){impl_->complete(request,std::move(result));}
View Runtime::snapshot() const {auto& self=*impl_;std::lock_guard<std::mutex> lock(self.mutex);View result;result.status=self.state;result.name=self.name;result.error=self.error;result.instructionCount=self.instructionCount;result.callbackCount=self.callbackCount;result.timers=self.timerEntries;result.logs=self.logEntries;return result;}

} // namespace niceecatm::script

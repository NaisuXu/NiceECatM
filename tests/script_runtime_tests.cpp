// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#include "script/runtime.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace niceecatm::script;
using niceecatm::Snapshot;
using namespace std::chrono_literals;

namespace {
bool waitFor(Runtime& runtime,Status expected,std::chrono::milliseconds timeout=1500ms) {
    const auto end=std::chrono::steady_clock::now()+timeout;
    while(std::chrono::steady_clock::now()<end) {
        if(runtime.snapshot().status==expected)return true;
        std::this_thread::sleep_for(5ms);
    }
    return runtime.snapshot().status==expected;
}
bool waitForCallbacks(Runtime& runtime,uint64_t expected,std::chrono::milliseconds timeout=500ms) {
    const auto end=std::chrono::steady_clock::now()+timeout;
    while(std::chrono::steady_clock::now()<end) {
        if(runtime.snapshot().callbackCount>=expected)return true;
        std::this_thread::sleep_for(2ms);
    }
    return runtime.snapshot().callbackCount>=expected;
}
bool waitForLog(Runtime& runtime,const std::string& text,size_t expected=1,std::chrono::milliseconds timeout=1500ms) {
    const auto hasMessages=[&] {
        const auto view=runtime.snapshot();
        return static_cast<size_t>(std::count_if(view.logs.begin(),view.logs.end(),[&](const LogEvent& event){return event.message.find(text)!=std::string::npos;}))>=expected;
    };
    const auto end=std::chrono::steady_clock::now()+timeout;
    while(std::chrono::steady_clock::now()<end) {
        if(hasMessages())return true;
        std::this_thread::sleep_for(2ms);
    }
    return hasMessages();
}
bool hasErrors(const View& view) {
    return std::any_of(view.logs.begin(),view.logs.end(),[](const LogEvent& event){return event.level==LogLevel::Error;});
}
}

int main() {
    int checks=0;
    {
        Runtime runtime;Snapshot snapshot;snapshot.connection.mode=niceecatm::ConnectionMode::Programming;runtime.updateSnapshot(snapshot);
        if(!runtime.load("programming.lua","return 1")||runtime.start()||runtime.snapshot().error.empty())return 140;
        ++checks;
    }
    {
        HostCallbacks callbacks;callbacks.execute=[](const ScriptRequest& request,HostResult& result){
            if(request.kind!=RequestKind::RefreshAdapters)return false;
            result.ok=true;result.adapters=std::vector<niceecatm::Adapter>{{"wired0","Physical metadata"}};return true;
        };
        Runtime runtime(callbacks);Snapshot snapshot;snapshot.simulation=true;snapshot.connection.mode=niceecatm::ConnectionMode::Simulation;
        snapshot.adapters={{"sim0","Existing simulator"}};runtime.updateSnapshot(snapshot);
        if(!runtime.load("enumeration.lua","local a=ecat.adapters(); assert(#a==1 and a[1].name=='wired0'); assert(ecat.current():status().mode=='simulation')")||!runtime.start()||!waitFor(runtime,Status::Stopped))return 141;
        ++checks;
    }
    {
        RuntimeOptions options;options.maxInstructions=3000;options.maxMemoryBytes=2u*1024u*1024u;
        Runtime runtime({},options);
        if(!runtime.load("loop.lua","while true do end")||!runtime.start()||!waitFor(runtime,Status::Error))return 1;
        ++checks;runtime.stop();
    }
    {
        RuntimeOptions options;options.maxSourceBytes=8;
        Runtime runtime({},options);
        if(runtime.load("large.lua","return 'too large'"))return 2;
        ++checks;
    }
    {
        // VM creation fails before Lua can report an error. It still needs a
        // single sequenced event so the UI can relay failures from one stream.
        RuntimeOptions options;options.maxMemoryBytes=1;
        Runtime runtime({},options);
        if(!runtime.load("no_memory.lua","return 1")||!runtime.start()||!waitFor(runtime,Status::Error))return 119;
        const auto view=runtime.snapshot();
        if(view.error.empty()||view.logs.size()!=1||view.logs.front().level!=LogLevel::Error||
           view.logs.front().message!=view.error||view.logs.front().sequence==0)return 120;
        for(int poll=0;poll<5;++poll) {
            const auto unchanged=runtime.snapshot();
            if(unchanged.logs.size()!=1||unchanged.logs.front().sequence!=view.logs.front().sequence)return 121;
        }
        ++checks;
    }
    {
        // A rejected name lookup must not survive loading corrected source.
        // Running the original source again is a new failure, with a new event
        // sequence even when its text and script filename are identical.
        HostCallbacks callbacks;
        callbacks.execute=[](const ScriptRequest& request,HostResult& result) {
            if(request.kind!=RequestKind::ReadSignal)return false;
            if(!request.signal.empty())result={false,"Signal name is ambiguous; use bus:signal(slave, index, subindex).",{}};
            else result={request.slave==1&&request.index==0x3001&&request.subindex==1,"1",{}};
            return true;
        };
        Runtime runtime(callbacks,RuntimeOptions{});
        const std::string rejected="local value = ecat.current():signal(1, 'Input'):read()\nlog.info(value)";
        const std::string corrected="local value = ecat.current():signal(1, 0x3001, 1):read()\nlog.info('indexed read=' .. value)";
        if(!runtime.load("untitled.lua",rejected)||!runtime.start()||!waitFor(runtime,Status::Error))return 122;
        const auto failed=runtime.snapshot();
        if(failed.logs.size()!=1||failed.logs.front().level!=LogLevel::Error||
           failed.logs.front().message!=failed.error||failed.error.find("Signal name is ambiguous")==std::string::npos)return 123;
        if(!runtime.load("untitled.lua",corrected)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 124;
        const auto recovered=runtime.snapshot();
        if(!recovered.error.empty()||recovered.logs.size()!=1||recovered.logs.front().level!=LogLevel::Info||
           recovered.logs.front().message!="indexed read=1"||recovered.logs.front().sequence<=failed.logs.front().sequence)return 125;
        if(!runtime.load("untitled.lua",rejected)||!runtime.start()||!waitFor(runtime,Status::Error))return 126;
        const auto failedAgain=runtime.snapshot();
        if(failedAgain.logs.size()!=1||failedAgain.logs.front().level!=LogLevel::Error||
           failedAgain.error!=failed.error||failedAgain.logs.front().message!=failedAgain.error||
           failedAgain.logs.front().sequence<=recovered.logs.front().sequence)return 127;
        ++checks;
    }
    {
        RuntimeOptions options;options.maxTimers=1;options.maxTimerPeriodMs=100;
        Runtime runtime({},options);
        if(!runtime.load("timers.lua","timer.every(1,function() end); timer.every(1,function() end)" )||!runtime.start()||!waitFor(runtime,Status::Error))return 3;
        ++checks;runtime.stop();
    }
    {
        Runtime runtime({},RuntimeOptions{});
        if(!runtime.load("cancel.lua","timer.every(1000,function() end)")||!runtime.start())return 4;
        std::this_thread::sleep_for(15ms);runtime.stop();
        if(runtime.snapshot().status!=Status::Stopped)return 5;
        ++checks;
    }
    {
        // Stopping while a host request is pending is a normal lifecycle
        // action. The interrupted await must not become a script ERROR.
        std::atomic<bool> submitted=false;
        HostCallbacks callbacks;
        callbacks.submit=[&](const ScriptRequest&) {submitted=true;return uint64_t(1);};
        Runtime runtime(callbacks,RuntimeOptions{});
        if(!runtime.load("stop_waiting.lua","ecat.current():disconnect()")||!runtime.start())return 6;
        const auto deadline=std::chrono::steady_clock::now()+500ms;
        while(!submitted.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(2ms);
        if(!submitted.load())return 7;
        runtime.stop();
        const auto view=runtime.snapshot();
        if(view.status!=Status::Stopped||hasErrors(view)||!view.error.empty())return 8;
        ++checks;
    }
    {
        // Example 05 completes its delayed state/disconnect sequence from a
        // one-shot callback. No task remains after it returns, so the runtime
        // must publish Stopped without requiring the user to press Stop.
        std::vector<RequestKind> requests;
        HostCallbacks callbacks;
        callbacks.execute=[&](const ScriptRequest& request,HostResult& result) {
            requests.push_back(request.kind);result={true,"ok",{}};return true;
        };
        Runtime runtime(callbacks,RuntimeOptions{});
        const std::string source=R"LUA(
local bus = ecat.current()
timer.after(10, function()
    bus:state("all", "PRE-OP")
    bus:disconnect()
end)
)LUA";
        if(!runtime.load("one_shot_disconnect.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 30;
        auto view=runtime.snapshot();
        if(view.callbackCount!=1||!view.timers.empty()||requests.size()!=2||
           requests[0]!=RequestKind::State||requests[1]!=RequestKind::Disconnect)return 31;
        // The same loaded script can be run again after natural completion.
        if(!runtime.start()||!waitFor(runtime,Status::Stopped))return 32;
        view=runtime.snapshot();
        if(view.callbackCount!=1||!view.timers.empty()||requests.size()!=4)return 33;
        ++checks;
    }
    {
        // A one-shot callback may enqueue more work. Completion is decided
        // after the callback returns, so its child timer must still execute.
        Runtime runtime({},RuntimeOptions{});
        const std::string source=R"LUA(
local step = 0
timer.after(5, function()
    step = 1
    timer.after(5, function()
        assert(step == 1)
        log.info("chained timer complete")
    end)
end)
)LUA";
        if(!runtime.load("chained_timer.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 34;
        const auto view=runtime.snapshot();
        bool completed=false;for(const auto& event:view.logs)if(event.message=="chained timer complete")completed=true;
        if(!completed||view.callbackCount!=2||!view.timers.empty())return 35;
        ++checks;
    }
    {
        // Registering work before a top-level error must not leak callbacks
        // (or registry references from the closed Lua state) into the next run.
        Runtime runtime({},RuntimeOptions{});
        if(!runtime.load("timer_then_error.lua","timer.every(10,function() end); error('expected failure')")||
           !runtime.start()||!waitFor(runtime,Status::Error))return 36;
        if(!runtime.snapshot().timers.empty())return 37;
        if(!runtime.load("after_error.lua","log.info('fresh runtime')")||!runtime.start()||!waitFor(runtime,Status::Stopped))return 38;
        if(runtime.snapshot().callbackCount!=0||!runtime.snapshot().timers.empty())return 39;
        ++checks;
    }
    {
        Runtime runtime({},RuntimeOptions{});
        if(!runtime.load("repeating.lua","timer.every(5,function() end)")||!runtime.start()||!waitForCallbacks(runtime,2))return 40;
        if(runtime.snapshot().status!=Status::Running)return 41;
        runtime.stop();
        if(!runtime.snapshot().timers.empty())return 42;
        if(!runtime.start()||!waitForCallbacks(runtime,2))return 43;
        runtime.stop();++checks;
    }
    {
        // A long-running script may execute many individually bounded
        // callbacks. Their cumulative cost must not exhaust a single-call
        // budget or produce errors in otherwise healthy repeating work.
        RuntimeOptions options;options.maxInstructions=12;
        Runtime runtime({},options);
        const std::string source=R"LUA(
timer.every(1, function()
    local sum = 0
    for value = 1, 1500 do sum = sum + value end
    assert(sum == 1125750)
    log.info("finite callback complete")
end)
)LUA";
        if(!runtime.load("long_running.lua",source)||!runtime.start()||!waitForLog(runtime,"finite callback complete",20))return 44;
        const auto view=runtime.snapshot();runtime.stop();
        if(view.status!=Status::Running||view.instructionCount<=options.maxInstructions||
           std::any_of(view.logs.begin(),view.logs.end(),[](const LogEvent& event){return event.level==LogLevel::Error;}))return 45;
        ++checks;
    }
    {
        // Resetting the allowance between callbacks must not weaken the
        // guard against one callback that never returns, or prevent Stop.
        RuntimeOptions options;options.maxInstructions=10;
        Runtime runtime({},options);
        if(!runtime.load("runaway_callback.lua","timer.every(1, function() while true do end end)")||
           !runtime.start()||!waitForLog(runtime,"script instruction budget exceeded"))return 46;
        runtime.stop();
        const auto view=runtime.snapshot();
        if(view.status!=Status::Stopped||!view.timers.empty()||view.callbackCount<1)return 47;
        ++checks;
    }
    {
        Snapshot snapshot;snapshot.connected=true;snapshot.running=true;snapshot.cycles=0;
        Runtime runtime({},RuntimeOptions{});runtime.updateSnapshot(snapshot);
        const std::string source=R"LUA(
local seen = 0
cycle.every(2, function(frame)
    assert(frame.cycles % 2 == 0)
    seen = seen + 1
end)
)LUA";
        if(!runtime.load("cycle_count.lua",source)||!runtime.start())return 20;
        // Give the script thread time to register the callback. A cycle count
        // is captured at registration, so cycle 1 must not invoke it.
        std::this_thread::sleep_for(20ms);
        snapshot.cycles=1;runtime.updateSnapshot(snapshot);std::this_thread::sleep_for(20ms);
        if(runtime.snapshot().callbackCount!=0)return 21;
        snapshot.cycles=2;runtime.updateSnapshot(snapshot);
        if(!waitForCallbacks(runtime,1))return 22;
        snapshot.cycles=3;runtime.updateSnapshot(snapshot);std::this_thread::sleep_for(20ms);
        if(runtime.snapshot().callbackCount!=1)return 23;
        snapshot.cycles=4;runtime.updateSnapshot(snapshot);
        if(!waitForCallbacks(runtime,2))return 24;
        runtime.stop();++checks;
    }
    {
        // Cancellation is kind-specific, idempotent and validates IDs without
        // removing an unrelated live task. Canceling every task in the initial
        // chunk must finish naturally without invoking any callback.
        Runtime runtime({},RuntimeOptions{});
        const std::string source=R"LUA(
local once = timer.after(1, function() error("canceled one-shot ran") end)
local repeating = timer.every(1, function() error("canceled timer ran") end)
local cyclic = cycle.every(1, function() error("canceled cycle ran") end)
assert(math.type(once) == "integer" and once > 0)
assert(once ~= repeating and repeating ~= cyclic and once ~= cyclic)
assert(cycle.cancel(once) == false)
assert(cycle.cancel(repeating) == false)
assert(timer.cancel(cyclic) == false)
for _, cancel in ipairs({timer.cancel, cycle.cancel}) do
    assert(not pcall(cancel))
    for _, invalid in ipairs({false, 0, -1, 1.5, {}, "1", "not an ID", math.huge, 0/0}) do
        assert(not pcall(cancel, invalid))
    end
    assert(cancel(math.maxinteger) == false)
end
assert(timer.cancel(once) == true)
assert(timer.cancel(once) == false)
assert(timer.cancel(repeating) == true)
assert(cycle.cancel(cyclic) == true)
assert(cycle.cancel(cyclic) == false)
log.info("all initial tasks canceled")
)LUA";
        if(!runtime.load("cancel_initial.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 100;
        const auto view=runtime.snapshot();
        if(hasErrors(view)||view.callbackCount!=0||!view.timers.empty()||!waitForLog(runtime,"all initial tasks canceled"))return 101;
        ++checks;
    }
    {
        // A repeating callback can cancel another task and then itself. The
        // statements following cancellation still run, but there is no next
        // invocation and no need to press the script Stop button.
        Runtime runtime({},RuntimeOptions{});
        const std::string source=R"LUA(
local count = 0
local other = timer.every(10000, function() error("other timer ran") end)
local self
self = timer.every(1, function()
    count = count + 1
    if count == 3 then
        assert(timer.cancel(other) == true)
        assert(timer.cancel(self) == true)
        assert(timer.cancel(self) == false)
        log.info("self-canceled after three callbacks")
    end
end)
)LUA";
        if(!runtime.load("cancel_repeating.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 102;
        const auto view=runtime.snapshot();
        if(hasErrors(view)||view.callbackCount!=3||!view.timers.empty()||!waitForLog(runtime,"self-canceled after three callbacks"))return 103;
        ++checks;
    }
    {
        // Self-canceling a one-shot frees both its capacity and Lua registry
        // reference immediately. A replacement may reuse that reference; the
        // old callback's cleanup must not unref the replacement a second time.
        RuntimeOptions options;options.maxTimers=1;
        Runtime runtime({},options);
        const std::string source=R"LUA(
local first
first = timer.after(1, function()
    assert(timer.cancel(first) == true)
    local replacement
    replacement = timer.after(1, function()
        assert(timer.cancel(first) == false)
        assert(timer.cancel(replacement) == true)
        log.info("replacement callback survived")
    end)
    log.info("one-shot continued after cancellation")
end)
)LUA";
        if(!runtime.load("cancel_ref_reuse.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 104;
        const auto view=runtime.snapshot();
        if(hasErrors(view)||view.callbackCount!=2||!view.timers.empty()||
           !waitForLog(runtime,"replacement callback survived")||!waitForLog(runtime,"one-shot continued after cancellation"))return 105;
        ++checks;
    }
    {
        // A completed one-shot's ID is no longer live. This case is distinct
        // from repeating cancellation and must not cancel the next timer.
        Runtime runtime({},RuntimeOptions{});
        const std::string source=R"LUA(
local expired = timer.after(1, function() log.info("one-shot expired") end)
timer.after(1, function()
    assert(timer.cancel(expired) == false)
    log.info("expired ID rejected")
end)
)LUA";
        if(!runtime.load("cancel_expired.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 106;
        const auto view=runtime.snapshot();
        if(hasErrors(view)||view.callbackCount!=2||!view.timers.empty()||!waitForLog(runtime,"expired ID rejected"))return 107;
        ++checks;
    }
    {
        // Two cycle callbacks may coexist. The first callback cancels the
        // second before it can consume the next snapshot, then cancels itself.
        Snapshot snapshot;snapshot.connected=true;snapshot.running=true;snapshot.cycles=0;
        Runtime runtime({},RuntimeOptions{});runtime.updateSnapshot(snapshot);
        const std::string source=R"LUA(
local first, second
local seenFirst, seenSecond = 0, 0
first = cycle.every(1, function(frame)
    seenFirst = seenFirst + 1
    if seenFirst == 2 then
        assert(frame.cycles == 2 and seenSecond == 1)
        assert(timer.cancel(first) == false)
        assert(cycle.cancel(second) == true)
        assert(cycle.cancel(first) == true)
        assert(cycle.cancel(first) == false)
        log.info("cycle cancellation complete")
    end
end)
second = cycle.every(1, function(frame)
    assert(frame.cycles == 1)
    seenSecond = seenSecond + 1
    log.info("second cycle callback ran")
end)
log.info("cycle callbacks registered")
)LUA";
        if(!runtime.load("cancel_cycles.lua",source)||!runtime.start()||!waitForLog(runtime,"cycle callbacks registered"))return 108;
        snapshot.cycles=1;runtime.updateSnapshot(snapshot);
        if(!waitForLog(runtime,"second cycle callback ran"))return 109;
        snapshot.cycles=2;runtime.updateSnapshot(snapshot);
        if(!waitFor(runtime,Status::Stopped))return 110;
        const auto view=runtime.snapshot();
        if(hasErrors(view)||view.callbackCount!=3||!view.timers.empty()||!waitForLog(runtime,"cycle cancellation complete"))return 111;
        // Later snapshots cannot reanimate canceled cycle registrations.
        snapshot.cycles=3;runtime.updateSnapshot(snapshot);
        if(runtime.snapshot().callbackCount!=3||runtime.snapshot().status!=Status::Stopped)return 112;
        ++checks;
    }
    {
        // A timer can cancel a cycle registration while no bus cycles are
        // arriving. Canceling the last cycle task must not leave the scheduler
        // waiting forever after the canceling one-shot has returned.
        Runtime runtime({},RuntimeOptions{});
        const std::string source=R"LUA(
local pending = cycle.every(1, function() error("inactive bus callback ran") end)
timer.after(1, function()
    assert(cycle.cancel(pending) == true)
    log.info("idle cycle task canceled")
end)
)LUA";
        if(!runtime.load("cancel_idle_cycle.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 113;
        const auto view=runtime.snapshot();
        if(hasErrors(view)||view.callbackCount!=1||!view.timers.empty()||!waitForLog(runtime,"idle cycle task canceled"))return 114;
        ++checks;
    }
    {
        // IDs remain distinct when one Runtime is reused, so a handle kept
        // from an earlier run cannot accidentally cancel a new registration.
        Runtime runtime({},RuntimeOptions{});
        if(!runtime.load("cancel_previous_run.lua","timer.every(10000, function() end); log.info('old task registered')")||
           !runtime.start()||!waitForLog(runtime,"old task registered"))return 115;
        const auto oldView=runtime.snapshot();
        if(oldView.timers.size()!=1)return 116;
        const auto oldId=oldView.timers.front().id;runtime.stop();
        const auto source="local stale = "+std::to_string(oldId)+R"LUA(
local current = timer.every(10000, function() error("new task ran") end)
assert(current ~= stale)
assert(timer.cancel(stale) == false)
assert(timer.cancel(current) == true)
log.info("previous run ID rejected")
)LUA";
        if(!runtime.load("cancel_new_run.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 117;
        const auto view=runtime.snapshot();
        if(hasErrors(view)||view.callbackCount!=0||!view.timers.empty()||!waitForLog(runtime,"previous run ID rejected"))return 118;
        ++checks;
    }
    {
        Runtime* active=nullptr;uint64_t next=1;
        HostCallbacks callbacks;
        callbacks.submit=[&](const ScriptRequest& request) {
            const auto id=next++;
            if(active)active->complete(id,{true,"ok",{}});
            (void)request;return id;
        };
        Runtime runtime(callbacks,RuntimeOptions{});active=&runtime;
        Snapshot snapshot;snapshot.connected=true;snapshot.simulation=true;
        snapshot.adapters.push_back({"sim0","Simulation"});
        snapshot.slaves.push_back({1,"TEST",0,0,0,0,2,2});
        niceecatm::PdoEntry entry;entry.input=true;entry.bitLength=1;entry.type="BOOL";entry.names={{0x0409,"Input"}};
        snapshot.slaves.front().pdoEntries.push_back(entry);
        runtime.updateSnapshot(snapshot);
        const std::string source=R"LUA(
local adapters = ecat.adapters()
assert(#adapters == 1 and adapters[1].name == "sim0")
local current = ecat.current()
local current_status = current:status()
assert(current_status.connected == true and current_status.running == false)
local bus = ecat.open({interface = adapters[1].name, simulation = true})
local listed = bus:signals(1)
assert(#listed == 1 and listed[1].name == "Input" and listed[1].input == true)
local devices = bus:scan()
assert(#devices == 1 and devices[1].position == 1)
bus:state("all", "PRE-OP")
log.info("api ok")
)LUA";
        if(!runtime.load("api.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 6;
        const auto view=runtime.snapshot();bool logged=false;for(const auto& event:view.logs)if(event.message=="api ok")logged=true;
        if(!logged)return 7;
        ++checks;
    }
    {
        // signal:read() returns the decoded value as a Lua string. Passing
        // that value straight into signal:write() must keep the decimal
        // representation intact; lua_isnumber() also accepts numeric strings
        // and would otherwise turn "45" into "45.000000".
        Runtime* active=nullptr;uint64_t next=1;std::vector<ScriptRequest> requests;
        HostCallbacks callbacks;
        callbacks.submit=[&](const ScriptRequest& request) {
            requests.push_back(request);
            const auto id=next++;
            if(active)active->complete(id,{true,request.kind==RequestKind::ReadSignal?"45":"ok",{}});
            return id;
        };
        Runtime runtime(callbacks,RuntimeOptions{});active=&runtime;
        const std::string source=R"LUA(
local bus = ecat.current()
local value = bus:signal(1, 0x3001, 1):read()
bus:signal(1, 0x3101, 1):write(value)
)LUA";
        if(!runtime.load("signal_roundtrip.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 8;
        if(requests.size()!=2||requests[0].kind!=RequestKind::ReadSignal||requests[1].kind!=RequestKind::WriteSignal||requests[1].value!="45")return 9;
        ++checks;
    }
    {
        // signal:read() returns the decoded engineering value as text.  A
        // numeric-looking text value must remain text when it is passed back
        // to signal:write(); otherwise lua_isnumber() would turn "0" into
        // "0.000000", which is invalid for an unsigned PDO integer.
        std::string written;
        HostCallbacks callbacks;
        callbacks.execute=[&](const ScriptRequest& request,HostResult& result) {
            if(request.kind==RequestKind::ReadSignal) {result={true,"0",{}};return true;}
            if(request.kind==RequestKind::WriteSignal) {written=request.value;result={true,"ok",{}};return true;}
            return false;
        };
        Runtime runtime(callbacks,RuntimeOptions{});
        Snapshot snapshot;snapshot.connected=true;snapshot.mapped=true;snapshot.simulation=true;
        snapshot.slaves.push_back({1,"TEST",2,2,0,0,8,8});
        runtime.updateSnapshot(snapshot);
        const std::string source=R"LUA(
local bus = ecat.current()
local input = bus:signal(1, 0x3001, 1)
local output = bus:signal(1, 0x3101, 1)
local value = input:read()
output:write(value)
)LUA";
        if(!runtime.load("roundtrip.lua",source)||!runtime.start()||!waitFor(runtime,Status::Stopped))return 8;
        if(written!="0")return 9;
        ++checks;
    }
    std::cout<<checks<<" script runtime checks passed.\n";
    return 0;
}

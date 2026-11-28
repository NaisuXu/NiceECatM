// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "model/types.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace niceecatm::script {

// These requests intentionally contain only value types.  The script worker
// submits them to a host (normally Controller) and never receives a SOEM
// object or calls a transport method directly.
enum class RequestKind {
    RefreshAdapters, Scan, Disconnect, State, SetCycle, Start, Stop,
    ReadRegister, WriteRegister, ReadSdo, WriteSdo, WriteOutput,
    ReadSignal, WriteSignal
};

struct ScriptRequest {
    RequestKind kind=RequestKind::Scan;
    Connection connection;
    int slave=0;
    AlState state=AlState::PreOp;
    uint16_t address=0, index=0;
    uint8_t subindex=0;
    int length=1, offset=0;
    std::string signal;
    std::string value;
    std::vector<uint8_t> data;
};

struct HostResult {
    bool ok=false;
    std::string message;
    std::vector<uint8_t> data;
    std::optional<std::vector<Adapter>> adapters{};
};

struct HostCallbacks {
    // Returning zero means that the host rejected the request.  Completion is
    // delivered later through Runtime::complete(), from any host thread.
    std::function<uint64_t(const ScriptRequest&)> submit;
    // Optional immediate path for snapshot-only queries (currently
    // ReadSignal). It is called on the script thread and must not touch SOEM.
    // Return true when result has been filled; false falls through to submit.
    std::function<bool(const ScriptRequest&,HostResult&)> execute;
    // Runs on the Lua worker after its last callback, including errors/stops.
    std::function<void()> finished;
};

enum class Status { Empty, Ready, Running, Waiting, Stopped, Error };
enum class LogLevel { Info, Warning, Error };

struct LogEvent {
    uint64_t sequence=0;
    std::string time;
    LogLevel level=LogLevel::Info;
    std::string message;
};

struct RuntimeOptions {
    size_t maxSourceBytes=256u*1024u;
    size_t maxMemoryBytes=8u*1024u*1024u;
    uint64_t maxInstructions=500000u;
    size_t maxTimers=64;
    uint32_t maxTimerPeriodMs=24u*60u*60u*1000u;
    uint32_t hostTimeoutMs=30000u;
    size_t maxLogEntries=500;
};

struct TimerView {
    uint64_t id=0;
    bool repeating=false, cycle=false;
    uint32_t periodMs=0;
    uint64_t cycleEvery=0;
};

struct View {
    Status status=Status::Empty;
    std::string name;
    std::string error;
    uint64_t instructionCount=0;
    uint64_t callbackCount=0;
    std::vector<TimerView> timers;
    std::vector<LogEvent> logs;
};

class Runtime final {
public:
    struct Impl;
    explicit Runtime(HostCallbacks callbacks={}, RuntimeOptions options={});
    ~Runtime();
    Runtime(const Runtime&)=delete;
    Runtime& operator=(const Runtime&)=delete;

    bool load(std::string name, std::string source);
    bool start();
    void stop();
    void updateSnapshot(const Snapshot& snapshot);
    void complete(uint64_t request, HostResult result);
    View snapshot() const;

private:
    std::unique_ptr<Impl> impl_;
};

using ScriptRuntime=Runtime;

} // namespace niceecatm::script


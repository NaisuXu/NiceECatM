// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "ethercat/backend.hpp"
#include <memory>
#include <string>
namespace niceecatm {
class Controller {
public:
    Controller();
    ~Controller();
    Controller(const Controller&)=delete;
    Controller& operator=(const Controller&)=delete;
    // Returns zero if the bounded queue is full. Generation zero targets current connection.
    uint64_t submit(Command command);
    // Marks a queued command canceled. Commands already owned by the I/O
    // worker may finish; the worker still publishes their result normally.
    bool cancel(uint64_t request);
    Snapshot snapshot() const;
    // Scripts may opt into a per-I/O-cycle snapshot stream.  The regular
    // snapshot remains throttled for the graphical workbench.
    void enableScriptSnapshots(bool enabled);
    Snapshot scriptSnapshot() const;
    // Adds a user-facing message to the same bounded log shown by the
    // workbench. This is used by script/runtime integrations that complete
    // outside the EtherCAT command queue.
    void appendLog(std::string message,bool error=false);
    // Clears the bounded workbench log and the copies published to scripts.
    void clearLogs();
    // Returns true once the requested command has completed. Results are kept
    // for a bounded history so a script task can wait for an operation without
    // polling the GUI's last-result field or touching the backend.
    bool result(uint64_t request, Result& value) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace niceecatm

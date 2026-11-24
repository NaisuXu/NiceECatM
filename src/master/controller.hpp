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
    // Queued commands are canceled before execution. Active EEPROM/FoE jobs
    // stop cooperatively after their current bounded transport transaction.
    bool cancel(uint64_t request);
    Snapshot snapshot() const;
    // Updates the set of stable PDO keys captured by the I/O owner. Selection
    // changes are asynchronous and do not perform transport operations.
    void setTrendSelection(std::vector<std::string> keys);
    // Scripts may opt into a per-I/O-cycle snapshot stream.  The regular
    // snapshot remains throttled for the graphical workbench.
    void enableScriptSnapshots(bool enabled);
    // GUI/runtime lifecycle gate, independent of the optional fast snapshots.
    bool setScriptRunning(bool running);
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
    // Includes queued/canceled-but-not-yet-drained and currently executing
    // commands. A missing bounded-history result need not mean still pending.
    bool pending(uint64_t request) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace niceecatm

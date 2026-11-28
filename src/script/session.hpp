// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "script/bridge.hpp"
#include "script/runtime.hpp"
#include <mutex>
#include <thread>

namespace niceecatm::script {
// Owns the Lua worker and its controller notifications for one workbench.
class Session {
public:
    explicit Session(Controller& controller);
    ~Session();
    bool start(std::string name,std::string source);
    void stop();
    View snapshot() const;
private:
    uint64_t submit(const ScriptRequest& request);
    void dispatchUpdates();
    void releaseController();

    Controller& controller_;
    Bridge bridge_;
    Runtime runtime_;
    std::thread dispatcher_;
    std::mutex requestMutex_;
    uint64_t pending_=0;
    bool stopping_=true;
    std::string startError_;
};
}
